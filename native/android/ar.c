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

#include "ar_session.inc"
#include "ar_query.inc"
#include "ar_cloud.inc"
#include "ar_targets.inc"
