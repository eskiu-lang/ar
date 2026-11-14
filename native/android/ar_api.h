// The part of ARCore's C API (arcore_c_api.h, ARCore SDK 1.57.0) that ar.c calls, declared
// here so the app's library builds with the NDK alone: ar.c opens libarcore_sdk_c.so (from
// the com.google.ar:core AAR, which Gradle packs into the APK) at run time and looks each
// function up by name. AR_API lists each one as X(result, name, (parameters)) with the
// header's own types; tools/check_arcore_api.sh compiles the list against Google's header.
#ifndef ESKIU_AR_API_H
#define ESKIU_AR_API_H
#include <stdint.h>

#ifndef ARCORE_C_API_H_
typedef struct ArSession_ ArSession;
typedef struct ArConfig_ ArConfig;
typedef struct ArCameraConfig_ ArCameraConfig;
typedef struct ArCameraConfigList_ ArCameraConfigList;
typedef struct ArCameraConfigFilter_ ArCameraConfigFilter;
typedef struct ArPose_ ArPose;
typedef struct ArCamera_ ArCamera;
typedef struct ArCameraIntrinsics_ ArCameraIntrinsics;
typedef struct ArFrame_ ArFrame;
typedef struct ArLightEstimate_ ArLightEstimate;
typedef struct ArPointCloud_ ArPointCloud;
typedef struct ArImage_ ArImage;
typedef struct ArTrackable_ ArTrackable;
typedef struct ArTrackableList_ ArTrackableList;
typedef struct ArPlane_ ArPlane;
typedef struct ArAnchor_ ArAnchor;
typedef struct ArHitResult_ ArHitResult;
typedef struct ArHitResultList_ ArHitResultList;
typedef struct ArAugmentedImage_ ArAugmentedImage;
typedef struct ArAugmentedImageDatabase_ ArAugmentedImageDatabase;
typedef int32_t ArAugmentedImageTrackingMethod;
typedef int32_t ArStatus;
typedef int32_t ArAvailability;
typedef int32_t ArInstallStatus;
typedef int32_t ArDepthMode;
typedef int32_t ArFocusMode;
typedef int32_t ArLightEstimateState;
typedef int32_t ArLightEstimationMode;
typedef int32_t ArPlaneFindingMode;
typedef int32_t ArPlaneType;
typedef int32_t ArTextureUpdateMode;
typedef int32_t ArTrackableType;
typedef int32_t ArTrackingFailureReason;
typedef int32_t ArTrackingState;
typedef int32_t ArUpdateMode;
typedef struct ArEarth_ ArEarth;
typedef struct ArGeospatialPose_ ArGeospatialPose;
typedef struct ArFuture_ ArFuture;
typedef struct ArHostCloudAnchorFuture_ ArHostCloudAnchorFuture;
typedef struct ArResolveCloudAnchorFuture_ ArResolveCloudAnchorFuture;
typedef struct ArResolveAnchorOnTerrainFuture_ ArResolveAnchorOnTerrainFuture;
typedef struct ArResolveAnchorOnRooftopFuture_ ArResolveAnchorOnRooftopFuture;
typedef int32_t ArCloudAnchorMode;
typedef int32_t ArGeospatialMode;
typedef int32_t ArFutureState;
typedef int32_t ArCloudAnchorState;
typedef int32_t ArEarthState;
typedef int32_t ArTerrainAnchorState;
typedef int32_t ArRooftopAnchorState;
typedef int32_t ArSemanticMode;
typedef struct ArVpsAvailabilityFuture_ ArVpsAvailabilityFuture;
typedef int32_t ArVpsAvailability;
typedef void (*ArVpsAvailabilityCallback)(void *context, ArVpsAvailability availability);
typedef void (*ArHostCloudAnchorCallback)(void *context, char *cloud_anchor_id, ArCloudAnchorState cloud_anchor_state);
typedef void (*ArResolveCloudAnchorCallback)(void *context, ArAnchor *anchor, ArCloudAnchorState cloud_anchor_state);
typedef void (*ArResolveAnchorOnTerrainCallback)(void *context, ArAnchor *anchor, ArTerrainAnchorState terrain_anchor_state);
typedef void (*ArResolveAnchorOnRooftopCallback)(void *context, ArAnchor *anchor, ArRooftopAnchorState rooftop_anchor_state);
#endif

