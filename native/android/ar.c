// <ar> on Android: ARCore through its C API, with no GL. The session runs in hardware-buffer
// mode (AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER), so ArSession_update needs no GL
// context or texture (docs/design.md), on a thread of its own. After each new frame that
// thread copies the CPU image (YUV_420_888, as NV12) and reads the camera, planes, anchors,
// points, light, depth and image targets into a snapshot; three snapshots rotate (one being filled, the
// newest waiting, the one lent to Eskiu), and "frame" is posted on the "ar_frame" channel
// when none is waiting. Hit tests and anchors call ARCore under the session's lock. EskiuAr
// (Java) asks for the camera permission, follows the activity's pause and resume, and calls
// nativeStart, nativeStop, nativePause and nativeResume. ARCore itself is opened with dlopen.
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ar_api.h"
#include "ui_native.h"

// ARCore's functions, looked up in libarcore_sdk_c.so.
#define X(result, name, params) static result (*p_##name) params;
AR_API
#undef X

static int loaded = -1;

// Opens ARCore and finds every function; 0 when it is missing (no AAR in the APK).
static int loadArcore(void) {
    if (loaded >= 0) return loaded;
    void *lib = dlopen("libarcore_sdk_c.so", RTLD_NOW);
    loaded = lib != NULL;
#define X(result, name, params) if (lib && !(p_##name = (result (*) params)dlsym(lib, #name))) loaded = 0;
    AR_API
#undef X
    return loaded;
}

#define MAX_PLANES 64
#define MAX_POLYGON 256
#define MAX_ANCHORS 64
#define MAX_POINTS 4096

#define FLAG_HORIZONTAL 1
#define FLAG_VERTICAL 2
#define FLAG_LIGHT 4
#define FLAG_DEPTH 8
#define FLAG_POINTS 16
#define FLAG_LIGHT_HDR 32
#define FLAG_IMAGES 64
#define FLAG_CLOUD 2048
#define FLAG_GEO 4096
#define FLAG_SEMANTICS 8192
#define FLAG_FRONT 32768
#define MAX_FACES 3
#define MAX_IMAGES 100

// One frame's worth of everything Eskiu reads, owned by this file.
typedef struct {
    int64_t image[24];
    double camera[24];
    uint8_t *pixels;
    size_t pixels_cap;
    int plane_count;
    double planes[MAX_PLANES][22];
    float polygons[MAX_PLANES][MAX_POLYGON * 2];
    int polygon_counts[MAX_PLANES];
    int anchor_count;
    double anchors[MAX_ANCHORS][18];
    float *points;
    int point_count;
    double light[42];
    int has_light;
    uint16_t *depth;
    size_t depth_cap;
    int depth_width, depth_height, has_depth;
    int image_count;
    double images[MAX_IMAGES][22];
    int has_earth;
    double earth[14];
    uint8_t *semantics;
    size_t semantics_cap;
    int semantics_width, semantics_height, has_semantics, has_confidence;
    int face_count;
    double faces[MAX_FACES][160];
    float *face_vertices[MAX_FACES];
    float *face_uvs[MAX_FACES];
    uint16_t *face_indices[MAX_FACES];
    size_t face_vertex_cap[MAX_FACES], face_index_cap[MAX_FACES];
} Snapshot;

static Snapshot snapshots[3];
static int pending = -1;
static int lent = -1;
static int posted;
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;

// The session, its frame, and what the app asked for; ARCore calls hold session_lock.
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static ArSession *session;
static ArFrame *frame;
static int flags;
static int wish_width = 1280, wish_height = 720;
static int wanted;
static int paused;
static int install_requested;
static int sensor_orientation = 90;
static volatile int display_rotation;
static int display_width, display_height;
static int64_t last_timestamp;

static pthread_t thread;
static volatile int thread_running;

// Planes known so far, by the ArPlane pointer ARCore returns (one reference held each), and
// the app's anchors by id.
typedef struct { ArPlane *plane; int64_t id; } KnownPlane;
static KnownPlane known_planes[256];
static int known_count;
typedef struct { ArAnchor *anchor; int64_t id; } KnownAnchor;
static KnownAnchor anchors[MAX_ANCHORS];
static int anchor_count;
static int64_t next_id = 1;

// The image targets (under session_lock): tight gray pixels or a file path (decoded by
// EskiuAr.decodeGray when the database is built), the width in meters and an ArImageState;
// which target each database index is, and the id each target found has.
typedef struct { uint8_t *gray; int width, height; char *path; double meters; int state; int64_t id; } Target;
static Target targets[MAX_IMAGES];
static int target_count;
static int db_target[MAX_IMAGES];
static int db_count;

static void status(const char *text) { ui_native_emit("ar", text); }

// The id of a plane, keeping a reference to it when it is new; 0 when the table is full.
static int64_t planeId(ArPlane *plane, int keep) {
    for (int i = 0; i < known_count; i++)
        if (known_planes[i].plane == plane) return known_planes[i].id;
    if (!keep || known_count == 256) return 0;
    known_planes[known_count].plane = plane;
    known_planes[known_count].id = next_id++;
    return known_planes[known_count++].id;
}

static void forgetPlanes(void) {
    for (int i = 0; i < known_count; i++) p_ArTrackable_release((ArTrackable *)known_planes[i].plane);
    known_count = 0;
}

static void forgetAnchors(void) {
    for (int i = 0; i < anchor_count; i++) {
        p_ArAnchor_detach(session, anchors[i].anchor);
        p_ArAnchor_release(anchors[i].anchor);
    }
    anchor_count = 0;
}

static void poseMatrix(const ArPose *pose, double *out) {
    float m[16];
    p_ArPose_getMatrix(session, pose, m);
    for (int i = 0; i < 16; i++) out[i] = m[i];
}

static int trackingOf(ArTrackingState state) {
    return state == AR_TRACKING_STATE_TRACKING_ ? 2 : state == AR_TRACKING_STATE_PAUSED_ ? 1 : 0;
}

// ARCore's failure reasons as ArReason values.
static int reasonOf(ArTrackingState state, ArTrackingFailureReason reason) {
    if (state == AR_TRACKING_STATE_TRACKING_) return 0;
    switch (reason) {
        case 1: return 7;
        case 2: return 4;
        case 3: return 2;
        case 4: return 3;
        case 5: return 6;
        default: return 1;
    }
}

static void *grow(void *data, size_t *cap, size_t need) {
    if (*cap >= need) return data;
    void *bigger = realloc(data, need);
    if (!bigger) return data;
    *cap = need;
    return bigger;
}

// The CPU image into the snapshot as NV12 (tight rows), from any YUV_420_888 layout.
static int copyImage(Snapshot *s, ArImage *image) {
    int32_t w = 0, h = 0, planes = 0;
    p_ArImage_getWidth(session, image, &w);
    p_ArImage_getHeight(session, image, &h);
    p_ArImage_getNumberOfPlanes(session, image, &planes);
    if (w <= 0 || h <= 0 || planes != 3) return 0;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t luma = (size_t)w * h, chroma = (size_t)cw * 2 * ch;
    s->pixels = grow(s->pixels, &s->pixels_cap, luma + chroma);
    if (s->pixels_cap < luma + chroma) return 0;
    const uint8_t *data[3];
    int32_t length[3], row[3], pixel[3];
    for (int p = 0; p < 3; p++) {
        p_ArImage_getPlaneData(session, image, p, &data[p], &length[p]);
        p_ArImage_getPlaneRowStride(session, image, p, &row[p]);
        p_ArImage_getPlanePixelStride(session, image, p, &pixel[p]);
        if (!data[p] || row[p] <= 0 || pixel[p] <= 0) return 0;
    }
    if ((int64_t)(h - 1) * row[0] + (int64_t)(w - 1) * pixel[0] + 1 > length[0]) return 0;
    for (int p = 1; p < 3; p++)
        if ((int64_t)(ch - 1) * row[p] + (int64_t)(cw - 1) * pixel[p] + 1 > length[p]) return 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = data[0] + (size_t)y * row[0];
        uint8_t *dst = s->pixels + (size_t)y * w;
        if (pixel[0] == 1) memcpy(dst, src, (size_t)w);
        else for (int x = 0; x < w; x++) dst[x] = src[(size_t)x * pixel[0]];
    }
    uint8_t *uv = s->pixels + luma;
    for (int y = 0; y < ch; y++) {
        const uint8_t *u = data[1] + (size_t)y * row[1];
        const uint8_t *v = data[2] + (size_t)y * row[2];
        uint8_t *dst = uv + (size_t)y * cw * 2;
        for (int x = 0; x < cw; x++) {
            dst[x * 2] = u[(size_t)x * pixel[1]];
            dst[x * 2 + 1] = v[(size_t)x * pixel[2]];
        }
    }
    int64_t *info = s->image;
    memset(info, 0, sizeof(s->image));
    info[0] = w;
    info[1] = h;
    info[2] = 0;
    info[3] = 2;
    info[4] = ((sensor_orientation - display_rotation * 90) % 360 + 360) % 360;
    info[6] = 0;
    info[7] = 1;
    info[9] = (int64_t)(intptr_t)s->pixels;
    info[10] = w;
    info[11] = 1;
    info[12] = (int64_t)luma;
    info[13] = (int64_t)(intptr_t)uv;
    info[14] = cw * 2;
    info[15] = 2;
    info[16] = (int64_t)chroma;
    return 1;
}

