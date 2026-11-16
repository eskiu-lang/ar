// ar_arcore.inc: ARCore for iOS: cloud anchors, Earth and semantics. Part of ar.m, which #includes it (one translation unit).

// --- ARCore for iOS: cloud anchors, Earth and semantics ---

#ifdef ESKIU_ARCORE
// The anchors ARCore made (resolved, on Earth), by their identifier, with the ids Eskiu sees.
static NSMutableDictionary<NSUUID *, NSNumber *> *garIds;
static NSMutableDictionary<NSNumber *, GARAnchor *> *garById;
// Jobs: id to @[state, code, anchor id, cloud id] (state 0 running, 1 done, 2 failed).
static NSMutableDictionary<NSNumber *, NSMutableArray *> *jobs;
static int64_t nextJob = 1;
static uint8_t *semanticCopy;
static size_t semanticCap;
#ifdef ESKIU_ARCORE_GEO
static CLLocationManager *location;
static int garFlags;

// Earth needs the location: ARCore refuses a geospatial configuration until it is granted, so
// the session is configured without Earth first and again once the user allows it.
@interface EskiuGarLocation : NSObject <CLLocationManagerDelegate>
@end
@implementation EskiuGarLocation
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager {
    CLAuthorizationStatus s = manager.authorizationStatus;
    if ((s == kCLAuthorizationStatusAuthorizedWhenInUse || s == kCLAuthorizationStatusAuthorizedAlways) && gar &&
        (garFlags & 4096))
        dispatch_async(dispatch_get_main_queue(), ^{ garConfigure(garFlags); });
}
@end
static EskiuGarLocation *locationDelegate;

static BOOL locationGranted(void) {
    if (!location) {
        location = [CLLocationManager new];
        locationDelegate = [EskiuGarLocation new];
        location.delegate = locationDelegate;
    }
    CLAuthorizationStatus s = location.authorizationStatus;
    if (s == kCLAuthorizationStatusNotDetermined) [location requestWhenInUseAuthorization];
    return (s == kCLAuthorizationStatusAuthorizedWhenInUse || s == kCLAuthorizationStatusAuthorizedAlways) &&
           location.accuracyAuthorization == CLAccuracyAuthorizationFullAccuracy;
}
#endif

// Makes the ARCore session for the flags asked for (2048 cloud anchors, 4096 Earth, 8192
// semantics) with the app's API key (GARAPIKey in Info.plist), or drops it.
static void garConfigure(int flags) {
    if (!(flags & (2048 | 4096 | 8192))) {
        gar = nil;
        return;
    }
    if (!garIds) {
        garIds = [NSMutableDictionary new];
        garById = [NSMutableDictionary new];
        jobs = [NSMutableDictionary new];
    }
    if (!gar) {
        NSString *key = [NSBundle.mainBundle objectForInfoDictionaryKey:@"GARAPIKey"];
        NSError *error = nil;
        gar = key.length ? [GARSession sessionWithAPIKey:key bundleIdentifier:nil error:&error] : nil;
        if (!gar) {
            NSLog(@"ar: no ARCore session (%@)", key.length ? error.localizedDescription : @"GARAPIKey missing from Info.plist");
            return;
        }
    }
    GARSessionConfiguration *config = [GARSessionConfiguration new];
#ifdef ESKIU_ARCORE_CLOUD
    if (flags & 2048) config.cloudAnchorMode = GARCloudAnchorModeEnabled;
#endif
#ifdef ESKIU_ARCORE_GEO
    garFlags = flags;
    if ((flags & 4096) && [gar isGeospatialModeSupported:GARGeospatialModeEnabled]) {
        if (locationGranted()) config.geospatialMode = GARGeospatialModeEnabled;
        else NSLog(@"ar: Earth waits for the location permission (full accuracy)");
    }
#endif
#ifdef ESKIU_ARCORE_SEMANTICS
    if ((flags & 8192) && [gar isSemanticModeSupported:GARSemanticModeEnabled]) config.semanticMode = GARSemanticModeEnabled;
#endif
    NSError *error = nil;
    [gar setConfiguration:config error:&error];
    if (error) NSLog(@"ar: ARCore refused the configuration (%@)", error.localizedDescription);
}

static double newJob(void) {
    NSNumber *id = @(nextJob++);
    @synchronized(jobs) {
        jobs[id] = [@[ @0, @0, @0, @"" ] mutableCopy];
    }
    return id.doubleValue;
}

