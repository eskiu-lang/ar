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

// ARCore for iOS (Google's Swift package: ARCoreGARSession, ARCoreCloudAnchors,
// ARCoreGeospatial, ARCoreSemantics), used when the app adds it ([ios.spm] in its ui.toml)
// for cloud anchors, Earth and scene semantics, as on Android. Without it those calls say
// they have nothing.
#if __has_include(<ARCoreGARSession/ARCoreGARSession.h>)
#import <ARCoreGARSession/ARCoreGARSession.h>
#define ESKIU_ARCORE 1
#if __has_include(<ARCoreCloudAnchors/ARCoreCloudAnchors.h>)
#import <ARCoreCloudAnchors/ARCoreCloudAnchors.h>
#define ESKIU_ARCORE_CLOUD 1
#endif
#if __has_include(<ARCoreGeospatial/ARCoreGeospatial.h>)
#import <ARCoreGeospatial/ARCoreGeospatial.h>
#import <CoreLocation/CoreLocation.h>
#define ESKIU_ARCORE_GEO 1
#endif
#if __has_include(<ARCoreSemantics/ARCoreSemantics.h>)
#import <ARCoreSemantics/ARCoreSemantics.h>
#define ESKIU_ARCORE_SEMANTICS 1
#endif
#endif
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
static NSArray<ARObjectAnchor *> *lentObjects;
// The object targets: an ARReferenceObject named by its index, or NSNull when it did not load.
static NSMutableArray *objectTargets;
static NSMutableDictionary<NSUUID *, NSNumber *> *objectIds;
static NSArray *lentMeshes;
static NSArray *lentBodies;

// Ids Eskiu sees: planes by their anchor's identifier, the app's anchors both ways.
static NSMutableDictionary<NSUUID *, NSNumber *> *planeIds;
static NSMutableDictionary<NSUUID *, NSNumber *> *anchorIds;
static NSMutableDictionary<NSNumber *, ARAnchor *> *anchorsById;
static NSMutableDictionary<NSUUID *, NSNumber *> *imageIds;
static int64_t nextId = 1;

#ifdef ESKIU_ARCORE
static GARSession *gar;
static GARFrame *garLatest;
static GARFrame *garLent;
static void garConfigure(int flags);
#endif