static void readCamera(Snapshot *s) {
    ArCamera *camera = NULL;
    p_ArFrame_acquireCamera(session, frame, &camera);
    ArTrackingState state = AR_TRACKING_STATE_STOPPED_;
    ArTrackingFailureReason reason = 0;
    p_ArCamera_getTrackingState(session, camera, &state);
    p_ArCamera_getTrackingFailureReason(session, camera, &reason);
    s->camera[0] = trackingOf(state);
    s->camera[1] = reasonOf(state, reason);
    ArPose *pose = NULL;
    p_ArPose_create(session, NULL, &pose);
    p_ArCamera_getPose(session, camera, pose);
    poseMatrix(pose, &s->camera[2]);
    p_ArPose_destroy(pose);
    ArCameraIntrinsics *k = NULL;
    p_ArCameraIntrinsics_create(session, &k);
    p_ArCamera_getImageIntrinsics(session, camera, k);
    float fx = 0, fy = 0, cx = 0, cy = 0;
    int32_t w = 0, h = 0;
    p_ArCameraIntrinsics_getFocalLength(session, k, &fx, &fy);
    p_ArCameraIntrinsics_getPrincipalPoint(session, k, &cx, &cy);
    p_ArCameraIntrinsics_getImageDimensions(session, k, &w, &h);
    p_ArCameraIntrinsics_destroy(k);
    s->camera[18] = fx;
    s->camera[19] = fy;
    s->camera[20] = cx;
    s->camera[21] = cy;
    s->camera[22] = w;
    s->camera[23] = h;
    p_ArCamera_release(camera);
}

// Every plane ARCore tracks, with an id that stays while it does; stopped ones are
// forgotten.
static void readPlanes(Snapshot *s) {
    s->plane_count = 0;
    ArTrackableList *list = NULL;
    p_ArTrackableList_create(session, &list);
    p_ArSession_getAllTrackables(session, AR_TRACKABLE_PLANE_, list);
    int32_t n = 0;
    p_ArTrackableList_getSize(session, list, &n);
    ArPose *pose = NULL;
    p_ArPose_create(session, NULL, &pose);
    for (int i = 0; i < n; i++) {
        ArTrackable *item = NULL;
        p_ArTrackableList_acquireItem(session, list, i, &item);
        ArPlane *plane = (ArPlane *)item;
        ArTrackingState state = AR_TRACKING_STATE_STOPPED_;
        p_ArTrackable_getTrackingState(session, item, &state);
        // A known plane already has the reference the table holds; a new one keeps this one.
        int64_t id = planeId(plane, 0);
        if (id) p_ArTrackable_release(item);
        else if (state != AR_TRACKING_STATE_STOPPED_ && (id = planeId(plane, 1)) != 0) {}
        else { p_ArTrackable_release(item); continue; }
        if (state == AR_TRACKING_STATE_STOPPED_ || s->plane_count == MAX_PLANES) continue;
        double *info = s->planes[s->plane_count];
        memset(info, 0, sizeof(double) * 22);
        info[0] = (double)id;
        ArPlaneType type = 0;
        p_ArPlane_getType(session, plane, &type);
        info[1] = type == AR_PLANE_VERTICAL_ ? 2 : type == AR_PLANE_HORIZONTAL_DOWNWARD_FACING_ ? 1 : 0;
        info[2] = trackingOf(state);
        p_ArPlane_getCenterPose(session, plane, pose);
        poseMatrix(pose, &info[3]);
        float ex = 0, ez = 0;
        p_ArPlane_getExtentX(session, plane, &ex);
        p_ArPlane_getExtentZ(session, plane, &ez);
        info[19] = ex;
        info[20] = ez;
        ArPlane *by = NULL;
        p_ArPlane_acquireSubsumedBy(session, plane, &by);
        if (by) {
            info[21] = (double)planeId(by, 0);
            p_ArTrackable_release((ArTrackable *)by);
        }
        int32_t size = 0;
        p_ArPlane_getPolygonSize(session, plane, &size);
        int points = size / 2;
        if (points > MAX_POLYGON) points = 0;
        if (points > 0) p_ArPlane_getPolygon(session, plane, s->polygons[s->plane_count]);
        s->polygon_counts[s->plane_count] = points;
        s->plane_count++;
    }
    p_ArPose_destroy(pose);
    p_ArTrackableList_destroy(list);
    // Forget planes that stopped (not returned as tracking any more).
    for (int i = 0; i < known_count;) {
        ArTrackingState state = AR_TRACKING_STATE_STOPPED_;
        p_ArTrackable_getTrackingState(session, (ArTrackable *)known_planes[i].plane, &state);
        if (state == AR_TRACKING_STATE_STOPPED_) {
            p_ArTrackable_release((ArTrackable *)known_planes[i].plane);
            known_planes[i] = known_planes[--known_count];
        } else i++;
    }
}

static void readAnchors(Snapshot *s) {
    s->anchor_count = 0;
    ArPose *pose = NULL;
    p_ArPose_create(session, NULL, &pose);
    for (int i = 0; i < anchor_count; i++) {
        double *info = s->anchors[s->anchor_count++];
        ArTrackingState state = AR_TRACKING_STATE_STOPPED_;
        p_ArAnchor_getTrackingState(session, anchors[i].anchor, &state);
        p_ArAnchor_getPose(session, anchors[i].anchor, pose);
        info[0] = (double)anchors[i].id;
        info[1] = trackingOf(state);
        poseMatrix(pose, &info[2]);
    }
    p_ArPose_destroy(pose);
}

// Earth (with the geospatial flag): its state, tracking, and the camera's geospatial pose
// (latitude, longitude, altitude, their accuracies, the east-up-south quaternion, the yaw
// accuracy).
static void readEarth(Snapshot *s) {
    s->has_earth = 0;
    ArEarth *earth = NULL;
    p_ArSession_acquireEarth(session, &earth);
    if (!earth) return;
    ArEarthState es = 0;
    ArTrackingState ts = AR_TRACKING_STATE_STOPPED_;
    p_ArEarth_getEarthState(session, earth, &es);
    p_ArTrackable_getTrackingState(session, (ArTrackable *)earth, &ts);
    double *e = s->earth;
    memset(e, 0, sizeof(s->earth));
    e[0] = es;
    e[1] = trackingOf(ts);
    if (es == AR_EARTH_STATE_ENABLED_ && ts == AR_TRACKING_STATE_TRACKING_) {
        ArGeospatialPose *g = NULL;
        p_ArGeospatialPose_create(session, &g);
        p_ArEarth_getCameraGeospatialPose(session, earth, g);
        p_ArGeospatialPose_getLatitudeLongitude(session, g, &e[2], &e[3]);
        p_ArGeospatialPose_getAltitude(session, g, &e[4]);
        p_ArGeospatialPose_getHorizontalAccuracy(session, g, &e[5]);
        p_ArGeospatialPose_getVerticalAccuracy(session, g, &e[6]);
        float q[4] = {0, 0, 0, 1};
        p_ArGeospatialPose_getEastUpSouthQuaternion(session, g, q);
        for (int k = 0; k < 4; k++) e[7 + k] = q[k];
        p_ArGeospatialPose_getOrientationYawAccuracy(session, g, &e[11]);
        p_ArGeospatialPose_destroy(g);
    }
    p_ArTrackable_release((ArTrackable *)earth);
    s->has_earth = 1;
}

