// <ar> on iOS: an ARSession with an ARWorldTrackingConfiguration, no view and no GL. The
// session's delegate (on the main queue) keeps the newest ARFrame and posts "frame" on the
// "ar_frame" channel when none is waiting; ar_native_frame_acquire lends it to Eskiu: the
// capturedImage (NV12, full range) by pointer with its base address locked read-only, the
// camera's pose and intrinsics, and while it is lent its planes, anchors, points, light and
// depth. Status goes to the "ar" channel. The layouts are src/ar.esk's.
#import <ARKit/ARKit.h>
#import <os/lock.h>
#import "ui_native.h"

static void emit(const char *status) { ui_native_emit("ar", status); }

@interface EskiuArDelegate : NSObject <ARSessionDelegate>
@end

static ARSession *session;
static EskiuArDelegate *delegate;
static int sessionFlags;
static bool running;

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

// Ids Eskiu sees: planes by their anchor's identifier, the app's anchors both ways.
static NSMutableDictionary<NSUUID *, NSNumber *> *planeIds;
static NSMutableDictionary<NSUUID *, NSNumber *> *anchorIds;
static NSMutableDictionary<NSNumber *, ARAnchor *> *anchorsById;
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

void ar_native_session(int on, int flags, int width, int height) {
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
    }
    ARWorldTrackingConfiguration *config = [ARWorldTrackingConfiguration new];
    ARPlaneDetection planes = ARPlaneDetectionNone;
    if (flags & 1) planes |= ARPlaneDetectionHorizontal;
    if (flags & 2) planes |= ARPlaneDetectionVertical;
    config.planeDetection = planes;
    config.lightEstimationEnabled = (flags & (4 | 32)) != 0;
    if ((flags & 8) && [ARWorldTrackingConfiguration supportsFrameSemantics:ARFrameSemanticSceneDepth]) {
        config.frameSemantics = ARFrameSemanticSceneDepth;
    }
    ARVideoFormat *format = videoFormat(width, height);
    if (format) config.videoFormat = format;
    // A new session starts from scratch; new options on a running one keep the world.
    ARSessionRunOptions options = running ? 0 : ARSessionRunOptionResetTracking | ARSessionRunOptionRemoveExistingAnchors;
    if (!running) {
        [planeIds removeAllObjects];
        [anchorIds removeAllObjects];
        [anchorsById removeAllObjects];
    }
    sessionFlags = flags;
    running = true;
    [session runWithConfiguration:config options:options];
    emit("running");
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
    for (ARAnchor *anchor in frame.anchors) {
        if ([anchor isKindOfClass:ARPlaneAnchor.class]) [planes addObject:(ARPlaneAnchor *)anchor];
        else if (anchorIds[anchor.identifier]) [anchors addObject:anchor];
    }
    lentPlanes = planes;
    lentAnchors = anchors;
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
}

int ar_native_plane_count(void) { return (int)lentPlanes.count; }

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
    for (int i = 0; i < 22; i++) info[i] = 0;
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

// Intensity 1.0 at ARKit's neutral 1000 lumens; the color is left to Eskiu, which computes
// it from the temperature.
int ar_native_light(double *info) {
    ARLightEstimate *light = lent.lightEstimate;
    for (int i = 0; i < 41; i++) info[i] = 0;
    if (!light) return 0;
    info[0] = 1;
    info[1] = light.ambientIntensity / 1000.0;
    info[2] = light.ambientColorTemperature;
    return 1;
}

int ar_native_depth(int64_t *info) {
    if (!lent) return 0;
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
