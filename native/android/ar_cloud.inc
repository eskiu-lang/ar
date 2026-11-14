// ar_cloud.inc: cloud anchors and Earth: ARCore's futures, kept as jobs the Eskiu side polls. Part of ar.c, which #includes it (one translation unit).

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