static void readPoints(Snapshot *s) {
    s->point_count = 0;
    ArPointCloud *cloud = NULL;
    if (p_ArFrame_acquirePointCloud(session, frame, &cloud) != AR_SUCCESS_ || !cloud) return;
    int32_t n = 0;
    const float *data = NULL;
    p_ArPointCloud_getNumberOfPoints(session, cloud, &n);
    p_ArPointCloud_getData(session, cloud, &data);
    if (n > MAX_POINTS) n = MAX_POINTS;
    if (!s->points) s->points = malloc(sizeof(float) * 4 * MAX_POINTS);
    if (data && s->points && n > 0) {
        memcpy(s->points, data, sizeof(float) * 4 * (size_t)n);
        s->point_count = n;
    }
    p_ArPointCloud_release(cloud);
}

// ARCore's own values, which Eskiu puts on the package's scale (ar_light_read). Ambient mode
// (source 2): the pixel intensity and color correction, both in gamma space. Environmental
// HDR (source 3): the main light and the spherical harmonics.
static void readLight(Snapshot *s) {
    s->has_light = 0;
    memset(s->light, 0, sizeof(s->light));
    ArLightEstimate *light = NULL;
    p_ArLightEstimate_create(session, &light);
    p_ArFrame_getLightEstimate(session, frame, light);
    ArLightEstimateState state = 0;
    p_ArLightEstimate_getState(session, light, &state);
    if (state == AR_LIGHT_ESTIMATE_STATE_VALID_) {
        s->has_light = 1;
        s->light[0] = 1;
        s->light[2] = 6500;
        if (flags & FLAG_LIGHT_HDR) {
            float d[3], i3[3], sh[27];
            p_ArLightEstimate_getEnvironmentalHdrMainLightDirection(session, light, d);
            p_ArLightEstimate_getEnvironmentalHdrMainLightIntensity(session, light, i3);
            p_ArLightEstimate_getEnvironmentalHdrAmbientSphericalHarmonics(session, light, sh);
            s->light[6] = 1;
            for (int k = 0; k < 3; k++) { s->light[7 + k] = d[k]; s->light[10 + k] = i3[k]; }
            s->light[13] = 1;
            for (int k = 0; k < 27; k++) s->light[14 + k] = sh[k];
            s->light[41] = 3;
        } else {
            float intensity = 0, color[4] = {1, 1, 1, 1};
            p_ArLightEstimate_getPixelIntensity(session, light, &intensity);
            p_ArLightEstimate_getColorCorrection(session, light, color);
            s->light[1] = intensity;
            s->light[3] = color[0];
            s->light[4] = color[1];
            s->light[5] = color[2];
            s->light[41] = 2;
        }
    }
    p_ArLightEstimate_destroy(light);
}

static void readDepth(Snapshot *s) {
    s->has_depth = 0;
    ArImage *image = NULL;
    if (p_ArFrame_acquireDepthImage16Bits(session, frame, &image) != AR_SUCCESS_ || !image) return;
    int32_t w = 0, h = 0, row = 0, length = 0;
    const uint8_t *data = NULL;
    p_ArImage_getWidth(session, image, &w);
    p_ArImage_getHeight(session, image, &h);
    p_ArImage_getPlaneRowStride(session, image, 0, &row);
    p_ArImage_getPlaneData(session, image, 0, &data, &length);
    size_t need = (size_t)w * h * 2;
    if (w > 0 && h > 0 && data && row >= w * 2 && (int64_t)(h - 1) * row + w * 2 <= length) {
        s->depth = grow(s->depth, &s->depth_cap, need);
        if (s->depth_cap >= need) {
            for (int y = 0; y < h; y++) memcpy((uint8_t *)s->depth + (size_t)y * w * 2, data + (size_t)y * row, (size_t)w * 2);
            s->depth_width = w;
            s->depth_height = h;
            s->has_depth = 1;
        }
    }
    p_ArImage_release(image);
}

// One 8-bit plane of an ARCore image, tight, into dst at `offset` (w x h); 0 when it is not
// one.
static int copyPlane8(ArImage *image, uint8_t *dst, int w, int h) {
    int32_t iw = 0, ih = 0, row = 0, length = 0;
    const uint8_t *data = NULL;
    p_ArImage_getWidth(session, image, &iw);
    p_ArImage_getHeight(session, image, &ih);
    p_ArImage_getPlaneRowStride(session, image, 0, &row);
    p_ArImage_getPlaneData(session, image, 0, &data, &length);
    if (iw != w || ih != h || !data || row < w || (int64_t)(h - 1) * row + w > length) return 0;
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, data + (size_t)y * row, (size_t)w);
    return 1;
}

// Scene semantics: the label of each pixel (sky, building, tree, road...) and its confidence,
// one after the other in s->semantics.
static void readSemantics(Snapshot *s) {
    s->has_semantics = 0;
    s->has_confidence = 0;
    ArImage *labels = NULL;
    if (p_ArFrame_acquireSemanticImage(session, frame, &labels) != AR_SUCCESS_ || !labels) return;
    int32_t w = 0, h = 0;
    p_ArImage_getWidth(session, labels, &w);
    p_ArImage_getHeight(session, labels, &h);
    size_t need = (size_t)w * h * 2;
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        s->semantics = grow(s->semantics, &s->semantics_cap, need);
        if (s->semantics_cap >= need && copyPlane8(labels, s->semantics, w, h)) {
            s->semantics_width = w;
            s->semantics_height = h;
            s->has_semantics = 1;
            ArImage *confidence = NULL;
            if (p_ArFrame_acquireSemanticConfidenceImage(session, frame, &confidence) == AR_SUCCESS_ && confidence) {
                s->has_confidence = copyPlane8(confidence, s->semantics + (size_t)w * h, w, h);
                p_ArImage_release(confidence);
            }
        }
    }
    p_ArImage_release(labels);
}

