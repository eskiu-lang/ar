// ar_query.inc: what the Eskiu side reads each frame: camera, planes, anchors, depth, points and hits. Part of ar.c, which #includes it (one translation unit).

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
void ar_native_depth_model_tuning(float scale, int fps) { (void)scale; (void)fps; }

// 1 when the device gives depth (ARCore's Depth API), 0 when not, -1 before a session says.
int ar_native_depth_supported(void) {
    if (!session) return -1;
    int32_t ok = 0;
    p_ArSession_isDepthModeSupported(session, AR_DEPTH_MODE_AUTOMATIC_, &ok);
    return ok ? 1 : 0;
}
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