@implementation EskiuArDelegate
- (void)session:(ARSession *)arSession didUpdateFrame:(ARFrame *)frame {
#ifdef ESKIU_ARCORE
    GARFrame *g = gar ? [gar update:frame error:nil] : nil;
#endif
    os_unfair_lock_lock(&frameLock);
    latest = frame;
#ifdef ESKIU_ARCORE
    garLatest = g;
#endif
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
static ARVideoFormat *videoFormat(NSArray<ARVideoFormat *> *formats, int width, int height) {
    ARVideoFormat *best = nil;
    ARVideoFormat *largest = nil;
    for (ARVideoFormat *format in formats) {
        CGSize size = format.imageResolution;
        if (!largest || size.width * size.height > largest.imageResolution.width * largest.imageResolution.height) largest = format;
        if (size.width >= width && size.height >= height &&
            (!best || size.width * size.height < best.imageResolution.width * best.imageResolution.height)) best = format;
    }
    return best ? best : largest;
}

void ar_native_frame_release(void);

// The front camera's face tracking, from the eskiu-lang/face package (face_install), so the
// TrueDepth calls stay out of apps that do not use them (App Store guideline 2.5.1).
typedef struct {
    ARConfiguration *(*configuration)(int flags, int width, int height);
    int (*count)(ARFrame *frame);
    void (*face)(ARFrame *frame, int index, double *info);
} EskiuArFaceProvider;
static const EskiuArFaceProvider *faceProvider;

void eskiu_ar_set_face_provider(const EskiuArFaceProvider *provider) {
    faceProvider = provider;
}

// The image targets validated so far.
static NSSet<ARReferenceImage *> *readyImages(void) {
    NSMutableSet<ARReferenceImage *> *ready = [NSMutableSet new];
    for (NSUInteger i = 0; i < targets.count; i++)
        if (targetStates[i].intValue == 1 && [targets[i] isKindOfClass:ARReferenceImage.class]) [ready addObject:targets[i]];
    return ready;
}

// Runs config: a new session starts from scratch; new options on a running one keep the world.
static void runWith(ARConfiguration *config) {
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

// Runs the session with the current flags, picture size and the image targets validated so
// far: the front camera with face tracking, body tracking, or world tracking. False (with
// "unsupported" sent) when the device or the app cannot run what was asked.
static bool runConfiguration(void) {
    if (sessionFlags & 32768) {
        ARConfiguration *config = faceProvider ? faceProvider->configuration(sessionFlags, sessionWidth, sessionHeight) : nil;
        if (!config) {
            emit("unsupported");
            return false;
        }
        config.lightEstimationEnabled = (sessionFlags & (4 | 32)) != 0;
        runWith(config);
        return true;
    }
    if (sessionFlags & 65536) {
        if (@available(iOS 13.0, *)) {
            if (!ARBodyTrackingConfiguration.isSupported) {
                emit("unsupported");
                return false;
            }
            ARBodyTrackingConfiguration *config = [ARBodyTrackingConfiguration new];
            ARPlaneDetection planes = ARPlaneDetectionNone;
            if (sessionFlags & 1) planes |= ARPlaneDetectionHorizontal;
            if (sessionFlags & 2) planes |= ARPlaneDetectionVertical;
            config.planeDetection = planes;
            config.lightEstimationEnabled = (sessionFlags & (4 | 32)) != 0;
            if (@available(iOS 14.0, *)) config.automaticSkeletonScaleEstimationEnabled = YES;
            ARVideoFormat *format = videoFormat(ARBodyTrackingConfiguration.supportedVideoFormats, sessionWidth, sessionHeight);
            if (format) config.videoFormat = format;
            if (sessionFlags & 64) {
                NSSet<ARReferenceImage *> *ready = readyImages();
                config.detectionImages = ready;
                config.maximumNumberOfTrackedImages = MIN((NSInteger)trackedMax, (NSInteger)ready.count);
            }
            runWith(config);
            return true;
        }
        emit("unsupported");
        return false;
    }
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
    ARVideoFormat *format = videoFormat(ARWorldTrackingConfiguration.supportedVideoFormats, sessionWidth, sessionHeight);
    if (format) config.videoFormat = format;
    if (sessionFlags & 64) {
        NSSet<ARReferenceImage *> *ready = readyImages();
        config.detectionImages = ready;
        config.maximumNumberOfTrackedImages = MIN((NSInteger)trackedMax, (NSInteger)ready.count);
    }
    if (sessionFlags & 16384) {
        NSMutableSet<ARReferenceObject *> *objects = [NSMutableSet new];
        for (id o in objectTargets)
            if ([o isKindOfClass:ARReferenceObject.class]) [objects addObject:o];
        config.detectionObjects = objects;
    }
    runWith(config);
    return true;
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
    if (!(flags & (32768 | 65536)) && !ARWorldTrackingConfiguration.isSupported) {
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
#ifdef ESKIU_ARCORE
    garConfigure(flags);
#endif
    sessionWidth = width;
    sessionHeight = height;
    // Run on the next turn of the main queue, once the UI update that started the session is
    // applied: a screen it replaces gives its camera back then, and ARKit started while
    // another session holds the camera waits about nine seconds before it tries again.
    int generation = startGeneration;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (generation != startGeneration) return;
        if (runConfiguration()) emit("running");
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
#ifdef ESKIU_ARCORE
    garLent = garLatest;
#endif
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
    NSMutableArray<ARObjectAnchor *> *objects = [NSMutableArray new];
    NSMutableArray *meshes = [NSMutableArray new];
    NSMutableArray *bodies = [NSMutableArray new];
    for (ARAnchor *anchor in frame.anchors) {
        if (@available(iOS 13.0, *)) {
            if ([anchor isKindOfClass:ARBodyAnchor.class]) {
                [bodies addObject:anchor];
                continue;
            }
        }
        if (@available(iOS 13.4, *)) {
            if ([anchor isKindOfClass:ARMeshAnchor.class]) {
                [meshes addObject:anchor];
                continue;
            }
        }
        if ([anchor isKindOfClass:ARPlaneAnchor.class]) [planes addObject:(ARPlaneAnchor *)anchor];
        else if ([anchor isKindOfClass:ARImageAnchor.class]) [images addObject:(ARImageAnchor *)anchor];
        else if ([anchor isKindOfClass:ARObjectAnchor.class]) [objects addObject:(ARObjectAnchor *)anchor];
        else if (anchorIds[anchor.identifier]) [anchors addObject:anchor];
    }
    lentPlanes = planes;
    lentAnchors = anchors;
    lentImages = images;
    lentObjects = objects;
    lentMeshes = meshes;
    lentBodies = bodies;
    return 1;
}

#include "ar_query.inc"
#include "ar_depth.inc"
#include "ar_arcore.inc"