// The faces ARCore's Augmented Faces tracks (front camera): the center pose, the mesh
// (copied into the snapshot: ARCore's arrays live only until the next update) and the
// nose tip's and forehead regions' poses.
static void readFaces(Snapshot *s) {
    s->face_count = 0;
    ArTrackableList *list = NULL;
    p_ArTrackableList_create(session, &list);
    p_ArSession_getAllTrackables(session, AR_TRACKABLE_FACE_, list);
    int32_t n = 0;
    p_ArTrackableList_getSize(session, list, &n);
    ArPose *pose = NULL;
    p_ArPose_create(session, NULL, &pose);
    for (int i = 0; i < n && s->face_count < MAX_FACES; i++) {
        ArTrackable *item = NULL;
        p_ArTrackableList_acquireItem(session, list, i, &item);
        ArAugmentedFace *face = (ArAugmentedFace *)item;
        ArTrackingState state = AR_TRACKING_STATE_STOPPED_;
        p_ArTrackable_getTrackingState(session, item, &state);
        const float *vertices = NULL;
        const float *uvs = NULL;
        const uint16_t *indices = NULL;
        int32_t vn = 0, un = 0, tn = 0;
        if (state == AR_TRACKING_STATE_TRACKING_) {
            p_ArAugmentedFace_getMeshVertices(session, face, &vertices, &vn);
            p_ArAugmentedFace_getMeshTextureCoordinates(session, face, &uvs, &un);
            p_ArAugmentedFace_getMeshTriangleIndices(session, face, &indices, &tn);
        }
        if (!vertices || !indices || vn <= 0 || tn <= 0) {
            p_ArTrackable_release(item);
            continue;
        }
        int k = s->face_count++;
        if (s->face_vertex_cap[k] < (size_t)vn) {
            free(s->face_vertices[k]);
            free(s->face_uvs[k]);
            s->face_vertices[k] = malloc(sizeof(float) * 3 * vn);
            s->face_uvs[k] = malloc(sizeof(float) * 2 * vn);
            s->face_vertex_cap[k] = vn;
        }
        if (s->face_index_cap[k] < (size_t)tn) {
            free(s->face_indices[k]);
            s->face_indices[k] = malloc(sizeof(uint16_t) * 3 * tn);
            s->face_index_cap[k] = tn;
        }
        memcpy(s->face_vertices[k], vertices, sizeof(float) * 3 * vn);
        if (uvs && un == vn) memcpy(s->face_uvs[k], uvs, sizeof(float) * 2 * vn);
        memcpy(s->face_indices[k], indices, sizeof(uint16_t) * 3 * tn);
        double *info = s->faces[k];
        memset(info, 0, sizeof(double) * 160);
        // ARCore tracks one face at a time: its id stays while it is tracked
        info[0] = (double)(k + 1);
        p_ArAugmentedFace_getCenterPose(session, face, pose);
        poseMatrix(pose, &info[1]);
        info[17] = (double)(intptr_t)s->face_vertices[k];
        info[18] = vn;
        info[19] = uvs && un == vn ? (double)(intptr_t)s->face_uvs[k] : 0;
        info[20] = (double)(intptr_t)s->face_indices[k];
        info[21] = tn;
        info[22] = 2;
        p_ArAugmentedFace_getRegionPose(session, face, AR_AUGMENTED_FACE_REGION_NOSE_TIP_, pose);
        poseMatrix(pose, &info[111]);
        p_ArAugmentedFace_getRegionPose(session, face, AR_AUGMENTED_FACE_REGION_FOREHEAD_LEFT_, pose);
        poseMatrix(pose, &info[127]);
        p_ArAugmentedFace_getRegionPose(session, face, AR_AUGMENTED_FACE_REGION_FOREHEAD_RIGHT_, pose);
        poseMatrix(pose, &info[143]);
        info[159] = trackingOf(state);
        p_ArTrackable_release(item);
    }
    p_ArPose_destroy(pose);
    p_ArTrackableList_destroy(list);
}

// The image targets ARCore knows of, tracked or seen before (not stopped).
static void readImages(Snapshot *s) {
    s->image_count = 0;
    ArTrackableList *list = NULL;
    p_ArTrackableList_create(session, &list);
    p_ArSession_getAllTrackables(session, AR_TRACKABLE_AUGMENTED_IMAGE_, list);
    int32_t n = 0;
    p_ArTrackableList_getSize(session, list, &n);
    ArPose *pose = NULL;
    p_ArPose_create(session, NULL, &pose);
    for (int i = 0; i < n && s->image_count < MAX_IMAGES; i++) {
        ArTrackable *item = NULL;
        p_ArTrackableList_acquireItem(session, list, i, &item);
        ArAugmentedImage *image = (ArAugmentedImage *)item;
        ArTrackingState state = AR_TRACKING_STATE_STOPPED_;
        p_ArTrackable_getTrackingState(session, item, &state);
        int32_t index = -1;
        p_ArAugmentedImage_getIndex(session, image, &index);
        if (state == AR_TRACKING_STATE_STOPPED_ || index < 0 || index >= db_count) {
            p_ArTrackable_release(item);
            continue;
        }
        Target *target = &targets[db_target[index]];
        if (!target->id) target->id = next_id++;
        ArAugmentedImageTrackingMethod method = 0;
        p_ArAugmentedImage_getTrackingMethod(session, image, &method);
        double *info = s->images[s->image_count++];
        memset(info, 0, sizeof(double) * 22);
        info[0] = (double)target->id;
        info[1] = db_target[index];
        info[2] = trackingOf(state);
        info[3] = method == AR_AUGMENTED_IMAGE_TRACKING_METHOD_FULL_TRACKING_ ? 1 :
                  method == AR_AUGMENTED_IMAGE_TRACKING_METHOD_LAST_KNOWN_POSE_ ? 2 : 0;
        p_ArAugmentedImage_getCenterPose(session, image, pose);
        poseMatrix(pose, &info[4]);
        float ex = 0, ez = 0;
        p_ArAugmentedImage_getExtentX(session, image, &ex);
        p_ArAugmentedImage_getExtentZ(session, image, &ez);
        info[20] = ex;
        info[21] = ez;
        p_ArTrackable_release(item);
    }
    p_ArPose_destroy(pose);
    p_ArTrackableList_destroy(list);
}

// One turn of the session's thread: update, and when the frame is new fill a free snapshot
// and make it the one waiting. Returns 1 when there was a new frame.
static int step(void) {
    pthread_mutex_lock(&snap_lock);
    int fill = 0;
    while (fill == pending || fill == lent) fill++;
    pthread_mutex_unlock(&snap_lock);
    Snapshot *s = &snapshots[fill];
    pthread_mutex_lock(&session_lock);
    if (!session || paused || p_ArSession_update(session, frame) != AR_SUCCESS_) {
        pthread_mutex_unlock(&session_lock);
        return 0;
    }
    int64_t timestamp = 0;
    p_ArFrame_getTimestamp(session, frame, &timestamp);
    if (timestamp == 0 || timestamp == last_timestamp) {
        pthread_mutex_unlock(&session_lock);
        return 0;
    }
    last_timestamp = timestamp;
    ArImage *image = NULL;
    int ok = p_ArFrame_acquireCameraImage(session, frame, &image) == AR_SUCCESS_ && image;
    if (ok) {
        ok = copyImage(s, image);
        p_ArImage_release(image);
    }
    if (ok) {
        s->image[8] = timestamp;
        readCamera(s);
        if (flags & (FLAG_HORIZONTAL | FLAG_VERTICAL)) readPlanes(s);
        else s->plane_count = 0;
        readAnchors(s);
        if (flags & FLAG_GEO) readEarth(s);
        else s->has_earth = 0;
        if (flags & FLAG_POINTS) readPoints(s);
        else s->point_count = 0;
        if (flags & (FLAG_LIGHT | FLAG_LIGHT_HDR)) readLight(s);
        else s->has_light = 0;
        if (flags & FLAG_DEPTH) readDepth(s);
        else s->has_depth = 0;
        if (flags & FLAG_SEMANTICS) readSemantics(s);
        else s->has_semantics = 0;
        if (flags & FLAG_IMAGES) readImages(s);
        else s->image_count = 0;
        if (flags & FLAG_FRONT) readFaces(s);
        else s->face_count = 0;
    }
    pthread_mutex_unlock(&session_lock);
    if (!ok) return 1;
    pthread_mutex_lock(&snap_lock);
    pending = fill;
    int post = !posted;
    posted = 1;
    pthread_mutex_unlock(&snap_lock);
    if (post) ui_native_emit("ar_frame", "frame");
    return 1;
}

static void *loop(void *unused) {
    (void)unused;
    ui_jni_env();
    while (thread_running) {
        if (!step()) {
            struct timespec wait = {0, 4000000};
            nanosleep(&wait, NULL);
        }
    }
    return NULL;
}

static void startThread(void) {
    if (thread_running) return;
    thread_running = 1;
    if (pthread_create(&thread, NULL, loop, NULL) != 0) thread_running = 0;
}

static void stopThread(void) {
    if (!thread_running) return;
    thread_running = 0;
    pthread_join(thread, NULL);
}

// Picks the camera config whose CPU image is the smallest that holds the wish, else the
// largest. Called while the session is paused.
static void chooseCameraConfig(void) {
    ArCameraConfigFilter *filter = NULL;
    ArCameraConfigList *list = NULL;
    p_ArCameraConfigFilter_create(session, &filter);
    p_ArCameraConfigList_create(session, &list);
    if (flags & FLAG_FRONT) p_ArCameraConfigFilter_setFacingDirection(session, filter, AR_CAMERA_CONFIG_FACING_DIRECTION_FRONT_);
    p_ArSession_getSupportedCameraConfigsWithFilter(session, filter, list);
    int32_t n = 0;
    p_ArCameraConfigList_getSize(session, list, &n);
    int best = -1, largest = -1;
    int64_t best_area = 0, largest_area = 0;
    ArCameraConfig *config = NULL;
    p_ArCameraConfig_create(session, &config);
    for (int i = 0; i < n; i++) {
        int32_t w = 0, h = 0;
        p_ArCameraConfigList_getItem(session, list, i, config);
        p_ArCameraConfig_getImageDimensions(session, config, &w, &h);
        int64_t area = (int64_t)w * h;
        if (area > largest_area) { largest_area = area; largest = i; }
        if (w >= wish_width && h >= wish_height && (best < 0 || area < best_area)) { best_area = area; best = i; }
    }
    int pick = best >= 0 ? best : largest;
    if (pick >= 0) {
        p_ArCameraConfigList_getItem(session, list, pick, config);
        p_ArSession_setCameraConfig(session, config);
    }
    p_ArCameraConfig_destroy(config);
    p_ArCameraConfigList_destroy(list);
    p_ArCameraConfigFilter_destroy(filter);
}