// The values ar.c uses, as the header defines them.
enum {
    AR_SUCCESS_ = 0,
    AR_UNAVAILABLE_DEVICE_NOT_COMPATIBLE_ = -101,
    AR_UNAVAILABLE_USER_DECLINED_INSTALLATION_ = -105,
    AR_AVAILABILITY_UNSUPPORTED_DEVICE_NOT_CAPABLE_ = 100,
    AR_INSTALL_STATUS_INSTALL_REQUESTED_ = 1,
    AR_TRACKING_STATE_TRACKING_ = 0,
    AR_TRACKING_STATE_PAUSED_ = 1,
    AR_TRACKING_STATE_STOPPED_ = 2,
    AR_TRACKABLE_PLANE_ = 0x41520101,
    AR_TRACKABLE_POINT_ = 0x41520102,
    AR_TRACKABLE_DEPTH_POINT_ = 0x41520111,
    AR_TRACKABLE_AUGMENTED_IMAGE_ = 0x41520104,
    AR_AUGMENTED_IMAGE_TRACKING_METHOD_FULL_TRACKING_ = 1,
    AR_AUGMENTED_IMAGE_TRACKING_METHOD_LAST_KNOWN_POSE_ = 2,
    AR_PLANE_HORIZONTAL_DOWNWARD_FACING_ = 1,
    AR_PLANE_VERTICAL_ = 2,
    AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER_ = 1,
    AR_UPDATE_MODE_LATEST_CAMERA_IMAGE_ = 1,
    AR_FOCUS_MODE_AUTO_ = 1,
    AR_DEPTH_MODE_AUTOMATIC_ = 1,
    AR_LIGHT_ESTIMATION_MODE_AMBIENT_INTENSITY_ = 1,
    AR_LIGHT_ESTIMATION_MODE_ENVIRONMENTAL_HDR_ = 2,
    AR_LIGHT_ESTIMATE_STATE_VALID_ = 1,
    AR_CLOUD_ANCHOR_MODE_ENABLED_ = 1,
    AR_GEOSPATIAL_MODE_ENABLED_ = 2,
    AR_FUTURE_STATE_PENDING_ = 0,
    AR_FUTURE_STATE_CANCELLED_ = 1,
    AR_FUTURE_STATE_DONE_ = 2,
    AR_CLOUD_ANCHOR_STATE_SUCCESS_ = 2,
    AR_TERRAIN_ANCHOR_STATE_SUCCESS_ = 2,
    AR_ROOFTOP_ANCHOR_STATE_SUCCESS_ = 1,
    AR_EARTH_STATE_ENABLED_ = 0,
    AR_SEMANTIC_MODE_ENABLED_ = 1,
    AR_VPS_AVAILABILITY_AVAILABLE_ = 1,
};

