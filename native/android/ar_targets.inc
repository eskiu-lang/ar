// ar_targets.inc: image targets, faces and the other tracked things. Part of ar.c, which #includes it (one translation unit).

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