static void finishJob(double job, BOOL ok, NSInteger code, GARAnchor *anchor, NSString *cloud) {
    NSNumber *anchorId = @0;
    if (anchor) {
        anchorId = @(nextId++);
        @synchronized(garIds) {
            garIds[anchor.identifier] = anchorId;
            garById[anchorId] = anchor;
        }
    }
    @synchronized(jobs) {
        NSMutableArray *j = jobs[@((int64_t)job)];
        if (!j) return;
        j[0] = ok ? @1 : @2;
        j[1] = @(code);
        j[2] = anchorId;
        j[3] = cloud ?: @"";
    }
}

static simd_quatf quatOf(float *q) { return simd_quaternion(q[0], q[1], q[2], q[3]); }

static int garAnchorCount(void) {
    if (!garLent) return 0;
    int n = 0;
    @synchronized(garIds) {
        for (GARAnchor *a in garLent.anchors) if (garIds[a.identifier]) n++;
    }
    return n;
}

static void garAnchor(int index, double *info) {
    if (!garLent || index < 0) return;
    int n = 0;
    @synchronized(garIds) {
        for (GARAnchor *a in garLent.anchors) {
            NSNumber *id = garIds[a.identifier];
            if (!id) continue;
            if (n++ != index) continue;
            info[0] = id.doubleValue;
            info[1] = a.trackingState == GARTrackingStateTracking ? 2 : a.trackingState == GARTrackingStatePaused ? 1 : 0;
            putMatrix(&info[2], a.transform);
            return;
        }
    }
}
#else
static int garAnchorCount(void) { return 0; }
static void garAnchor(int index, double *info) {}
#endif

double ar_native_cloud_host(double anchor, int ttl_days) {
#ifdef ESKIU_ARCORE_CLOUD
    ARAnchor *a = anchorsById[@((int64_t)anchor)];
    if (!gar || !a) return 0;
    double job = newJob();
    NSError *error = nil;
    GARHostCloudAnchorFuture *f = [gar hostCloudAnchor:a TTLDays:ttl_days completionHandler:^(NSString *cloud, GARCloudAnchorState state) {
        finishJob(job, state == GARCloudAnchorStateSuccess, state, nil, cloud);
    } error:&error];
    if (!f) finishJob(job, NO, error.code, nil, nil);
    return job;
#else
    return 0;
#endif
}

double ar_native_cloud_resolve(const char *cloud_id) {
#ifdef ESKIU_ARCORE_CLOUD
    if (!gar || !cloud_id) return 0;
    double job = newJob();
    NSError *error = nil;
    GARResolveCloudAnchorFuture *f = [gar resolveCloudAnchorWithIdentifier:[NSString stringWithUTF8String:cloud_id]
        completionHandler:^(GARAnchor *a, GARCloudAnchorState state) {
            finishJob(job, state == GARCloudAnchorStateSuccess && a, state, a, nil);
        } error:&error];
    if (!f) finishJob(job, NO, error.code, nil, nil);
    return job;
#else
    return 0;
#endif
}

double ar_native_earth_anchor(double lat, double lng, double alt, float *quaternion) {
#ifdef ESKIU_ARCORE_GEO
    if (!gar) return 0;
    GARAnchor *a = [gar createAnchorWithCoordinate:CLLocationCoordinate2DMake(lat, lng) altitude:alt
                                eastUpSouthQAnchor:quatOf(quaternion) error:nil];
    if (!a) return 0;
    NSNumber *id = @(nextId++);
    @synchronized(garIds) {
        garIds[a.identifier] = id;
        garById[id] = a;
    }
    return id.doubleValue;
#else
    return 0;
#endif
}

double ar_native_earth_resolve(int rooftop, double lat, double lng, double alt, float *quaternion) {
#ifdef ESKIU_ARCORE_GEO
    if (!gar) return 0;
    double job = newJob();
    NSError *error = nil;
    CLLocationCoordinate2D at = CLLocationCoordinate2DMake(lat, lng);
    id f;
    if (rooftop) {
        f = [gar createAnchorWithCoordinate:at altitudeAboveRooftop:alt eastUpSouthQAnchor:quatOf(quaternion)
            completionHandler:^(GARAnchor *a, GARRooftopAnchorState state) {
                finishJob(job, state == GARRooftopAnchorStateSuccess && a, state, a, nil);
            } error:&error];
    } else {
        f = [gar createAnchorWithCoordinate:at altitudeAboveTerrain:alt eastUpSouthQAnchor:quatOf(quaternion)
            completionHandler:^(GARAnchor *a, GARTerrainAnchorState state) {
                finishJob(job, state == GARTerrainAnchorStateSuccess && a, state, a, nil);
            } error:&error];
    }
    if (!f) finishJob(job, NO, error.code, nil, nil);
    return job;
#else
    return 0;
#endif
}

