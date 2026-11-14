// ar_session.inc: the session thread, its configuration and the JNI entry points. Part of ar.c, which #includes it (one translation unit).

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
    if (flags & FLAG_GEO) {
        p_ArSession_isGeospatialModeSupported(session, AR_GEOSPATIAL_MODE_ENABLED_, &geo);
        if (!geo) fprintf(stderr, "ar: ARCore's geospatial mode is not supported on this device\n");
    }
    p_ArConfig_setGeospatialMode(session, config, geo ? AR_GEOSPATIAL_MODE_ENABLED_ : 0);
    int32_t semantic = 0;
    if (flags & FLAG_SEMANTICS) p_ArSession_isSemanticModeSupported(session, AR_SEMANTIC_MODE_ENABLED_, &semantic);
    p_ArConfig_setSemanticMode(session, config, semantic ? AR_SEMANTIC_MODE_ENABLED_ : 0);
    ArAugmentedImageDatabase *database = buildDatabase();
    p_ArConfig_setAugmentedImageDatabase(session, config, database);
    if (database) p_ArAugmentedImageDatabase_destroy(database);
    ArStatus result = p_ArSession_configure(session, config);
    p_ArConfig_destroy(config);
    if (result != AR_SUCCESS_) fprintf(stderr, "ar: ARCore refused the configuration (%d)\n", (int)result);
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
