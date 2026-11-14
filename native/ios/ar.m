// <ar> on iOS: an ARSession with an ARWorldTrackingConfiguration, no view and no GL. The
// session's delegate (on the main queue) keeps the newest ARFrame and posts "frame" on the
// "ar_frame" channel when none is waiting; ar_native_frame_acquire lends it to Eskiu: the
// capturedImage (NV12, full range) by pointer with its base address locked read-only, the
// camera's pose and intrinsics, and while it is lent its planes, anchors, points, light and
// depth. Image targets become ARReferenceImages (named by their index), each validated
// before the session looks for it; found ones are ARImageAnchors. Status goes to the "ar"
// channel. The layouts are src/ar.esk's.
#import <ARKit/ARKit.h>

#define AR_MESH_INFO_COUNT 25
static int64_t planeId(NSUUID *identifier);
#import <os/lock.h>
#import <CoreML/CoreML.h>
#import <Vision/Vision.h>
#import <stdatomic.h>
#import "ui_native.h"

static void emit(const char *status) { ui_native_emit("ar", status); }

@interface EskiuArDelegate : NSObject <ARSessionDelegate>
@end

static ARSession *session;
static EskiuArDelegate *delegate;
static int sessionFlags;
static int sessionWidth, sessionHeight;
static bool running;

// The image targets: an ARReferenceImage (or NSNull when refused) and an ArImageState for
// each, how many ARKit follows at once, and a generation that tells a validation of an
// older set to stop.
static NSMutableArray *targets;
static NSMutableArray<NSNumber *> *targetStates;
static int trackedMax;
static int targetGeneration;

// The newest frame and whether "frame" was posted since the last acquire, under frameLock.
static os_unfair_lock frameLock = OS_UNFAIR_LOCK_INIT;
static ARFrame *latest;
static bool posted;

// The lent frame, its picture and depth (base addresses locked), and the planes and the
// app's anchors it holds, in the order Eskiu reads them.
static ARFrame *lent;
static CVPixelBufferRef lentImage;
static CVPixelBufferRef lentDepth;
static NSArray<ARPlaneAnchor *> *lentPlanes;
static NSArray<ARAnchor *> *lentAnchors;
static NSArray<ARImageAnchor *> *lentImages;
static NSArray *lentMeshes;

// Ids Eskiu sees: planes by their anchor's identifier, the app's anchors both ways.
static NSMutableDictionary<NSUUID *, NSNumber *> *planeIds;
static NSMutableDictionary<NSUUID *, NSNumber *> *anchorIds;
static NSMutableDictionary<NSNumber *, ARAnchor *> *anchorsById;
static NSMutableDictionary<NSUUID *, NSNumber *> *imageIds;
static int64_t nextId = 1;

@implementation EskiuArDelegate
- (void)session:(ARSession *)arSession didUpdateFrame:(ARFrame *)frame {
    os_unfair_lock_lock(&frameLock);
    latest = frame;
    bool post = !posted;
    posted = true;
    os_unfair_lock_unlock(&frameLock);
    if (post) ui_native_emit("ar_frame", "frame");
}

- (void)session:(ARSession *)arSession didFailWithError:(NSError *)error {
    running = false;
    emit(error.code == ARErrorCodeCameraUnauthorized ? "denied" :
         error.code == ARErrorCodeUnsupportedConfiguration ? "unsupported" : "failed");
}

- (void)sessionWasInterrupted:(ARSession *)arSession { emit("paused"); }

- (void)sessionInterruptionEnded:(ARSession *)arSession { if (running) emit("running"); }
@end

// How far to turn the camera picture (landscape, home button on the right) clockwise to be
// upright as the interface is shown.
static int imageRotation(void) {
    UIInterfaceOrientation orientation = UIInterfaceOrientationPortrait;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:UIWindowScene.class]) {
            orientation = ((UIWindowScene *)scene).interfaceOrientation;
            break;
        }
    }
    switch (orientation) {
        case UIInterfaceOrientationLandscapeRight: return 0;
        case UIInterfaceOrientationLandscapeLeft: return 180;
        case UIInterfaceOrientationPortraitUpsideDown: return 270;
        default: return 90;
    }
}