// A target's file decoded to gray by EskiuAr.decodeGray (a byte array: width and height as
// 4 bytes each, big-endian, then the rows), kept in the target; 0 when it does not decode.
static int decodeTarget(Target *target) {
    JNIEnv *env = ui_jni_env();
    jclass type = ui_find_class("org.eskiu.ar.EskiuAr");
    if (!type) return 0;
    jmethodID decode = (*env)->GetStaticMethodID(env, type, "decodeGray", "(Ljava/lang/String;)[B");
    jstring path = (*env)->NewStringUTF(env, target->path);
    jbyteArray bytes = decode && path ? (jbyteArray)(*env)->CallStaticObjectMethod(env, type, decode, path) : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); bytes = NULL; }
    int ok = 0;
    jsize length = bytes ? (*env)->GetArrayLength(env, bytes) : 0;
    if (length > 8) {
        jbyte *data = (*env)->GetByteArrayElements(env, bytes, NULL);
        const uint8_t *u = (const uint8_t *)data;
        int w = (int)((uint32_t)u[0] << 24 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | u[3]);
        int h = (int)((uint32_t)u[4] << 24 | (uint32_t)u[5] << 16 | (uint32_t)u[6] << 8 | u[7]);
        if (w > 0 && h > 0 && (int64_t)w * h == length - 8 && (target->gray = malloc((size_t)w * h)) != NULL) {
            memcpy(target->gray, u + 8, (size_t)w * h);
            target->width = w;
            target->height = h;
            ok = 1;
        }
        (*env)->ReleaseByteArrayElements(env, bytes, data, JNI_ABORT);
    }
    if (bytes) (*env)->DeleteLocalRef(env, bytes);
    if (path) (*env)->DeleteLocalRef(env, path);
    (*env)->DeleteLocalRef(env, type);
    return ok;
}

// The image database of the targets, when the flags ask for them: each one added with its
// width in meters (20 to 30 ms each, ARCore says), its state set by the result (ARCore
// refuses an image with too few features). NULL when there are none.
static ArAugmentedImageDatabase *buildDatabase(void) {
    db_count = 0;
    if (!(flags & FLAG_IMAGES) || target_count == 0) return NULL;
    ArAugmentedImageDatabase *database = NULL;
    p_ArAugmentedImageDatabase_create(session, &database);
    if (!database) return NULL;
    for (int i = 0; i < target_count; i++) {
        Target *target = &targets[i];
        if (target->meters <= 0 || (!target->gray && !target->path)) { target->state = 2; continue; }
        if (!target->gray && !decodeTarget(target)) { target->state = 2; continue; }
        char name[16];
        snprintf(name, sizeof(name), "%d", i);
        int32_t index = -1;
        ArStatus result = p_ArAugmentedImageDatabase_addImageWithPhysicalSize(session, database, name, target->gray, target->width,
                                                                              target->height, target->width, (float)target->meters, &index);
        if (result == AR_SUCCESS_ && index >= 0 && index < MAX_IMAGES) {
            target->state = 1;
            db_target[index] = i;
            if (index >= db_count) db_count = index + 1;
        } else target->state = 2;
    }
    return database;
}

// Applies the flags to the session; 0 when ARCore refuses the configuration.
static int configure(void) {
    ArConfig *config = NULL;
    p_ArConfig_create(session, &config);
    p_ArConfig_setTextureUpdateMode(session, config, AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER_);
    p_ArConfig_setUpdateMode(session, config, AR_UPDATE_MODE_LATEST_CAMERA_IMAGE_);
    p_ArConfig_setFocusMode(session, config, AR_FOCUS_MODE_AUTO_);
    // Augmented Faces takes no planes and only the ambient light estimate (as Viro sets it)
    int front = (flags & FLAG_FRONT) != 0;
    p_ArConfig_setPlaneFindingMode(session, config, front ? 0 : flags & (FLAG_HORIZONTAL | FLAG_VERTICAL));
    if (front) p_ArConfig_setAugmentedFaceMode(session, config, AR_AUGMENTED_FACE_MODE_MESH3D_);
    p_ArConfig_setLightEstimationMode(session, config, (flags & FLAG_LIGHT_HDR) && !front ? AR_LIGHT_ESTIMATION_MODE_ENVIRONMENTAL_HDR_ :
                                      (flags & FLAG_LIGHT) ? AR_LIGHT_ESTIMATION_MODE_AMBIENT_INTENSITY_ : 0);
    int32_t depth = 0;
    if (flags & FLAG_DEPTH) p_ArSession_isDepthModeSupported(session, AR_DEPTH_MODE_AUTOMATIC_, &depth);
    p_ArConfig_setDepthMode(session, config, depth ? AR_DEPTH_MODE_AUTOMATIC_ : 0);
    p_ArConfig_setCloudAnchorMode(session, config, (flags & FLAG_CLOUD) && !front ? AR_CLOUD_ANCHOR_MODE_ENABLED_ : 0);
    int32_t geo = 0;
    if (flags & FLAG_GEO) p_ArSession_isGeospatialModeSupported(session, AR_GEOSPATIAL_MODE_ENABLED_, &geo);
    p_ArConfig_setGeospatialMode(session, config, geo ? AR_GEOSPATIAL_MODE_ENABLED_ : 0);
    int32_t semantic = 0;
    if (flags & FLAG_SEMANTICS) p_ArSession_isSemanticModeSupported(session, AR_SEMANTIC_MODE_ENABLED_, &semantic);
    p_ArConfig_setSemanticMode(session, config, semantic ? AR_SEMANTIC_MODE_ENABLED_ : 0);
    ArAugmentedImageDatabase *database = buildDatabase();
    p_ArConfig_setAugmentedImageDatabase(session, config, database);
    if (database) p_ArAugmentedImageDatabase_destroy(database);
    ArStatus result = p_ArSession_configure(session, config);
    p_ArConfig_destroy(config);
    return result == AR_SUCCESS_;
}

// Installs ARCore if needed, then creates, configures and resumes the session. On the UI
// thread, with the camera permission granted.
static void begin(JNIEnv *env, jobject activity) {
    if (!loadArcore()) { status("unsupported"); return; }
    ArAvailability availability = 0;
    p_ArCoreApk_checkAvailability(env, activity, &availability);
    if (availability == AR_AVAILABILITY_UNSUPPORTED_DEVICE_NOT_CAPABLE_) { status("unsupported"); return; }
    ArInstallStatus install = 0;
    ArStatus result = p_ArCoreApk_requestInstall(env, activity, !install_requested, &install);
    if (result != AR_SUCCESS_) {
        install_requested = 0;
        status(result == AR_UNAVAILABLE_DEVICE_NOT_COMPATIBLE_ ? "unsupported" : "failed");
        return;
    }
    if (install == AR_INSTALL_STATUS_INSTALL_REQUESTED_) {
        install_requested = 1;
        status("installing");
        return;
    }
    install_requested = 0;
    pthread_mutex_lock(&session_lock);
    if (!session) {
        result = p_ArSession_create(env, activity, &session);
        if (result != AR_SUCCESS_ || !session) {
            session = NULL;
            pthread_mutex_unlock(&session_lock);
            status(result == AR_UNAVAILABLE_DEVICE_NOT_COMPATIBLE_ ? "unsupported" : "failed");
            return;
        }
        p_ArFrame_create(session, &frame);
        chooseCameraConfig();
        if (display_width > 0) p_ArSession_setDisplayGeometry(session, display_rotation, display_width, display_height);
    } else {
        p_ArSession_pause(session);
    }
    int ok = configure();
    if (ok) ok = p_ArSession_resume(session) == AR_SUCCESS_;
    pthread_mutex_unlock(&session_lock);
    if (!ok) { status("unsupported"); return; }
    paused = 0;
    startThread();
    status("running");
}

