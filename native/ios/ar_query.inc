// ar_query.inc: what the Eskiu side reads each frame: planes, meshes, anchors, images, objects and people. Part of ar.m, which #includes it (one translation unit).

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
#ifdef ESKIU_ARCORE
    garLent = nil;
#endif
    lentPlanes = nil;
    lentAnchors = nil;
    lentImages = nil;
    lentObjects = nil;
    lentMeshes = nil;
    lentBodies = nil;
}

int ar_native_plane_count(void) { return (int)lentPlanes.count; }

int ar_native_mesh_count(void) { return (int)lentMeshes.count; }

// Faces of the lent frame, read by the face package's provider (none without it).
int ar_native_face_count(void) {
    return lent && faceProvider ? faceProvider->count(lent) : 0;
}

void ar_native_face(int index, double *info) {
    for (int i = 0; i < 160; i++) info[i] = 0;
    if (lent && faceProvider) faceProvider->face(lent, index, info);
}

int ar_native_body_count(void) { return (int)lentBodies.count; }

// A body of the lent frame: id, pose, joint count, the joints' model transforms where ARKit
// keeps them (read in place while lent), estimated scale, tracking.
void ar_native_body(int index, double *info) {
    for (int i = 0; i < 21; i++) info[i] = 0;
    if (index < 0 || index >= (int)lentBodies.count) return;
    if (@available(iOS 13.0, *)) {
        ARBodyAnchor *body = lentBodies[index];
        info[0] = (double)planeId(body.identifier);
        putMatrix(&info[1], body.transform);
        info[17] = body.skeleton.definition.jointCount;
        info[18] = (double)(intptr_t)body.skeleton.jointModelTransforms;
        info[19] = 1.0;
        if (@available(iOS 14.0, *)) info[19] = body.estimatedScaleFactor;
        info[20] = body.isTracked ? 2 : 0;
    }
}

// The name of joint j of ARKit's body skeleton and its parent (-1 for the root, -2 for no
// such joint).
int ar_native_body_joint(int joint, char *name, int capacity) {
    if (capacity > 0) name[0] = 0;
    if (@available(iOS 13.0, *)) {
        ARSkeletonDefinition *d = ARSkeletonDefinition.defaultBody3DSkeletonDefinition;
        if (joint < 0 || joint >= (int)d.jointCount) return -2;
        if (capacity > 0) snprintf(name, capacity, "%s", d.jointNames[joint].UTF8String);
        return d.parentIndices[joint].intValue;
    }
    return -2;
}

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

static int garAnchorCount(void);
static void garAnchor(int index, double *info);

int ar_native_anchor_count(void) { return (int)lentAnchors.count + garAnchorCount(); }

void ar_native_anchor(int index, double *info) {
    for (int i = 0; i < 18; i++) info[i] = 0;
    if (index >= (int)lentAnchors.count) {
        garAnchor(index - (int)lentAnchors.count, info);
        return;
    }
    if (index < 0) return;
    ARAnchor *anchor = lentAnchors[index];
    info[0] = (double)anchorIds[anchor.identifier].longLongValue;
    info[1] = trackingOf(lent.camera);
    putMatrix(&info[2], anchor.transform);
}

// A new set of object targets, loaded one by one by ar_native_object_add; a running session
// looks for them from its next configuration.
void ar_native_objects(int count) {
    objectTargets = [NSMutableArray new];
    if (!objectIds) objectIds = [NSMutableDictionary new];
    for (int i = 0; i < count; i++) [objectTargets addObject:NSNull.null];
}

// Loads target `index` from an .arobject file; 1 when it loaded.
int ar_native_object_add(int index, const char *path) {
    if (index < 0 || index >= (int)objectTargets.count || !path) return 0;
    NSError *error = nil;
    ARReferenceObject *object = [[ARReferenceObject alloc] initWithArchiveURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
                                                                        error:&error];
    if (!object) {
        NSLog(@"ar: object target %s does not load: %@", path, error.localizedDescription);
        return 0;
    }
    object.name = [NSString stringWithFormat:@"%d", index];
    objectTargets[index] = object;
    if (running) runConfiguration();
    return 1;
}

int ar_native_tracked_object_count(void) { return (int)lentObjects.count; }

// An object anchor: the reference object's origin, and its points' center and extent.
void ar_native_tracked_object(int index, double *info) {
    for (int i = 0; i < 24; i++) info[i] = 0;
    if (index < 0 || index >= (int)lentObjects.count) return;
    ARObjectAnchor *anchor = lentObjects[index];
    NSNumber *id = objectIds[anchor.identifier];
    if (!id) {
        id = @(nextId++);
        objectIds[anchor.identifier] = id;
    }
    info[0] = id.doubleValue;
    info[1] = anchor.referenceObject.name ? anchor.referenceObject.name.intValue : -1;
    putMatrix(&info[2], anchor.transform);
    simd_float3 c = anchor.referenceObject.center;
    simd_float3 e = anchor.referenceObject.extent;
    info[18] = c.x;
    info[19] = c.y;
    info[20] = c.z;
    info[21] = e.x;
    info[22] = e.y;
    info[23] = e.z;
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