// The smallest video format whose picture holds width x height (landscape), else the
// largest there is.
static ARVideoFormat *videoFormat(int width, int height) {
    ARVideoFormat *best = nil;
    ARVideoFormat *largest = nil;
    for (ARVideoFormat *format in ARWorldTrackingConfiguration.supportedVideoFormats) {
        CGSize size = format.imageResolution;
        if (!largest || size.width * size.height > largest.imageResolution.width * largest.imageResolution.height) largest = format;
        if (size.width >= width && size.height >= height &&
            (!best || size.width * size.height < best.imageResolution.width * best.imageResolution.height)) best = format;
    }
    return best ? best : largest;
}

void ar_native_frame_release(void);

// Runs the session with the current flags, picture size and the image targets validated so
// far. A session that runs already keeps its world.
static void runConfiguration(void) {
    ARWorldTrackingConfiguration *config = [ARWorldTrackingConfiguration new];
    ARPlaneDetection planes = ARPlaneDetectionNone;
    if (sessionFlags & 1) planes |= ARPlaneDetectionHorizontal;
    if (sessionFlags & 2) planes |= ARPlaneDetectionVertical;
    config.planeDetection = planes;
    config.lightEstimationEnabled = (sessionFlags & (4 | 32)) != 0;
    ARFrameSemantics semantics = ARFrameSemanticNone;
    if ((sessionFlags & 8) && [ARWorldTrackingConfiguration supportsFrameSemantics:ARFrameSemanticSceneDepth]) {
        semantics |= ARFrameSemanticSceneDepth;
    }
    if ((sessionFlags & 128) && [ARWorldTrackingConfiguration supportsFrameSemantics:ARFrameSemanticPersonSegmentationWithDepth]) {
        semantics |= ARFrameSemanticPersonSegmentationWithDepth;
    }
    if (@available(iOS 13.4, *)) {
        if ((sessionFlags & 256) && [ARWorldTrackingConfiguration supportsSceneReconstruction:ARSceneReconstructionMesh]) {
            config.sceneReconstruction = ARSceneReconstructionMesh;
        }
    }
    config.frameSemantics = semantics;
    ARVideoFormat *format = videoFormat(sessionWidth, sessionHeight);
    if (format) config.videoFormat = format;
    if (sessionFlags & 64) {
        NSMutableSet<ARReferenceImage *> *ready = [NSMutableSet new];
        for (NSUInteger i = 0; i < targets.count; i++)
            if (targetStates[i].intValue == 1 && [targets[i] isKindOfClass:ARReferenceImage.class]) [ready addObject:targets[i]];
        config.detectionImages = ready;
        config.maximumNumberOfTrackedImages = MIN((NSInteger)trackedMax, (NSInteger)ready.count);
    }
    // A new session starts from scratch; new options on a running one keep the world.
    ARSessionRunOptions options = running ? 0 : ARSessionRunOptionResetTracking | ARSessionRunOptionRemoveExistingAnchors;
    if (!running) {
        [planeIds removeAllObjects];
        [anchorIds removeAllObjects];
        [anchorsById removeAllObjects];
        [imageIds removeAllObjects];
    }
    running = true;
    [session runWithConfiguration:config options:options];
}

// Bumped by each start and stop, so a start still queued when a stop comes does nothing.
static int startGeneration;

void ar_native_session(int on, int flags, int width, int height) {
    startGeneration++;
    if (!on) {
        ar_native_frame_release();
        [session pause];
        running = false;
        os_unfair_lock_lock(&frameLock);
        latest = nil;
        posted = false;
        os_unfair_lock_unlock(&frameLock);
        return;
    }
    if (!ARWorldTrackingConfiguration.isSupported) {
        emit("unsupported");
        return;
    }
    if (!session) {
        session = [ARSession new];
        delegate = [EskiuArDelegate new];
        session.delegate = delegate;
        planeIds = [NSMutableDictionary new];
        anchorIds = [NSMutableDictionary new];
        anchorsById = [NSMutableDictionary new];
        imageIds = [NSMutableDictionary new];
    }
    sessionFlags = flags;
    sessionWidth = width;
    sessionHeight = height;
    // Run on the next turn of the main queue, once the UI update that started the session is
    // applied: a screen it replaces gives its camera back then, and ARKit started while
    // another session holds the camera waits about nine seconds before it tries again.
    int generation = startGeneration;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (generation != startGeneration) return;
        runConfiguration();
        emit("running");
    });
}