static void end(void) {
    stopThread();
    pthread_mutex_lock(&session_lock);
    for (int i = 0; i < target_count; i++) targets[i].id = 0;
    if (session) {
        forgetAnchors();
        forgetPlanes();
        p_ArFrame_destroy(frame);
        p_ArSession_destroy(session);
        session = NULL;
        frame = NULL;
    }
    pthread_mutex_unlock(&session_lock);
    pthread_mutex_lock(&snap_lock);
    pending = -1;
    posted = 0;
    pthread_mutex_unlock(&snap_lock);
    last_timestamp = 0;
}

JNIEXPORT void JNICALL Java_org_eskiu_ar_EskiuAr_nativeStart(JNIEnv *env, jclass type, jobject activity, jint sensor) {
    (void)type;
    sensor_orientation = sensor;
    if (wanted) begin(env, activity);
}

JNIEXPORT void JNICALL Java_org_eskiu_ar_EskiuAr_nativeDisplay(JNIEnv *env, jclass type, jint rotation, jint width, jint height) {
    (void)env; (void)type;
    display_rotation = rotation;
    display_width = width;
    display_height = height;
    pthread_mutex_lock(&session_lock);
    if (session) p_ArSession_setDisplayGeometry(session, rotation, width, height);
    pthread_mutex_unlock(&session_lock);
}

JNIEXPORT void JNICALL Java_org_eskiu_ar_EskiuAr_nativePause(JNIEnv *env, jclass type) {
    (void)env; (void)type;
    if (!session) return;
    stopThread();
    pthread_mutex_lock(&session_lock);
    paused = 1;
    p_ArSession_pause(session);
    pthread_mutex_unlock(&session_lock);
    status("paused");
}

JNIEXPORT void JNICALL Java_org_eskiu_ar_EskiuAr_nativeResume(JNIEnv *env, jclass type, jobject activity) {
    (void)type;
    if (!wanted) return;
    if (install_requested || !session) { begin(env, activity); return; }
    if (!paused) return;
    pthread_mutex_lock(&session_lock);
    int ok = p_ArSession_resume(session) == AR_SUCCESS_;
    pthread_mutex_unlock(&session_lock);
    if (!ok) { status("failed"); return; }
    paused = 0;
    startThread();
    status("running");
}

// Starts (asks EskiuAr for the camera permission, which calls nativeStart) or stops.
void ar_native_session(int on, int new_flags, int width, int height) {
    JNIEnv *env = ui_jni_env();
    if (!on) {
        wanted = 0;
        install_requested = 0;
        end();
        return;
    }
    wanted = 1;
    int restart = session && (width != wish_width || height != wish_height);
    flags = new_flags;
    wish_width = width;
    wish_height = height;
    if (restart) end();
    if (session) {
        // New flags on a running session: configure it again.
        stopThread();
        pthread_mutex_lock(&session_lock);
        p_ArSession_pause(session);
        int ok = configure() && p_ArSession_resume(session) == AR_SUCCESS_;
        pthread_mutex_unlock(&session_lock);
        if (!ok) { status("unsupported"); return; }
        startThread();
        return;
    }
    jclass type = ui_find_class("org.eskiu.ar.EskiuAr");
    if (!type) { status("failed"); return; }
    (*env)->CallStaticVoidMethod(env, type, (*env)->GetStaticMethodID(env, type, "start", "()V"));
    (*env)->DeleteLocalRef(env, type);
}

int ar_native_frame_acquire(int64_t *image, double *camera) {
    pthread_mutex_lock(&snap_lock);
    lent = pending;
    pending = -1;
    posted = 0;
    pthread_mutex_unlock(&snap_lock);
    if (lent < 0) return 0;
    Snapshot *s = &snapshots[lent];
    memcpy(image, s->image, sizeof(s->image));
    memcpy(camera, s->camera, sizeof(s->camera));
    return 1;
}

void ar_native_frame_release(void) {
    pthread_mutex_lock(&snap_lock);
    lent = -1;
    pthread_mutex_unlock(&snap_lock);
}

static Snapshot *lentSnapshot(void) { return lent >= 0 ? &snapshots[lent] : NULL; }

// ARCore has no reconstruction mesh (its depth is in ar_native_depth).
// ARCore's Depth API gives depth without a sensor, so no depth model runs here.
void ar_native_depth_model(const char *path) { (void)path; }
// ARCore has no object detection: no targets load and none are found.
void ar_native_objects(int count) { (void)count; }
int ar_native_object_add(int index, const char *path) { (void)index; (void)path; return 0; }
int ar_native_tracked_object_count(void) { return 0; }
void ar_native_tracked_object(int index, double *info) { (void)index; for (int i = 0; i < 24; i++) info[i] = 0; }
int ar_native_mesh_count(void) { return 0; }
void ar_native_mesh(int index, double *info) { (void)index; for (int i = 0; i < 25; i++) info[i] = 0; }

int ar_native_plane_count(void) {
    Snapshot *s = lentSnapshot();
    return s ? s->plane_count : 0;
}

int ar_native_plane(int index, double *info, float *polygon, int max_points) {
    Snapshot *s = lentSnapshot();
    if (!s || index < 0 || index >= s->plane_count) return -1;
    memcpy(info, s->planes[index], sizeof(double) * 22);
    info[22] = 0;    // ARCore does not classify planes
    int n = s->polygon_counts[index];
    if (n > max_points) n = max_points;
    memcpy(polygon, s->polygons[index], sizeof(float) * 2 * (size_t)n);
    return n;
}

int ar_native_anchor_count(void) {
    Snapshot *s = lentSnapshot();
    return s ? s->anchor_count : 0;
}

void ar_native_anchor(int index, double *info) {
    Snapshot *s = lentSnapshot();
    memset(info, 0, sizeof(double) * 18);
    if (s && index >= 0 && index < s->anchor_count) memcpy(info, s->anchors[index], sizeof(double) * 18);
}

int ar_native_points(float *out, int max_points) {
    Snapshot *s = lentSnapshot();
    if (!s || !s->points) return 0;
    int n = s->point_count < max_points ? s->point_count : max_points;
    memcpy(out, s->points, sizeof(float) * 4 * (size_t)n);
    return n;
}

int ar_native_light(double *info) {
    Snapshot *s = lentSnapshot();
    if (!s || !s->has_light) return 0;
    memcpy(info, s->light, sizeof(s->light));
    return 1;
}

int ar_native_depth(int64_t *info) {
    Snapshot *s = lentSnapshot();
    if (!s || !s->has_depth) return 0;
    info[0] = s->depth_width;
    info[1] = s->depth_height;
    info[2] = 1;
    info[3] = s->depth_width * 2;
    info[4] = (int64_t)(intptr_t)s->depth;
    info[5] = (int64_t)s->depth_width * s->depth_height * 2;
    return 1;
}

// The lent frame's semantics: width, height, the labels, the confidences (0 when there are
// none); 0 when it has none.
int ar_native_semantics(int64_t *info) {
    Snapshot *s = lentSnapshot();
    if (!s || !s->has_semantics) return 0;
    info[0] = s->semantics_width;
    info[1] = s->semantics_height;
    info[2] = (int64_t)(intptr_t)s->semantics;
    info[3] = s->has_confidence ? (int64_t)(intptr_t)(s->semantics + (size_t)s->semantics_width * s->semantics_height) : 0;
    return 1;
}