#define AR_API \
    X(void, ArCoreApk_checkAvailability, (void *env, void *context, ArAvailability *out_availability)) \
    X(ArStatus, ArCoreApk_requestInstall, (void *env, void *application_activity, int32_t user_requested_install, ArInstallStatus *out_install_status)) \
    X(ArStatus, ArSession_create, (void *env, void *context, ArSession **out_session_pointer)) \
    X(void, ArSession_destroy, (ArSession *session)) \
    X(ArStatus, ArSession_configure, (ArSession *session, const ArConfig *config)) \
    X(ArStatus, ArSession_resume, (ArSession *session)) \
    X(ArStatus, ArSession_pause, (ArSession *session)) \
    X(ArStatus, ArSession_update, (ArSession *session, ArFrame *out_frame)) \
    X(void, ArSession_setDisplayGeometry, (ArSession *session, int32_t rotation, int32_t width, int32_t height)) \
    X(ArStatus, ArSession_acquireNewAnchor, (ArSession *session, const ArPose *pose, ArAnchor **out_anchor)) \
    X(void, ArSession_isDepthModeSupported, (const ArSession *session, ArDepthMode depth_mode, int32_t *out_is_supported)) \
    X(void, ArSession_getAllTrackables, (const ArSession *session, ArTrackableType filter_type, ArTrackableList *out_trackable_list)) \
    X(void, ArSession_getSupportedCameraConfigsWithFilter, (const ArSession *session, const ArCameraConfigFilter *filter, ArCameraConfigList *list)) \
    X(ArStatus, ArSession_setCameraConfig, (const ArSession *session, const ArCameraConfig *camera_config)) \
    X(void, ArConfig_create, (const ArSession *session, ArConfig **out_config)) \
    X(void, ArConfig_destroy, (ArConfig *config)) \
    X(void, ArConfig_setTextureUpdateMode, (const ArSession *session, ArConfig *config, ArTextureUpdateMode texture_update_mode)) \
    X(void, ArConfig_setPlaneFindingMode, (const ArSession *session, ArConfig *config, ArPlaneFindingMode plane_finding_mode)) \
    X(void, ArConfig_setLightEstimationMode, (const ArSession *session, ArConfig *config, ArLightEstimationMode light_estimation_mode)) \
    X(void, ArConfig_setDepthMode, (const ArSession *session, ArConfig *config, ArDepthMode mode)) \
    X(void, ArConfig_setUpdateMode, (const ArSession *session, ArConfig *config, ArUpdateMode update_mode)) \
    X(void, ArConfig_setFocusMode, (const ArSession *session, ArConfig *config, ArFocusMode focus_mode)) \
    X(void, ArCameraConfigFilter_create, (const ArSession *session, ArCameraConfigFilter **out_filter)) \
    X(void, ArCameraConfigFilter_destroy, (ArCameraConfigFilter *filter)) \
    X(void, ArCameraConfigList_create, (const ArSession *session, ArCameraConfigList **out_list)) \
    X(void, ArCameraConfigList_destroy, (ArCameraConfigList *list)) \
    X(void, ArCameraConfigList_getSize, (const ArSession *session, const ArCameraConfigList *list, int32_t *out_size)) \
    X(void, ArCameraConfigList_getItem, (const ArSession *session, const ArCameraConfigList *list, int32_t index, ArCameraConfig *out_camera_config)) \
    X(void, ArCameraConfig_create, (const ArSession *session, ArCameraConfig **out_camera_config)) \
    X(void, ArCameraConfig_destroy, (ArCameraConfig *camera_config)) \
    X(void, ArCameraConfig_getImageDimensions, (const ArSession *session, const ArCameraConfig *camera_config, int32_t *out_width, int32_t *out_height)) \
    X(void, ArFrame_create, (const ArSession *session, ArFrame **out_frame)) \
    X(void, ArFrame_destroy, (ArFrame *frame)) \
    X(void, ArFrame_acquireCamera, (const ArSession *session, const ArFrame *frame, ArCamera **out_camera)) \
    X(void, ArFrame_getTimestamp, (const ArSession *session, const ArFrame *frame, int64_t *out_timestamp_ns)) \
    X(ArStatus, ArFrame_acquireCameraImage, (ArSession *session, ArFrame *frame, ArImage **out_image)) \
    X(ArStatus, ArFrame_acquirePointCloud, (const ArSession *session, const ArFrame *frame, ArPointCloud **out_point_cloud)) \
    X(void, ArFrame_getLightEstimate, (const ArSession *session, const ArFrame *frame, ArLightEstimate *out_light_estimate)) \
    X(ArStatus, ArFrame_acquireDepthImage16Bits, (const ArSession *session, const ArFrame *frame, ArImage **out_depth_image)) \
    X(void, ArFrame_hitTestRay, (const ArSession *session, const ArFrame *frame, const float *ray_origin_3, const float *ray_direction_3, ArHitResultList *hit_result_list)) \
    X(void, ArCamera_getPose, (const ArSession *session, const ArCamera *camera, ArPose *out_pose)) \
    X(void, ArCamera_getTrackingState, (const ArSession *session, const ArCamera *camera, ArTrackingState *out_tracking_state)) \
    X(void, ArCamera_getTrackingFailureReason, (const ArSession *session, const ArCamera *camera, ArTrackingFailureReason *out_tracking_failure_reason)) \
    X(void, ArCamera_getImageIntrinsics, (const ArSession *session, const ArCamera *camera, ArCameraIntrinsics *out_camera_intrinsics)) \
    X(void, ArCamera_release, (ArCamera *camera)) \
    X(void, ArCameraIntrinsics_create, (const ArSession *session, ArCameraIntrinsics **out_camera_intrinsics)) \
    X(void, ArCameraIntrinsics_destroy, (ArCameraIntrinsics *camera_intrinsics)) \
    X(void, ArCameraIntrinsics_getFocalLength, (const ArSession *session, const ArCameraIntrinsics *intrinsics, float *out_fx, float *out_fy)) \
    X(void, ArCameraIntrinsics_getPrincipalPoint, (const ArSession *session, const ArCameraIntrinsics *intrinsics, float *out_cx, float *out_cy)) \
    X(void, ArCameraIntrinsics_getImageDimensions, (const ArSession *session, const ArCameraIntrinsics *intrinsics, int32_t *out_width, int32_t *out_height)) \
    X(void, ArPose_create, (const ArSession *session, const float *pose_raw, ArPose **out_pose)) \
    X(void, ArPose_destroy, (ArPose *pose)) \
    X(void, ArPose_getMatrix, (const ArSession *session, const ArPose *pose, float *out_matrix_col_major_4x4)) \
    X(void, ArImage_getWidth, (const ArSession *session, const ArImage *image, int32_t *out_width)) \
    X(void, ArImage_getHeight, (const ArSession *session, const ArImage *image, int32_t *out_height)) \
    X(void, ArImage_getNumberOfPlanes, (const ArSession *session, const ArImage *image, int32_t *out_num_planes)) \
    X(void, ArImage_getPlanePixelStride, (const ArSession *session, const ArImage *image, int32_t plane_index, int32_t *out_pixel_stride)) \
    X(void, ArImage_getPlaneRowStride, (const ArSession *session, const ArImage *image, int32_t plane_index, int32_t *out_row_stride)) \
    X(void, ArImage_getPlaneData, (const ArSession *session, const ArImage *image, int32_t plane_index, const uint8_t **out_data, int32_t *out_data_length)) \
    X(void, ArImage_release, (ArImage *image)) \
    X(void, ArPointCloud_getNumberOfPoints, (const ArSession *session, const ArPointCloud *point_cloud, int32_t *out_number_of_points)) \
    X(void, ArPointCloud_getData, (const ArSession *session, const ArPointCloud *point_cloud, const float **out_point_cloud_data)) \
    X(void, ArPointCloud_release, (ArPointCloud *point_cloud)) \
    X(void, ArLightEstimate_create, (const ArSession *session, ArLightEstimate **out_light_estimate)) \
    X(void, ArLightEstimate_destroy, (ArLightEstimate *light_estimate)) \
    X(void, ArLightEstimate_getState, (const ArSession *session, const ArLightEstimate *light_estimate, ArLightEstimateState *out_light_estimate_state)) \
    X(void, ArLightEstimate_getPixelIntensity, (const ArSession *session, const ArLightEstimate *light_estimate, float *out_pixel_intensity)) \
    X(void, ArLightEstimate_getColorCorrection, (const ArSession *session, const ArLightEstimate *light_estimate, float *out_color_correction_4)) \
    X(void, ArLightEstimate_getEnvironmentalHdrMainLightDirection, (const ArSession *session, const ArLightEstimate *light_estimate, float *out_direction_3)) \
    X(void, ArLightEstimate_getEnvironmentalHdrMainLightIntensity, (const ArSession *session, const ArLightEstimate *light_estimate, float *out_intensity_3)) \
    X(void, ArLightEstimate_getEnvironmentalHdrAmbientSphericalHarmonics, (const ArSession *session, const ArLightEstimate *light_estimate, float *out_coefficients_27)) \
    X(void, ArTrackableList_create, (const ArSession *session, ArTrackableList **out_trackable_list)) \
    X(void, ArTrackableList_destroy, (ArTrackableList *trackable_list)) \
    X(void, ArTrackableList_getSize, (const ArSession *session, const ArTrackableList *trackable_list, int32_t *out_size)) \
    X(void, ArTrackableList_acquireItem, (const ArSession *session, const ArTrackableList *trackable_list, int32_t index, ArTrackable **out_trackable)) \
    X(void, ArTrackable_getTrackingState, (const ArSession *session, const ArTrackable *trackable, ArTrackingState *out_tracking_state)) \
    X(void, ArTrackable_release, (ArTrackable *trackable)) \
    X(void, ArTrackable_getType, (const ArSession *session, const ArTrackable *trackable, ArTrackableType *out_trackable_type)) \
    X(ArStatus, ArTrackable_acquireNewAnchor, (ArSession *session, ArTrackable *trackable, ArPose *pose, ArAnchor **out_anchor)) \
    X(void, ArPlane_getType, (const ArSession *session, const ArPlane *plane, ArPlaneType *out_plane_type)) \
    X(void, ArPlane_getCenterPose, (const ArSession *session, const ArPlane *plane, ArPose *out_pose)) \
    X(void, ArPlane_getExtentX, (const ArSession *session, const ArPlane *plane, float *out_extent_x)) \
    X(void, ArPlane_getExtentZ, (const ArSession *session, const ArPlane *plane, float *out_extent_z)) \
    X(void, ArPlane_getPolygonSize, (const ArSession *session, const ArPlane *plane, int32_t *out_polygon_size)) \
    X(void, ArPlane_getPolygon, (const ArSession *session, const ArPlane *plane, float *out_polygon_xz)) \
    X(void, ArPlane_acquireSubsumedBy, (const ArSession *session, const ArPlane *plane, ArPlane **out_subsumed_by)) \
    X(void, ArPlane_isPoseInPolygon, (const ArSession *session, const ArPlane *plane, const ArPose *pose, int32_t *out_pose_in_polygon)) \
    X(void, ArHitResultList_create, (const ArSession *session, ArHitResultList **out_hit_result_list)) \
    X(void, ArHitResultList_destroy, (ArHitResultList *hit_result_list)) \
    X(void, ArHitResultList_getSize, (const ArSession *session, const ArHitResultList *hit_result_list, int32_t *out_size)) \
    X(void, ArHitResultList_getItem, (const ArSession *session, const ArHitResultList *hit_result_list, int32_t index, ArHitResult *out_hit_result)) \
    X(void, ArHitResult_create, (const ArSession *session, ArHitResult **out_hit_result)) \
    X(void, ArHitResult_destroy, (ArHitResult *hit_result)) \
    X(void, ArHitResult_getDistance, (const ArSession *session, const ArHitResult *hit_result, float *out_distance)) \
    X(void, ArHitResult_getHitPose, (const ArSession *session, const ArHitResult *hit_result, ArPose *out_pose)) \
    X(void, ArHitResult_acquireTrackable, (const ArSession *session, const ArHitResult *hit_result, ArTrackable **out_trackable)) \
    X(void, ArAnchor_getPose, (const ArSession *session, const ArAnchor *anchor, ArPose *out_pose)) \
    X(void, ArAnchor_getTrackingState, (const ArSession *session, const ArAnchor *anchor, ArTrackingState *out_tracking_state)) \
    X(void, ArAnchor_detach, (ArSession *session, ArAnchor *anchor)) \
    X(void, ArAnchor_release, (ArAnchor *anchor)) \
    X(void, ArAugmentedImageDatabase_create, (const ArSession *session, ArAugmentedImageDatabase **out_augmented_image_database)) \
    X(void, ArAugmentedImageDatabase_destroy, (ArAugmentedImageDatabase *augmented_image_database)) \
    X(ArStatus, ArAugmentedImageDatabase_addImageWithPhysicalSize, (const ArSession *session, ArAugmentedImageDatabase *augmented_image_database, const char *image_name, const uint8_t *image_grayscale_pixels, int32_t image_width_in_pixels, int32_t image_height_in_pixels, int32_t image_stride_in_pixels, float image_width_in_meters, int32_t *out_index)) \
    X(void, ArConfig_setAugmentedImageDatabase, (const ArSession *session, ArConfig *config, const ArAugmentedImageDatabase *augmented_image_database)) \
    X(void, ArAugmentedImage_getCenterPose, (const ArSession *session, const ArAugmentedImage *augmented_image, ArPose *out_pose)) \
    X(void, ArAugmentedImage_getExtentX, (const ArSession *session, const ArAugmentedImage *augmented_image, float *out_extent_x)) \
    X(void, ArAugmentedImage_getExtentZ, (const ArSession *session, const ArAugmentedImage *augmented_image, float *out_extent_z)) \
    X(void, ArAugmentedImage_getIndex, (const ArSession *session, const ArAugmentedImage *augmented_image, int32_t *out_index)) \
    X(void, ArAugmentedImage_getTrackingMethod, (const ArSession *session, const ArAugmentedImage *image, ArAugmentedImageTrackingMethod *out_tracking_method)) \
    X(void, ArConfig_setCloudAnchorMode, (const ArSession *session, ArConfig *config, ArCloudAnchorMode cloud_anchor_mode)) \
    X(void, ArConfig_setGeospatialMode, (const ArSession *session, ArConfig *config, ArGeospatialMode geospatial_mode)) \
    X(void, ArSession_isGeospatialModeSupported, (const ArSession *session, ArGeospatialMode geospatial_mode, int32_t *out_is_supported)) \
    X(void, ArFuture_getState, (const ArSession *session, const ArFuture *future, ArFutureState *out_state)) \
    X(void, ArFuture_release, (ArFuture *future)) \
    X(void, ArString_release, (char *str)) \
    X(ArStatus, ArSession_hostCloudAnchorAsync, (ArSession *session, const ArAnchor *anchor, int32_t ttl_days, void *context, ArHostCloudAnchorCallback callback, ArHostCloudAnchorFuture **out_future)) \
    X(void, ArHostCloudAnchorFuture_acquireResultCloudAnchorId, (const ArSession *session, const ArHostCloudAnchorFuture *future, char **out_cloud_anchor_id)) \
    X(void, ArHostCloudAnchorFuture_getResultCloudAnchorState, (const ArSession *session, const ArHostCloudAnchorFuture *future, ArCloudAnchorState *out_cloud_anchor_state)) \
    X(ArStatus, ArSession_resolveCloudAnchorAsync, (ArSession *session, const char *cloud_anchor_id, void *context, ArResolveCloudAnchorCallback callback, ArResolveCloudAnchorFuture **out_future)) \
    X(void, ArResolveCloudAnchorFuture_acquireResultAnchor, (const ArSession *session, const ArResolveCloudAnchorFuture *future, ArAnchor **out_anchor)) \
    X(void, ArResolveCloudAnchorFuture_getResultCloudAnchorState, (const ArSession *session, const ArResolveCloudAnchorFuture *future, ArCloudAnchorState *out_cloud_anchor_state)) \
    X(void, ArSession_acquireEarth, (const ArSession *session, ArEarth **out_earth)) \
    X(void, ArEarth_getEarthState, (const ArSession *session, const ArEarth *earth, ArEarthState *out_state)) \
    X(void, ArEarth_getCameraGeospatialPose, (const ArSession *session, const ArEarth *earth, ArGeospatialPose *out_camera_geospatial_pose)) \
    X(ArStatus, ArEarth_acquireNewAnchor, (ArSession *session, ArEarth *earth, double latitude, double longitude, double altitude, const float *eus_quaternion_4, ArAnchor **out_anchor)) \
    X(ArStatus, ArEarth_resolveAnchorOnTerrainAsync, (ArSession *session, ArEarth *earth, double latitude, double longitude, double altitude_above_terrain, const float *eus_quaternion_4, void *context, ArResolveAnchorOnTerrainCallback callback, ArResolveAnchorOnTerrainFuture **out_future)) \
    X(void, ArResolveAnchorOnTerrainFuture_acquireResultAnchor, (const ArSession *session, const ArResolveAnchorOnTerrainFuture *future, ArAnchor **out_anchor)) \
    X(void, ArResolveAnchorOnTerrainFuture_getResultTerrainAnchorState, (const ArSession *session, const ArResolveAnchorOnTerrainFuture *future, ArTerrainAnchorState *out_terrain_anchor_state)) \
    X(ArStatus, ArEarth_resolveAnchorOnRooftopAsync, (ArSession *session, ArEarth *earth, double latitude, double longitude, double altitude_above_rooftop, const float *eus_quaternion_4, void *context, ArResolveAnchorOnRooftopCallback callback, ArResolveAnchorOnRooftopFuture **out_future)) \
    X(void, ArResolveAnchorOnRooftopFuture_acquireResultAnchor, (const ArSession *session, const ArResolveAnchorOnRooftopFuture *future, ArAnchor **out_anchor)) \
    X(void, ArResolveAnchorOnRooftopFuture_getResultRooftopAnchorState, (const ArSession *session, const ArResolveAnchorOnRooftopFuture *future, ArRooftopAnchorState *out_rooftop_anchor_state)) \
    X(void, ArGeospatialPose_create, (const ArSession *session, ArGeospatialPose **out_pose)) \
    X(void, ArGeospatialPose_destroy, (ArGeospatialPose *pose)) \
    X(void, ArGeospatialPose_getLatitudeLongitude, (const ArSession *session, const ArGeospatialPose *geospatial_pose, double *out_latitude_degrees, double *out_longitude_degrees)) \
    X(void, ArGeospatialPose_getHorizontalAccuracy, (const ArSession *session, const ArGeospatialPose *geospatial_pose, double *out_horizontal_accuracy_meters)) \
    X(void, ArGeospatialPose_getAltitude, (const ArSession *session, const ArGeospatialPose *geospatial_pose, double *out_altitude_meters)) \
    X(void, ArGeospatialPose_getVerticalAccuracy, (const ArSession *session, const ArGeospatialPose *geospatial_pose, double *out_vertical_accuracy_meters)) \
    X(void, ArGeospatialPose_getEastUpSouthQuaternion, (const ArSession *session, const ArGeospatialPose *geospatial_pose, float *out_quaternion_4)) \
    X(void, ArGeospatialPose_getOrientationYawAccuracy, (const ArSession *session, const ArGeospatialPose *geospatial_pose, double *out_orientation_yaw_accuracy_degrees)) \
    X(void, ArConfig_setSemanticMode, (const ArSession *session, ArConfig *config, ArSemanticMode semantic_mode)) \
    X(void, ArSession_isSemanticModeSupported, (const ArSession *session, ArSemanticMode semantic_mode, int32_t *out_is_supported)) \
    X(ArStatus, ArFrame_acquireSemanticImage, (const ArSession *session, const ArFrame *frame, ArImage **out_semantic_image)) \
    X(ArStatus, ArFrame_acquireSemanticConfidenceImage, (const ArSession *session, const ArFrame *frame, ArImage **out_semantic_confidence_image)) \
    X(ArStatus, ArSession_checkVpsAvailabilityAsync, (ArSession *session, double latitude_degrees, double longitude_degrees, void *context, ArVpsAvailabilityCallback callback, ArVpsAvailabilityFuture **out_future)) \
    X(void, ArVpsAvailabilityFuture_getResult, (const ArSession *session, const ArVpsAvailabilityFuture *future, ArVpsAvailability *out_result_availability))

#endif