void ar_native_images(int count, int tracked) {
    targetGeneration++;
    targets = [NSMutableArray new];
    targetStates = [NSMutableArray new];
    for (int i = 0; i < count; i++) {
        [targets addObject:NSNull.null];
        [targetStates addObject:@2];
    }
    trackedMax = tracked;
}

// A gray picture (copied) or the file at `path` as a CGImage; NULL when neither gives one.
static CGImageRef targetImage(const uint8_t *gray, int width, int height, const char *path) {
    if (gray && width > 0 && height > 0) {
        CFDataRef data = CFDataCreate(NULL, gray, (CFIndex)width * height);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
        CGImageRef image = CGImageCreate(width, height, 8, 8, width, space, (CGBitmapInfo)kCGImageAlphaNone, provider, NULL,
                                         false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        return image;
    }
    if (!path) return NULL;
    UIImage *file = [UIImage imageWithContentsOfFile:[NSString stringWithUTF8String:path]];
    return file.CGImage ? CGImageRetain(file.CGImage) : NULL;
}

// Makes target `index` an ARReferenceImage and validates it (ARKit refuses an image with too
// few features); when the last one of the set is validated, a running session looks for
// the valid ones.
void ar_native_image_add(int index, uint8_t *gray, int width, int height, const char *path, double physical_width) {
    if (index < 0 || index >= (int)targets.count) return;
    CGImageRef image = physical_width > 0 ? targetImage(gray, width, height, path) : NULL;
    if (!image) return;
    ARReferenceImage *reference = [[ARReferenceImage alloc] initWithCGImage:image orientation:kCGImagePropertyOrientationUp
                                                             physicalWidth:physical_width];
    CGImageRelease(image);
    reference.name = [NSString stringWithFormat:@"%d", index];
    targets[index] = reference;
    targetStates[index] = @0;
    int generation = targetGeneration;
    [reference validateWithCompletionHandler:^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != targetGeneration) return;
            targetStates[index] = error ? @2 : @1;
            for (NSNumber *state in targetStates)
                if (state.intValue == 0) return;
            if (running && (sessionFlags & 64)) runConfiguration();
        });
    }];
}

int ar_native_image_state(int index) {
    if (index < 0 || index >= (int)targetStates.count) return 2;
    return targetStates[index].intValue;
}

static void putMatrix(double *out, simd_float4x4 m) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) out[c * 4 + r] = m.columns[c][r];
}

static int trackingOf(ARCamera *camera) {
    switch (camera.trackingState) {
        case ARTrackingStateNormal: return 2;
        case ARTrackingStateLimited: return 1;
        default: return 0;
    }
}

static int reasonOf(ARCamera *camera) {
    if (camera.trackingState != ARTrackingStateLimited) return 0;
    switch (camera.trackingStateReason) {
        case ARTrackingStateReasonInitializing: return 1;
        case ARTrackingStateReasonExcessiveMotion: return 2;
        case ARTrackingStateReasonInsufficientFeatures: return 3;
        case ARTrackingStateReasonRelocalizing: return 5;
        default: return 0;
    }
}

