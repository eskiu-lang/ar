// ar_depth.inc: depth from a model (ArOptions.depth_model): Depth Anything V2 through Core ML. Part of ar.m, which #includes it (one translation unit).

// --- depth from a model (ArOptions.depth_model): Depth Anything V2 (metric) as a compiled
// CoreML model, run on the picture a few times a second off the main thread. Its output is
// put back in the sensor's orientation, so it lines up with the picture as sceneDepth does.

static NSString *modelPath;
static VNCoreMLModel *depthModel;
static int modelLoading;
static dispatch_queue_t modelQueue;
static atomic_int modelBusy;
static os_unfair_lock modelLock = OS_UNFAIR_LOCK_INIT;
static float *modelDepth;
static float *modelWork;
static size_t modelSize;
static int modelW, modelH;
static int modelReady;
// ArOptions.depth_model_scale and depth_model_fps, and when the model last started
static float modelScale = 1.0f;
static int modelFps = 5;
static CFAbsoluteTime modelStarted;

// 1 when the device runs ARKit's world tracking, else 0.
int ar_native_supported(void) {
    return ARWorldTrackingConfiguration.isSupported ? 1 : 0;
}

// 1 when the device gives depth: a LiDAR sensor, or a depth model given (ArOptions.depth_model).
int ar_native_depth_supported(void) {
    if ([ARWorldTrackingConfiguration supportsFrameSemantics:ARFrameSemanticSceneDepth]) return 1;
    return modelPath != nil && [NSFileManager.defaultManager fileExistsAtPath:modelPath] ? 1 : 0;
}

void ar_native_depth_model_tuning(float scale, int fps) {
    modelScale = scale > 0.0f ? scale : 1.0f;
    modelFps = fps > 0 ? fps : 5;
}

void ar_native_depth_model(const char *path) {
    NSString *p = path ? [NSString stringWithUTF8String:path] : nil;
    // a relative path names a file in the app's bundle
    if (p && ![p isAbsolutePath]) {
        NSString *inBundle = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:p];
        if ([NSFileManager.defaultManager fileExistsAtPath:inBundle]) p = inBundle;
    }
    if (!p || [p isEqualToString:modelPath ?: @""]) return;
    modelPath = p;
    depthModel = nil;
    os_unfair_lock_lock(&modelLock);
    modelReady = 0;
    os_unfair_lock_unlock(&modelLock);
    if (!modelQueue)
        modelQueue = dispatch_queue_create("eskiu.ar.depth", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    modelLoading = 1;
    dispatch_async(modelQueue, ^{
        NSURL *url = [NSURL fileURLWithPath:p];
        NSError *error = nil;
        if (![p hasSuffix:@".mlmodelc"]) {
            url = [MLModel compileModelAtURL:url error:&error];
            if (!url) {
                NSLog(@"ar: depth model %@ does not compile: %@", p, error.localizedDescription);
                dispatch_async(dispatch_get_main_queue(), ^{ modelLoading = 0; });
                return;
            }
        }
        MLModelConfiguration *config = [MLModelConfiguration new];
        config.computeUnits = MLComputeUnitsAll;
        MLModel *ml = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
        VNCoreMLModel *vn = ml ? [VNCoreMLModel modelForMLModel:ml error:&error] : nil;
        if (!vn) NSLog(@"ar: depth model %@ does not load: %@", p, error.localizedDescription);
        dispatch_async(dispatch_get_main_queue(), ^{
            if ([p isEqualToString:modelPath]) depthModel = vn;
            modelLoading = 0;
        });
    });
}

// The model's output (a 1 x H x W or H x W float array) into modelWork in the sensor's
// orientation, from the picture turned by `orientation` for the model.
static int readModelOutput(MLMultiArray *a, CGImagePropertyOrientation orientation) {
    NSInteger n = a.shape.count;
    if (n < 2) return 0;
    NSInteger oh = a.shape[n - 2].integerValue, ow = a.shape[n - 1].integerValue;
    NSInteger rs = a.strides[n - 2].integerValue, cs = a.strides[n - 1].integerValue;
    if (oh <= 0 || ow <= 0 || oh > 4096 || ow > 4096) return 0;
    int turned = orientation == kCGImagePropertyOrientationRight || orientation == kCGImagePropertyOrientationLeft;
    int w = (int)(turned ? oh : ow), h = (int)(turned ? ow : oh);
    if (modelSize < (size_t)(w * h)) {
        free(modelWork);
        modelWork = malloc((size_t)(w * h) * sizeof(float));
        os_unfair_lock_lock(&modelLock);
        free(modelDepth);
        modelDepth = malloc((size_t)(w * h) * sizeof(float));
        modelReady = 0;
        os_unfair_lock_unlock(&modelLock);
        modelSize = (size_t)(w * h);
    }
    int f16 = 0;
    if (@available(iOS 16.0, *)) f16 = a.dataType == MLMultiArrayDataTypeFloat16;
    if (!f16 && a.dataType != MLMultiArrayDataTypeFloat32) return 0;
    const uint8_t *base = a.dataPointer;
    size_t elem = f16 ? 2 : 4;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // the sensor's pixel (x, y) in the turned picture the model saw
            NSInteger col, row;
            switch (orientation) {
                case kCGImagePropertyOrientationRight: col = h - 1 - y; row = x; break;
                case kCGImagePropertyOrientationLeft: col = y; row = w - 1 - x; break;
                case kCGImagePropertyOrientationDown: col = w - 1 - x; row = h - 1 - y; break;
                default: col = x; row = y; break;
            }
            const uint8_t *at = base + (row * rs + col * cs) * elem;
            float v;
            if (f16) {
                __fp16 hv;
                memcpy(&hv, at, 2);
                v = (float)hv;
            } else {
                memcpy(&v, at, 4);
            }
            modelWork[y * w + x] = isfinite(v) && v > 0.0f ? v : 0.0f;
        }
    }
    os_unfair_lock_lock(&modelLock);
    float *t = modelDepth;
    modelDepth = modelWork;
    modelWork = t;
    modelW = w;
    modelH = h;
    modelReady = 1;
    os_unfair_lock_unlock(&modelLock);
    return 1;
}