// A hit test on the session's current frame (the newest, a frame or two after the lent
// snapshot), along the ray.
int ar_native_hit_test(float *ray, int targets, double *out, int max_hits) {
    for (int k = 0; k < 6; k++) if (!isfinite(ray[k])) return 0;
    if (ray[3] * ray[3] + ray[4] * ray[4] + ray[5] * ray[5] < 1e-12f) return 0;
    pthread_mutex_lock(&session_lock);
    if (!session || paused || last_timestamp == 0) {
        pthread_mutex_unlock(&session_lock);
        return 0;
    }
    ArHitResultList *list = NULL;
    ArHitResult *hit = NULL;
    ArPose *pose = NULL;
    p_ArHitResultList_create(session, &list);
    p_ArHitResult_create(session, &hit);
    p_ArPose_create(session, NULL, &pose);
    p_ArFrame_hitTestRay(session, frame, ray, ray + 3, list);
    int32_t n = 0;
    p_ArHitResultList_getSize(session, list, &n);
    int count = 0;
    for (int i = 0; i < n && count < max_hits; i++) {
        p_ArHitResultList_getItem(session, list, i, hit);
        ArTrackable *trackable = NULL;
        p_ArHitResult_acquireTrackable(session, hit, &trackable);
        if (!trackable) continue;
        ArTrackableType type = 0;
        p_ArTrackable_getType(session, trackable, &type);
        p_ArHitResult_getHitPose(session, hit, pose);
        int kind = 0;
        int64_t plane = 0;
        if (type == AR_TRACKABLE_PLANE_ && (targets & 1)) {
            int32_t inside = 0;
            p_ArPlane_isPoseInPolygon(session, (ArPlane *)trackable, pose, &inside);
            if (inside) {
                kind = 1;
                plane = planeId((ArPlane *)trackable, 0);
            }
        } else if ((type == AR_TRACKABLE_POINT_ || type == AR_TRACKABLE_DEPTH_POINT_) && (targets & 2)) {
            kind = 2;
        }
        if (kind) {
            double *h = &out[count * 19];
            float distance = 0;
            p_ArHitResult_getDistance(session, hit, &distance);
            h[0] = distance;
            h[1] = kind;
            h[2] = (double)plane;
            poseMatrix(pose, &h[3]);
            count++;
        }
        p_ArTrackable_release(trackable);
    }
    p_ArPose_destroy(pose);
    p_ArHitResult_destroy(hit);
    p_ArHitResultList_destroy(list);
    pthread_mutex_unlock(&session_lock);
    return count;
}

// A rotation matrix's quaternion and the translation, as ARCore's raw pose (qx, qy, qz, qw,
// tx, ty, tz).
static void rawPose(const float *m, float *raw) {
    float trace = m[0] + m[5] + m[10];
    float qx, qy, qz, qw;
    if (trace > 0) {
        float s = sqrtf(trace + 1.0f) * 2;
        qw = 0.25f * s; qx = (m[6] - m[9]) / s; qy = (m[8] - m[2]) / s; qz = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2;
        qw = (m[6] - m[9]) / s; qx = 0.25f * s; qy = (m[4] + m[1]) / s; qz = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2;
        qw = (m[8] - m[2]) / s; qx = (m[4] + m[1]) / s; qy = 0.25f * s; qz = (m[9] + m[6]) / s;
    } else {
        float s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2;
        qw = (m[1] - m[4]) / s; qx = (m[8] + m[2]) / s; qy = (m[9] + m[6]) / s; qz = 0.25f * s;
    }
    raw[0] = qx; raw[1] = qy; raw[2] = qz; raw[3] = qw;
    raw[4] = m[12]; raw[5] = m[13]; raw[6] = m[14];
}

double ar_native_anchor_add(float *matrix, double plane_id) {
    pthread_mutex_lock(&session_lock);
    if (!session || paused || anchor_count == MAX_ANCHORS) {
        pthread_mutex_unlock(&session_lock);
        return 0;
    }
    float raw[7];
    rawPose(matrix, raw);
    ArPose *pose = NULL;
    p_ArPose_create(session, raw, &pose);
    ArAnchor *anchor = NULL;
    ArPlane *plane = NULL;
    for (int i = 0; i < known_count; i++)
        if (plane_id > 0 && known_planes[i].id == (int64_t)plane_id) plane = known_planes[i].plane;
    ArStatus result = plane ? p_ArTrackable_acquireNewAnchor(session, (ArTrackable *)plane, pose, &anchor)
                            : p_ArSession_acquireNewAnchor(session, pose, &anchor);
    p_ArPose_destroy(pose);
    double id = 0;
    if (result == AR_SUCCESS_ && anchor) {
        anchors[anchor_count].anchor = anchor;
        anchors[anchor_count].id = next_id++;
        id = (double)anchors[anchor_count++].id;
    }
    pthread_mutex_unlock(&session_lock);
    return id;
}

// --- cloud anchors and Earth: ARCore's futures, kept as jobs the Eskiu side polls ---

#define MAX_JOBS 32
typedef struct { int64_t id; int kind; ArFuture *future; } Job;
static Job jobs[MAX_JOBS];
static int job_count;
static int64_t next_job = 1;

// Keeps an anchor ARCore handed over; its id, or 0 when the table is full.
static int64_t keepAnchor(ArAnchor *anchor) {
    if (!anchor) return 0;
    if (anchor_count == MAX_ANCHORS) {
        p_ArAnchor_detach(session, anchor);
        p_ArAnchor_release(anchor);
        return 0;
    }
    anchors[anchor_count].anchor = anchor;
    anchors[anchor_count].id = next_id++;
    return anchors[anchor_count++].id;
}

static double addJob(int kind, ArFuture *future) {
    if (!future) return 0;
    if (job_count == MAX_JOBS) {
        p_ArFuture_release(future);
        return 0;
    }
    jobs[job_count].id = next_job++;
    jobs[job_count].kind = kind;
    jobs[job_count].future = future;
    return (double)jobs[job_count++].id;
}

// Hosts the anchor with that id for ttl_days; a job id, 0 when it cannot start.
double ar_native_cloud_host(double anchor_id, int ttl_days) {
    pthread_mutex_lock(&session_lock);
    double job = 0;
    for (int i = 0; session && (flags & FLAG_CLOUD) && i < anchor_count; i++) {
        if (anchors[i].id != (int64_t)anchor_id) continue;
        ArHostCloudAnchorFuture *f = NULL;
        if (p_ArSession_hostCloudAnchorAsync(session, anchors[i].anchor, ttl_days, NULL, NULL, &f) == AR_SUCCESS_)
            job = addJob(1, (ArFuture *)f);
        break;
    }
    pthread_mutex_unlock(&session_lock);
    return job;
}

// Resolves a cloud anchor id; a job id, 0 when it cannot start.
double ar_native_cloud_resolve(const char *cloud_id) {
    pthread_mutex_lock(&session_lock);
    double job = 0;
    ArResolveCloudAnchorFuture *f = NULL;
    if (session && (flags & FLAG_CLOUD) && cloud_id &&
        p_ArSession_resolveCloudAnchorAsync(session, cloud_id, NULL, NULL, &f) == AR_SUCCESS_)
        job = addJob(2, (ArFuture *)f);
    pthread_mutex_unlock(&session_lock);
    return job;
}

// An anchor at a place on Earth (WGS84, altitude above the ellipsoid, east-up-south
// quaternion); its id, 0 when Earth is not tracking.
double ar_native_earth_anchor(double lat, double lng, double alt, float *quat) {
    pthread_mutex_lock(&session_lock);
    double id = 0;
    ArEarth *earth = NULL;
    if (session && (flags & FLAG_GEO)) p_ArSession_acquireEarth(session, &earth);
    if (earth) {
        ArAnchor *anchor = NULL;
        if (p_ArEarth_acquireNewAnchor(session, earth, lat, lng, alt, quat, &anchor) == AR_SUCCESS_) id = (double)keepAnchor(anchor);
        p_ArTrackable_release((ArTrackable *)earth);
    }
    pthread_mutex_unlock(&session_lock);
    return id;
}