int ar_native_frame_acquire(int64_t *image, double *camera) {
    ar_native_frame_release();
    os_unfair_lock_lock(&frameLock);
    ARFrame *frame = latest;
    posted = false;
    os_unfair_lock_unlock(&frameLock);
    if (!frame) return 0;
    CVPixelBufferRef buffer = frame.capturedImage;
    if (!buffer || CVPixelBufferGetPlaneCount(buffer) != 2 ||
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
    lent = frame;
    lentImage = CVPixelBufferRetain(buffer);
    for (int i = 0; i < 24; i++) image[i] = 0;
    image[0] = (int64_t)CVPixelBufferGetWidth(buffer);
    image[1] = (int64_t)CVPixelBufferGetHeight(buffer);
    image[2] = 0;
    image[3] = 2;
    image[4] = imageRotation();
    CFTypeRef matrix = CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, NULL);
    image[6] = matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2) ? 1 : 0;
    if (matrix) CFRelease(matrix);
    image[7] = CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    image[8] = (int64_t)(frame.timestamp * 1e9);
    for (int p = 0; p < 2; p++) {
        size_t row = CVPixelBufferGetBytesPerRowOfPlane(buffer, p);
        image[9 + p * 4] = (int64_t)(intptr_t)CVPixelBufferGetBaseAddressOfPlane(buffer, p);
        image[10 + p * 4] = (int64_t)row;
        image[11 + p * 4] = p == 0 ? 1 : 2;
        image[12 + p * 4] = (int64_t)(row * CVPixelBufferGetHeightOfPlane(buffer, p));
    }
    ARCamera *cam = frame.camera;
    camera[0] = trackingOf(cam);
    camera[1] = reasonOf(cam);
    putMatrix(&camera[2], cam.transform);
    simd_float3x3 k = cam.intrinsics;
    camera[18] = k.columns[0][0];
    camera[19] = k.columns[1][1];
    camera[20] = k.columns[2][0];
    camera[21] = k.columns[2][1];
    camera[22] = cam.imageResolution.width;
    camera[23] = cam.imageResolution.height;
    NSMutableArray<ARPlaneAnchor *> *planes = [NSMutableArray new];
    NSMutableArray<ARAnchor *> *anchors = [NSMutableArray new];
    NSMutableArray<ARImageAnchor *> *images = [NSMutableArray new];
    NSMutableArray *meshes = [NSMutableArray new];
    for (ARAnchor *anchor in frame.anchors) {
        if (@available(iOS 13.4, *)) {
            if ([anchor isKindOfClass:ARMeshAnchor.class]) {
                [meshes addObject:anchor];
                continue;
            }
        }
        if ([anchor isKindOfClass:ARPlaneAnchor.class]) [planes addObject:(ARPlaneAnchor *)anchor];
        else if ([anchor isKindOfClass:ARImageAnchor.class]) [images addObject:(ARImageAnchor *)anchor];
        else if (anchorIds[anchor.identifier]) [anchors addObject:anchor];
    }
    lentPlanes = planes;
    lentAnchors = anchors;
    lentImages = images;
    lentMeshes = meshes;
    return 1;
}