// Starts the model on the lent picture unless it is busy with another.
static void runModel(void) {
    if (!depthModel || !lent || atomic_load(&modelBusy)) return;
    // at most depth_model_fps runs a second, fewer as the device heats up (none when critical)
    int fps = modelFps;
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateFair: fps = fps < 3 ? fps : 3; break;
        case NSProcessInfoThermalStateSerious: fps = fps < 2 ? fps : 2; break;
        case NSProcessInfoThermalStateCritical: return;
        default: break;
    }
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (now - modelStarted < 1.0 / fps) return;
    modelStarted = now;
    atomic_store(&modelBusy, 1);
    CVPixelBufferRef image = CVPixelBufferRetain(lent.capturedImage);
    CGImagePropertyOrientation orientation;
    switch (imageRotation()) {
        case 90: orientation = kCGImagePropertyOrientationRight; break;
        case 180: orientation = kCGImagePropertyOrientationDown; break;
        case 270: orientation = kCGImagePropertyOrientationLeft; break;
        default: orientation = kCGImagePropertyOrientationUp; break;
    }
    VNCoreMLModel *model = depthModel;
    dispatch_async(modelQueue, ^{
        @autoreleasepool {
            VNCoreMLRequest *request = [[VNCoreMLRequest alloc] initWithModel:model];
            request.imageCropAndScaleOption = VNImageCropAndScaleOptionScaleFill;
            VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:image orientation:orientation options:@{}];
            NSError *error = nil;
            if ([handler performRequests:@[request] error:&error]) {
                for (VNObservation *o in request.results) {
                    if ([o isKindOfClass:VNCoreMLFeatureValueObservation.class]) {
                        MLMultiArray *a = ((VNCoreMLFeatureValueObservation *)o).featureValue.multiArrayValue;
                        if (a && readModelOutput(a, orientation)) break;
                    }
                }
            }
            CVPixelBufferRelease(image);
            atomic_store(&modelBusy, 0);
        }
    });
}

// The model's latest depth, copied into a buffer the frame lends (the model writes the
// other one meanwhile).
static float *lentModel;
static size_t lentModelSize;

static int lendModel(int64_t *info) {
    os_unfair_lock_lock(&modelLock);
    if (!modelReady) {
        os_unfair_lock_unlock(&modelLock);
        return 0;
    }
    size_t n = (size_t)(modelW * modelH);
    if (lentModelSize < n) {
        free(lentModel);
        lentModel = malloc(n * sizeof(float));
        lentModelSize = n;
    }
    memcpy(lentModel, modelDepth, n * sizeof(float));
    if (modelScale != 1.0f) {
        for (size_t i = 0; i < n; i++) lentModel[i] *= modelScale;
    }
    int w = modelW, h = modelH;
    os_unfair_lock_unlock(&modelLock);
    info[0] = w;
    info[1] = h;
    info[2] = 0;
    info[3] = (int64_t)(w * sizeof(float));
    info[4] = (int64_t)(intptr_t)lentModel;
    info[5] = (int64_t)(n * sizeof(float));
    info[6] = 1;
    return 1;
}

// True when this frame's depth comes from the model: asked for, and the model only or no
// sensor depth.
static int usesModel(void) {
    if (!(sessionFlags & 8) || !(sessionFlags & 512)) return 0;
    if (sessionFlags & 1024) return 1;
    return lent.sceneDepth == nil;
}