// An anchor at a place on Earth `alt` meters above the terrain (rooftop 0) or a rooftop
// (rooftop 1); a job id, 0 when it cannot start.
double ar_native_earth_resolve(int rooftop, double lat, double lng, double alt, float *quat) {
    pthread_mutex_lock(&session_lock);
    double job = 0;
    ArEarth *earth = NULL;
    if (session && (flags & FLAG_GEO)) p_ArSession_acquireEarth(session, &earth);
    if (earth) {
        if (rooftop) {
            ArResolveAnchorOnRooftopFuture *f = NULL;
            if (p_ArEarth_resolveAnchorOnRooftopAsync(session, earth, lat, lng, alt, quat, NULL, NULL, &f) == AR_SUCCESS_)
                job = addJob(4, (ArFuture *)f);
        } else {
            ArResolveAnchorOnTerrainFuture *f = NULL;
            if (p_ArEarth_resolveAnchorOnTerrainAsync(session, earth, lat, lng, alt, quat, NULL, NULL, &f) == AR_SUCCESS_)
                job = addJob(3, (ArFuture *)f);
        }
        p_ArTrackable_release((ArTrackable *)earth);
    }
    pthread_mutex_unlock(&session_lock);
    return job;
}

// Asks whether Google's VPS covers a place; a job id (done: VPS there; failed: info[0] 2 for
// not there, negative for an error), 0 when it cannot start.
double ar_native_vps_check(double lat, double lng) {
    pthread_mutex_lock(&session_lock);
    double job = 0;
    ArVpsAvailabilityFuture *f = NULL;
    if (session && p_ArSession_checkVpsAvailabilityAsync(session, lat, lng, NULL, NULL, &f) == AR_SUCCESS_)
        job = addJob(5, (ArFuture *)f);
    pthread_mutex_unlock(&session_lock);
    return job;
}

// Where a job stands: 0 pending, 1 done (info[1] the anchor's id for a resolve, `text` the
// cloud anchor id for a host), 2 failed (info[0] ARCore's state for it), 3 no such job. A
// finished job is reported once.
int ar_native_job(double job, double *info, char *text, int cap) {
    pthread_mutex_lock(&session_lock);
    int result = 3;
    info[0] = 0;
    info[1] = 0;
    if (cap > 0) text[0] = 0;
    for (int i = 0; session && i < job_count; i++) {
        if (jobs[i].id != (int64_t)job) continue;
        Job *j = &jobs[i];
        ArFutureState fs = AR_FUTURE_STATE_PENDING_;
        p_ArFuture_getState(session, j->future, &fs);
        if (fs == AR_FUTURE_STATE_PENDING_) {
            result = 0;
            break;
        }
        int32_t state = 0;
        int ok = 0;
        ArAnchor *anchor = NULL;
        if (fs == AR_FUTURE_STATE_DONE_) {
            if (j->kind == 1) {
                p_ArHostCloudAnchorFuture_getResultCloudAnchorState(session, (ArHostCloudAnchorFuture *)j->future, &state);
                ok = state == AR_CLOUD_ANCHOR_STATE_SUCCESS_;
                if (ok) {
                    char *cloud = NULL;
                    p_ArHostCloudAnchorFuture_acquireResultCloudAnchorId(session, (ArHostCloudAnchorFuture *)j->future, &cloud);
                    if (cloud) {
                        snprintf(text, (size_t)cap, "%s", cloud);
                        p_ArString_release(cloud);
                    }
                }
            } else if (j->kind == 2) {
                p_ArResolveCloudAnchorFuture_getResultCloudAnchorState(session, (ArResolveCloudAnchorFuture *)j->future, &state);
                ok = state == AR_CLOUD_ANCHOR_STATE_SUCCESS_;
                if (ok) p_ArResolveCloudAnchorFuture_acquireResultAnchor(session, (ArResolveCloudAnchorFuture *)j->future, &anchor);
            } else if (j->kind == 3) {
                p_ArResolveAnchorOnTerrainFuture_getResultTerrainAnchorState(session, (ArResolveAnchorOnTerrainFuture *)j->future, &state);
                ok = state == AR_TERRAIN_ANCHOR_STATE_SUCCESS_;
                if (ok) p_ArResolveAnchorOnTerrainFuture_acquireResultAnchor(session, (ArResolveAnchorOnTerrainFuture *)j->future, &anchor);
            } else if (j->kind == 5) {
                p_ArVpsAvailabilityFuture_getResult(session, (ArVpsAvailabilityFuture *)j->future, &state);
                ok = state == AR_VPS_AVAILABILITY_AVAILABLE_;
            } else {
                p_ArResolveAnchorOnRooftopFuture_getResultRooftopAnchorState(session, (ArResolveAnchorOnRooftopFuture *)j->future, &state);
                ok = state == AR_ROOFTOP_ANCHOR_STATE_SUCCESS_;
                if (ok) p_ArResolveAnchorOnRooftopFuture_acquireResultAnchor(session, (ArResolveAnchorOnRooftopFuture *)j->future, &anchor);
            }
        }
        if (anchor) info[1] = (double)keepAnchor(anchor);
        info[0] = state;
        result = ok ? 1 : 2;
        p_ArFuture_release(j->future);
        jobs[i] = jobs[--job_count];
        break;
    }
    pthread_mutex_unlock(&session_lock);
    return result;
}

// The lent frame's Earth values (readEarth's); 0 when it has none.
int ar_native_earth(double *info) {
    Snapshot *s = lentSnapshot();
    if (!s || !s->has_earth) return 0;
    memcpy(info, s->earth, sizeof(s->earth));
    return 1;
}

void ar_native_anchor_remove(double id) {
    pthread_mutex_lock(&session_lock);
    for (int i = 0; i < anchor_count; i++) {
        if (anchors[i].id != (int64_t)id) continue;
        if (session) p_ArAnchor_detach(session, anchors[i].anchor);
        p_ArAnchor_release(anchors[i].anchor);
        anchors[i] = anchors[--anchor_count];
        break;
    }
    pthread_mutex_unlock(&session_lock);
}

// A new set of image targets; the session takes them when it is configured next (the
// ar_native_session call that follows).
void ar_native_images(int count, int tracked) {
    (void)tracked;
    if (count < 0) count = 0;
    if (count > MAX_IMAGES) count = MAX_IMAGES;
    pthread_mutex_lock(&session_lock);
    for (int i = 0; i < target_count; i++) {
        free(targets[i].gray);
        free(targets[i].path);
    }
    memset(targets, 0, sizeof(targets));
    for (int i = 0; i < count; i++) targets[i].state = 2;
    target_count = count;
    db_count = 0;
    pthread_mutex_unlock(&session_lock);
}

void ar_native_image_add(int index, uint8_t *gray, int width, int height, const char *path, double physical_width) {
    pthread_mutex_lock(&session_lock);
    if (index >= 0 && index < target_count) {
        Target *target = &targets[index];
        target->meters = physical_width;
        if (gray && width > 0 && height > 0 && (target->gray = malloc((size_t)width * height)) != NULL) {
            memcpy(target->gray, gray, (size_t)width * height);
            target->width = width;
            target->height = height;
        } else if (path) target->path = strdup(path);
        if (physical_width > 0 && (target->gray || target->path)) target->state = 0;
    }
    pthread_mutex_unlock(&session_lock);
}

int ar_native_image_state(int index) {
    pthread_mutex_lock(&session_lock);
    int state = index >= 0 && index < target_count ? targets[index].state : 2;
    pthread_mutex_unlock(&session_lock);
    return state;
}

int ar_native_face_count(void) {
    Snapshot *s = lentSnapshot();
    return s ? s->face_count : 0;
}

void ar_native_face(int index, double *info) {
    Snapshot *s = lentSnapshot();
    memset(info, 0, sizeof(double) * 160);
    if (s && index >= 0 && index < s->face_count) memcpy(info, s->faces[index], sizeof(double) * 160);
}

// ARCore has no body tracking.
int ar_native_body_count(void) { return 0; }

void ar_native_body(int index, double *info) { memset(info, 0, sizeof(double) * 21); }

int ar_native_body_joint(int joint, char *name, int capacity) {
    if (capacity > 0) name[0] = 0;
    return -2;
}

int ar_native_tracked_image_count(void) {
    Snapshot *s = lentSnapshot();
    return s ? s->image_count : 0;
}

void ar_native_tracked_image(int index, double *info) {
    Snapshot *s = lentSnapshot();
    memset(info, 0, sizeof(double) * 22);
    if (s && index >= 0 && index < s->image_count) memcpy(info, s->images[index], sizeof(double) * 22);
}