void ar_native_frame_release(void) {
    if (lentImage) {
        CVPixelBufferUnlockBaseAddress(lentImage, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(lentImage);
        lentImage = NULL;
    }
    if (lentDepth) {
        CVPixelBufferUnlockBaseAddress(lentDepth, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(lentDepth);
        lentDepth = NULL;
    }
    lent = nil;
    lentPlanes = nil;
    lentAnchors = nil;
    lentImages = nil;
    lentMeshes = nil;
}

int ar_native_plane_count(void) { return (int)lentPlanes.count; }

int ar_native_mesh_count(void) { return (int)lentMeshes.count; }

// A reconstruction mesh of the lent frame (LiDAR): id, pose, then the vertices, normals and
// triangles where ARKit keeps them (shared Metal buffers, read in place while lent): pointer,
// count and stride of the vertices, pointer and stride of the normals, pointer and count of
// the triangles and the bytes of an index.
void ar_native_mesh(int index, double *info) {
    for (int i = 0; i < AR_MESH_INFO_COUNT; i++) info[i] = 0;
    if (index < 0 || index >= (int)lentMeshes.count) return;
    if (@available(iOS 13.4, *)) {
        ARMeshAnchor *mesh = lentMeshes[index];
        ARMeshGeometry *g = mesh.geometry;
        info[0] = (double)planeId(mesh.identifier);
        putMatrix(&info[1], mesh.transform);
        info[17] = (double)(intptr_t)((uint8_t *)g.vertices.buffer.contents + g.vertices.offset);
        info[18] = g.vertices.count;
        info[19] = g.vertices.stride;
        info[20] = (double)(intptr_t)((uint8_t *)g.normals.buffer.contents + g.normals.offset);
        info[21] = g.normals.stride;
        info[22] = (double)(intptr_t)g.faces.buffer.contents;
        info[23] = g.faces.count;
        info[24] = g.faces.bytesPerIndex;
    }
}

static int64_t planeId(NSUUID *identifier) {
    NSNumber *id = planeIds[identifier];
    if (!id) {
        id = @(nextId++);
        planeIds[identifier] = id;
    }
    return id.longLongValue;
}

// The plane's pose is its anchor's moved to the center; the outline is taken about it.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
int ar_native_plane(int index, double *info, float *polygon, int max_points) {
    if (index < 0 || index >= (int)lentPlanes.count) return -1;
    ARPlaneAnchor *plane = lentPlanes[index];
    for (int i = 0; i < 23; i++) info[i] = 0;
    info[0] = (double)planeId(plane.identifier);
    int alignment = plane.alignment == ARPlaneAnchorAlignmentVertical ? 2 : 0;
    if (alignment == 0 && ARPlaneAnchor.classificationSupported && plane.classification == ARPlaneClassificationCeiling) alignment = 1;
    info[1] = alignment;
    info[2] = trackingOf(lent.camera) == 2 ? 2 : 1;
    simd_float3 center = plane.center;
    simd_float4x4 pose = plane.transform;
    pose.columns[3] = simd_mul(plane.transform, simd_make_float4(center, 1));
    putMatrix(&info[3], pose);
    info[19] = plane.extent.x;
    info[20] = plane.extent.z;
    // ARPlaneClassification's values are ArPlaneClass's
    if (ARPlaneAnchor.classificationSupported) info[22] = (double)plane.classification;
    ARPlaneGeometry *geometry = plane.geometry;
    int n = (int)geometry.boundaryVertexCount;
    if (n > max_points) n = max_points;
    const simd_float3 *vertices = geometry.boundaryVertices;
    for (int i = 0; i < n; i++) {
        polygon[i * 2] = vertices[i].x - center.x;
        polygon[i * 2 + 1] = vertices[i].z - center.z;
    }
    return n;
}
#pragma clang diagnostic pop

int ar_native_anchor_count(void) { return (int)lentAnchors.count; }

void ar_native_anchor(int index, double *info) {
    for (int i = 0; i < 18; i++) info[i] = 0;
    if (index < 0 || index >= (int)lentAnchors.count) return;
    ARAnchor *anchor = lentAnchors[index];
    info[0] = (double)anchorIds[anchor.identifier].longLongValue;
    info[1] = trackingOf(lent.camera);
    putMatrix(&info[2], anchor.transform);
}

int ar_native_tracked_image_count(void) { return (int)lentImages.count; }

// An image anchor: tracked now (full), or where it was last seen (last known). Its pose is
// the image's center, x to its right, z toward its bottom, y out of its face; the size is
// the reference image's, scaled as ARKit estimates it.
void ar_native_tracked_image(int index, double *info) {
    for (int i = 0; i < 22; i++) info[i] = 0;
    if (index < 0 || index >= (int)lentImages.count) return;
    ARImageAnchor *anchor = lentImages[index];
    NSNumber *id = imageIds[anchor.identifier];
    if (!id) {
        id = @(nextId++);
        imageIds[anchor.identifier] = id;
    }
    info[0] = id.doubleValue;
    info[1] = anchor.referenceImage.name ? anchor.referenceImage.name.intValue : -1;
    info[2] = anchor.isTracked ? trackingOf(lent.camera) : 1;
    info[3] = anchor.isTracked ? 1 : 2;
    putMatrix(&info[4], anchor.transform);
    CGSize size = anchor.referenceImage.physicalSize;
    CGFloat scale = anchor.estimatedScaleFactor > 0 ? anchor.estimatedScaleFactor : 1;
    info[20] = size.width * scale;
    info[21] = size.height * scale;
}

int ar_native_points(float *out, int max_points) {
    ARPointCloud *cloud = lent.rawFeaturePoints;
    if (!cloud) return 0;
    int n = (int)cloud.count;
    if (n > max_points) n = max_points;
    const simd_float3 *points = cloud.points;
    for (int i = 0; i < n; i++) {
        out[i * 4] = points[i].x;
        out[i * 4 + 1] = points[i].y;
        out[i * 4 + 2] = points[i].z;
        out[i * 4 + 3] = 1;
    }
    return n;
}

// ARKit's own values: the ambient intensity in lumens (1000 neutral) and the color
// temperature, source 1 (ARKit); Eskiu puts them on the package's scale (ar_light_read).
int ar_native_light(double *info) {
    ARLightEstimate *light = lent.lightEstimate;
    for (int i = 0; i < 42; i++) info[i] = 0;
    if (!light) return 0;
    info[0] = 1;
    info[1] = light.ambientIntensity;
    info[2] = light.ambientColorTemperature;
    info[41] = 1;
    return 1;
}

// The depth of the people in the picture (ARKit's person segmentation, 0 where nobody is)
// merged into the scene depth, nearer wins, or alone without LiDAR: in a buffer of ours.
static float *peopleDepth;
static size_t peopleDepthSize;

static int lendPeople(int64_t *info) {
    CVPixelBufferRef est = lent.estimatedDepthData;
    if (!est || CVPixelBufferGetPixelFormatType(est) != kCVPixelFormatType_DepthFloat32) return 0;
    if (CVPixelBufferLockBaseAddress(est, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
    size_t ew = CVPixelBufferGetWidth(est), eh = CVPixelBufferGetHeight(est);
    size_t erow = CVPixelBufferGetBytesPerRow(est);
    const uint8_t *eb = CVPixelBufferGetBaseAddress(est);
    CVPixelBufferRef scene = lent.sceneDepth.depthMap;
    int merge = scene && CVPixelBufferGetPixelFormatType(scene) == kCVPixelFormatType_DepthFloat32 &&
        CVPixelBufferLockBaseAddress(scene, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess;
    size_t w = merge ? CVPixelBufferGetWidth(scene) : ew, h = merge ? CVPixelBufferGetHeight(scene) : eh;
    if (peopleDepthSize < w * h) {
        free(peopleDepth);
        peopleDepth = malloc(w * h * sizeof(float));
        peopleDepthSize = w * h;
    }
    const uint8_t *sb = merge ? CVPixelBufferGetBaseAddress(scene) : NULL;
    size_t srow = merge ? CVPixelBufferGetBytesPerRow(scene) : 0;
    for (size_t y = 0; y < h; y++) {
        const float *er = (const float *)(eb + (y * eh / h) * erow);
        const float *sr = merge ? (const float *)(sb + y * srow) : NULL;
        for (size_t x = 0; x < w; x++) {
            float person = er[x * ew / w];
            float d = merge ? sr[x] : 0.0f;
            if (person > 0.0f && (d <= 0.0f || person < d)) d = person;
            peopleDepth[y * w + x] = d;
        }
    }
    if (merge) CVPixelBufferUnlockBaseAddress(scene, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferUnlockBaseAddress(est, kCVPixelBufferLock_ReadOnly);
    info[0] = (int64_t)w;
    info[1] = (int64_t)h;
    info[2] = 0;
    info[3] = (int64_t)(w * sizeof(float));
    info[4] = (int64_t)(intptr_t)peopleDepth;
    info[5] = (int64_t)(w * h * sizeof(float));
    return 1;
}

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

// ARCore's cloud anchors and Earth are not part of ARKit: these say so (ar_job: unknown).
double ar_native_cloud_host(double anchor, int ttl_days) { return 0; }
double ar_native_cloud_resolve(const char *cloud_id) { return 0; }
double ar_native_earth_anchor(double lat, double lng, double alt, float *quaternion) { return 0; }
double ar_native_earth_resolve(int rooftop, double lat, double lng, double alt, float *quaternion) { return 0; }
int ar_native_job(double job, double *info, char *text, int capacity) {
    info[0] = 0;
    info[1] = 0;
    if (capacity > 0) text[0] = 0;
    return 3;
}
int ar_native_earth(double *info) { return 0; }
int ar_native_semantics(int64_t *info) { return 0; }
double ar_native_vps_check(double lat, double lng) { return 0; }