// The people in the picture (ARKit's person segmentation) over the depth info describes, in
// place, nearer wins.
static void mergePeople(int64_t *info) {
    CVPixelBufferRef est = lent.estimatedDepthData;
    if (!est || CVPixelBufferGetPixelFormatType(est) != kCVPixelFormatType_DepthFloat32) return;
    if (CVPixelBufferLockBaseAddress(est, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    size_t ew = CVPixelBufferGetWidth(est), eh = CVPixelBufferGetHeight(est);
    size_t erow = CVPixelBufferGetBytesPerRow(est);
    const uint8_t *eb = CVPixelBufferGetBaseAddress(est);
    size_t w = (size_t)info[0], h = (size_t)info[1];
    float *d = (float *)(intptr_t)info[4];
    for (size_t y = 0; y < h; y++) {
        const float *er = (const float *)(eb + (y * eh / h) * erow);
        for (size_t x = 0; x < w; x++) {
            float person = er[x * ew / w];
            if (person > 0.0f && (d[y * w + x] <= 0.0f || person < d[y * w + x])) d[y * w + x] = person;
        }
    }
    CVPixelBufferUnlockBaseAddress(est, kCVPixelBufferLock_ReadOnly);
}

int ar_native_depth(int64_t *info) {
    if (!lent) return 0;
    if (usesModel()) {
        runModel();
        if (!lendModel(info)) return 0;
        if (sessionFlags & 128) mergePeople(info);
        return 1;
    }
    if ((sessionFlags & 128) && lendPeople(info)) return 1;
    if (!(sessionFlags & 8)) return 0;
    if (!lentDepth) {
        CVPixelBufferRef depth = lent.sceneDepth.depthMap;
        if (!depth || CVPixelBufferGetPixelFormatType(depth) != kCVPixelFormatType_DepthFloat32 ||
            CVPixelBufferLockBaseAddress(depth, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
        lentDepth = CVPixelBufferRetain(depth);
    }
    size_t row = CVPixelBufferGetBytesPerRow(lentDepth);
    info[0] = (int64_t)CVPixelBufferGetWidth(lentDepth);
    info[1] = (int64_t)CVPixelBufferGetHeight(lentDepth);
    info[2] = 0;
    info[3] = (int64_t)row;
    info[4] = (int64_t)(intptr_t)CVPixelBufferGetBaseAddress(lentDepth);
    info[5] = (int64_t)(row * CVPixelBufferGetHeight(lentDepth));
    return 1;
}

// Raycasts along the ray against the targets asked for (existing plane geometry, estimated
// planes), nearest first.
int ar_native_hit_test(float *ray, int targets, double *out, int max_hits) {
    if (!session || !running) return 0;
    for (int k = 0; k < 6; k++) if (!isfinite(ray[k])) return 0;
    if (ray[3] * ray[3] + ray[4] * ray[4] + ray[5] * ray[5] < 1e-12f) return 0;
    simd_float3 origin = simd_make_float3(ray[0], ray[1], ray[2]);
    simd_float3 direction = simd_make_float3(ray[3], ray[4], ray[5]);
    NSMutableArray<ARRaycastResult *> *results = [NSMutableArray new];
    NSMutableArray<NSNumber *> *kinds = [NSMutableArray new];
    for (int kind = 1; kind <= 2; kind++) {
        if (!(targets & kind)) continue;
        ARRaycastTarget target = kind == 1 ? ARRaycastTargetExistingPlaneGeometry : ARRaycastTargetEstimatedPlane;
        ARRaycastQuery *query = [[ARRaycastQuery alloc] initWithOrigin:origin direction:direction
                                                    allowingTarget:target alignment:ARRaycastTargetAlignmentAny];
        for (ARRaycastResult *result in [session raycast:query]) {
            [results addObject:result];
            [kinds addObject:@(kind)];
        }
    }
    int n = (int)results.count;
    double distances[n > 0 ? n : 1];
    int order[n > 0 ? n : 1];
    for (int i = 0; i < n; i++) {
        simd_float4 at = results[i].worldTransform.columns[3];
        distances[i] = simd_distance(simd_make_float3(at.x, at.y, at.z), origin);
        order[i] = i;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && distances[order[j]] < distances[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    if (n > max_hits) n = max_hits;
    for (int i = 0; i < n; i++) {
        ARRaycastResult *result = results[order[i]];
        double *hit = &out[i * 19];
        hit[0] = distances[order[i]];
        hit[1] = kinds[order[i]].intValue;
        hit[2] = [result.anchor isKindOfClass:ARPlaneAnchor.class] ? (double)planeId(result.anchor.identifier) : 0;
        putMatrix(&hit[3], result.worldTransform);
    }
    return n;
}

double ar_native_anchor_add(float *pose, double plane) {
    (void)plane;
    if (!session || !running) return 0;
    simd_float4x4 m;
    for (int c = 0; c < 4; c++) m.columns[c] = simd_make_float4(pose[c * 4], pose[c * 4 + 1], pose[c * 4 + 2], pose[c * 4 + 3]);
    ARAnchor *anchor = [[ARAnchor alloc] initWithTransform:m];
    NSNumber *id = @(nextId++);
    anchorIds[anchor.identifier] = id;
    anchorsById[id] = anchor;
    [session addAnchor:anchor];
    return id.doubleValue;
}

void ar_native_anchor_remove(double id) {
    NSNumber *key = @((int64_t)id);
    ARAnchor *anchor = anchorsById[key];
    if (!anchor) return;
    [anchorsById removeObjectForKey:key];
    [anchorIds removeObjectForKey:anchor.identifier];
    [session removeAnchor:anchor];
}