double ar_native_vps_check(double lat, double lng) {
#ifdef ESKIU_ARCORE_GEO
    if (!gar) return 0;
    double job = newJob();
    [gar checkVPSAvailabilityAtCoordinate:CLLocationCoordinate2DMake(lat, lng) completionHandler:^(GARVPSAvailability v) {
        finishJob(job, v == GARVPSAvailabilityAvailable, v, nil, nil);
    }];
    return job;
#else
    return 0;
#endif
}

int ar_native_job(double job, double *info, char *text, int capacity) {
    info[0] = 0;
    info[1] = 0;
    if (capacity > 0) text[0] = 0;
#ifdef ESKIU_ARCORE
    if (!jobs) return 3;
    @synchronized(jobs) {
        NSMutableArray *j = jobs[@((int64_t)job)];
        if (!j) return 3;
        int state = [j[0] intValue];
        if (state == 0) return 0;
        info[0] = [j[1] doubleValue];
        info[1] = [j[2] doubleValue];
        if (capacity > 0) snprintf(text, (size_t)capacity, "%s", [(NSString *)j[3] UTF8String]);
        [jobs removeObjectForKey:@((int64_t)job)];
        return state;
    }
#else
    return 3;
#endif
}

int ar_native_earth(double *info) {
#ifdef ESKIU_ARCORE_GEO
    GAREarth *earth = garLent.earth;
    if (!earth) return 0;
    for (int i = 0; i < 14; i++) info[i] = 0;
    info[0] = earth.earthState;
    info[1] = earth.trackingState == GARTrackingStateTracking ? 2 : earth.trackingState == GARTrackingStatePaused ? 1 : 0;
    GARGeospatialTransform *t = earth.cameraGeospatialTransform;
    if (t && earth.trackingState == GARTrackingStateTracking) {
        info[2] = t.coordinate.latitude;
        info[3] = t.coordinate.longitude;
        info[4] = t.altitude;
        info[5] = t.horizontalAccuracy;
        info[6] = t.verticalAccuracy;
        simd_quatf q = t.eastUpSouthQTarget;
        for (int k = 0; k < 4; k++) info[7 + k] = q.vector[k];
        info[11] = t.orientationYawAccuracy;
    }
    return 1;
#else
    return 0;
#endif
}

// 1 when ARCore's Scene Semantics runs on this device (the ARCoreSemantics pod linked), 0 when
// not, -1 before ARCore's session exists.
int ar_native_semantics_supported(void) {
#ifdef ESKIU_ARCORE_SEMANTICS
    if (!gar) return -1;
    return [gar isSemanticModeSupported:GARSemanticModeEnabled] ? 1 : 0;
#else
    return 0;
#endif
}

int ar_native_semantics(int64_t *info) {
#ifdef ESKIU_ARCORE_SEMANTICS
    CVPixelBufferRef labels = garLent.semanticImage;
    if (!labels || CVPixelBufferLockBaseAddress(labels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
    size_t w = CVPixelBufferGetWidth(labels), h = CVPixelBufferGetHeight(labels), row = CVPixelBufferGetBytesPerRow(labels);
    if (semanticCap < w * h * 2) {
        free(semanticCopy);
        semanticCopy = malloc(w * h * 2);
        semanticCap = w * h * 2;
    }
    const uint8_t *src = CVPixelBufferGetBaseAddress(labels);
    for (size_t y = 0; y < h; y++) memcpy(semanticCopy + y * w, src + y * row, w);
    CVPixelBufferUnlockBaseAddress(labels, kCVPixelBufferLock_ReadOnly);
    int has_confidence = 0;
    CVPixelBufferRef conf = garLent.semanticConfidenceImage;
    if (conf && CVPixelBufferGetWidth(conf) == w && CVPixelBufferGetHeight(conf) == h &&
        CVPixelBufferLockBaseAddress(conf, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
        const uint8_t *c = CVPixelBufferGetBaseAddress(conf);
        size_t crow = CVPixelBufferGetBytesPerRow(conf);
        for (size_t y = 0; y < h; y++) memcpy(semanticCopy + w * h + y * w, c + y * crow, w);
        CVPixelBufferUnlockBaseAddress(conf, kCVPixelBufferLock_ReadOnly);
        has_confidence = 1;
    }
    info[0] = (int64_t)w;
    info[1] = (int64_t)h;
    info[2] = (int64_t)(intptr_t)semanticCopy;
    info[3] = has_confidence ? (int64_t)(intptr_t)(semanticCopy + w * h) : 0;
    return 1;
#else
    return 0;
#endif
}
