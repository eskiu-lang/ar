# Design and spike findings

Before the package was built, a spike answered one question for each platform: can an AR
session run without the app owning an OpenGL context, handing the camera picture to Eskiu
as CPU YUV planes that eskiu-gpu (Metal on iOS, Vulkan on Android, both through wgpu-native)
draws as the background? This page records what was found, how, and what still needs a
device.

Sources read: ARCore SDK 1.57.0 (`com.google.ar:core:1.57.0` from Google's Maven, the
newest on 2026-11-07), its `arcore_c_api.h` from google-ar/arcore-android-sdk, the
`hello_ar_vulkan_c` sample in that repository, Google's "Vulkan rendering" guide for the C
API, and the iOS SDK's ARKit headers. Nothing from Google is kept in this repository: the
app's Gradle build downloads ARCore (`ui.toml`, `[android] dependencies`).

## ARCore (Android)

### Does ARCore need a GL context?

Not when the session is configured with `AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER`.

- The default texture mode, `AR_TEXTURE_UPDATE_MODE_BIND_TO_TEXTURE_EXTERNAL_OES`, writes
  the camera picture into a `GL_TEXTURE_EXTERNAL_OES` texture the app names with
  `ArSession_setCameraTextureName`. In that mode `ArSession_update` fails with
  `AR_ERROR_TEXTURE_NOT_SET` when no name was given and `AR_ERROR_MISSING_GL_CONTEXT` when
  no context is current (both listed as its results in the header).
- `AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER` (header: "texture names provided to
  `ArSession_setCameraTextureName` ... are ignored") hands each frame's picture over as an
  `AHardwareBuffer` (`ArFrame_getHardwareBuffer`) instead. It needs Android 8.1 (API 27);
  `ArSession_configure` returns `AR_ERROR_UNSUPPORTED_CONFIGURATION` on a device that cannot
  do it.
- Google's `hello_ar_vulkan_c` sample runs ARCore this way with no GL at all: its sources
  (`hello_ar_vulkan_application.cc`, `background_renderer.cc`, `VulkanSurfaceView.java`)
  never create or touch an EGL context. Its frame loop is `ArSession_update`,
  `ArFrame_getHardwareBuffer`, then Vulkan.

The header still says `ArSession_update` "may cause off-screen OpenGL activity": that is
ARCore's own context inside the ARCore service, not one the app provides.

### The camera picture as CPU YUV

`ArFrame_acquireCameraImage` gives the frame's CPU image as an `ArImage` in
`AR_IMAGE_FORMAT_YUV_420_888`: three planes with row and pixel strides
(`ArImage_getPlaneData`, `ArImage_getPlaneRowStride`, `ArImage_getPlanePixelStride`), the
same layout as a Camera2 `YUV_420_888` `Image`, which ui-camera 0.2.0 already lends to
Eskiu. Its size is chosen by the session's camera config:
`ArSession_getSupportedCameraConfigsWithFilter` lists, for the back camera, three configs
whose CPU image is VGA, a middle size (typically 1280x720) and "a large resolution matching
the GPU texture resolution" (typically 1920x1080). The package picks the largest one that
fits `ArOptions.image_width` x `image_height` (default 1280x720), so the background is
sharp enough without copying 1080p every frame.

The intrinsics of that same image come from `ArCamera_getImageIntrinsics` (focal length,
principal point, size in pixels), and `ArCamera_getPose` is "the pose of the physical camera
... with right and up being relative to the image readout", the same convention as ARKit's
`ARCamera.transform`. So the projection, view and display transform for a viewport are
computed in Eskiu from the image's own intrinsics, the same code on both platforms
([math](math.md)), and the background drawn with eskiu-gpu's cover fit lines up with them.

### Minimal setup (no GL)

```
ArCoreApk_requestInstall(env, activity, 1, &status)      until INSTALLED (UI thread)
ArSession_create(env, activity, &session)                after the CAMERA permission
ArSession_getSupportedCameraConfigsWithFilter + ArSession_setCameraConfig   CPU image size
ArConfig_setTextureUpdateMode(EXPOSE_HARDWARE_BUFFER)
ArConfig_setUpdateMode(LATEST_CAMERA_IMAGE)              update never blocks
ArConfig_setPlaneFindingMode / setLightEstimationMode / setDepthMode
ArSession_configure, ArSession_resume
loop on a thread of ours:
  ArSession_update(session, frame)                       no texture name, no context
  ArFrame_acquireCameraImage -> ArImage YUV_420_888      copied out, released at once
  ArFrame_acquireCamera -> pose, tracking, image intrinsics
  planes, point cloud, light estimate, depth (ArFrame_acquireDepthImage16Bits)
ArFrame_hitTestRay(origin, direction)                    hit tests from a ray (no view needed)
```

`ArSession_setDisplayGeometry` is still called with the display's rotation and size, but
nothing the package uses depends on it: hit tests go through `ArFrame_hitTestRay` with a ray
computed in Eskiu, not through `ArFrame_hitTest`'s view coordinates.

### How the package links ARCore

`libarcore_sdk_c.so` ships in the AAR's `jni/` folders and is a thin loader: the tracking
itself is in Google Play Services for AR, installed on the device. The package declares the
Maven dependency in its `ui.toml`, so the ui tool builds the app through Gradle, which puts
the AAR's classes and `.so` files into the APK. The package's C code does not link against
the `.so` at build time: it opens it with `dlopen` at run time and looks up each function
with `dlsym`, so the app's library builds with only the NDK, and a device without it gets
"unsupported" instead of a crash at load.

### What was checked without a device

- Every ARCore function the package calls is exported by `libarcore_sdk_c.so` 1.57.0
  (arm64-v8a), checked with `llvm-nm -D` against the list in `native/android/ar.c`.
- The package's own function-pointer types were compiled against Google's
  `arcore_c_api.h` (assigning each real function to the package's pointer type, warnings
  as errors), so the prototypes match.
- The Android build of a throwaway app (`ui build android`, Gradle, the ARCore dependency)
  compiles and packages; the iOS simulator build compiles and links the ARKit code.

### What needs a device

- That `ArFrame_acquireCameraImage` returns images in hardware-buffer mode on real devices
  (the header does not tie it to a texture mode; the Vulkan sample does not use it).
- Whether ARCore needs the `com.google.ar.core` meta-data (`required` or `optional`) in the
  app's manifest. Google's docs say apps declare it but not what happens without it, and
  the ui tool has no `ui.toml` key for `<meta-data>` yet: if ARCore refuses to start without
  it, the ui tool needs that key (a small change to `tools/ui/android.esk`).
- That an `ArTrackable` pointer stays the same for the same plane between frames (the
  package keys plane ids on it, as Google's `hello_ar_c` does with its plane color map).
- Light: the scale that makes ARCore's pixel intensity comparable with ARKit's ambient
  intensity (the package divides by 0.466, ARCore's mid-gray, for `intensity` 1.0 at a
  neutral scene).
- Copy cost of the CPU image at 1280x720 (about 1.4 MB a frame) on a slow phone.

## ARKit (iOS)

ARKit needs no GL and no view: an `ARSession` with an `ARWorldTrackingConfiguration` and a
delegate gets an `ARFrame` per camera frame.

| What | Where |
|---|---|
| Camera picture | `ARFrame.capturedImage`: a `CVPixelBuffer`, `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` (NV12, full range); the matrix is read from its YCbCr attachment, BT.601 |
| Camera pose | `ARCamera.transform`: camera to world, axes relative to the image (landscape, home button right) |
| Intrinsics | `ARCamera.intrinsics` (fx, fy, cx, cy) for `ARCamera.imageResolution` |
| Projection and display transform | computed in Eskiu from the intrinsics, matching `projectionMatrixForOrientation:viewportSize:zNear:zFar:` and `displayTransformForOrientation:viewportSize:` (aspect fill) |
| Planes | `ARPlaneAnchor`: `alignment`, `center`, `extent`, `geometry.boundaryVertices` |
| Anchors | `ARAnchor initWithTransform:`, `addAnchor:`, `removeAnchor:` |
| Hit tests | `ARRaycastQuery initWithOrigin:direction:allowingTarget:alignment:` and `[session raycast:]`, with the ray computed in Eskiu |
| Points | `ARFrame.rawFeaturePoints` |
| Light | `ARLightEstimate.ambientIntensity` (1000 is neutral) and `ambientColorTemperature` |
| Depth | `ARFrame.sceneDepth` (LiDAR devices, `ARFrameSemanticSceneDepth`): Float32 meters |

### The lending pattern

ui-camera 0.2.0 lends a frame to Eskiu: the native side keeps the newest one, posts one
event when it is waiting, and Eskiu borrows it on the UI thread through an acquire and
release pair. It fits `ARFrame`: the session's delegate (on the main queue) keeps the newest
`ARFrame`, the acquire retains it and locks `capturedImage`'s base address read-only, and the
release unlocks and lets it go. At most two frames are held (the newest and the lent one),
well under the count ARKit warns about when an app retains its frames ("the delegate is
retaining N ARFrames"), so the camera never stalls.

On Android the same pair lends a snapshot: the session's thread copies the CPU image out of
the `ArImage` (released at once, so ARCore's limit on held images never matters) and reads
the pose, planes, points, light and depth into it. Three snapshots rotate: one being
filled, the newest waiting, and the lent one.

### What was checked without a device

The ARKit code compiles and links in the simulator build. The simulator has no AR
(`ARWorldTrackingConfiguration.isSupported` is false there), so the package reports
`ArUnsupported` on it.

### What needs a device

Everything at run time: tracking, the orientation of the picture and of the pose against the
math in [math.md](math.md) on a held phone in each orientation, plane polygons, raycasts,
light, LiDAR depth.

## Web

WebXR's `immersive-ar` is out of scope for 0.1.0: the web build's native code reports "AR
is not supported in the browser" (`ArUnsupported`). ViroReact's web AR uses tinyvio, a
visual-inertial odometry engine already ported to Eskiu (eskiu-vision, private for now),
which could be a later fallback for the browser and for devices without ARCore.

## The design that follows

- **One frame path.** `use_ar_session(ArOptions, fn(*ArFrame))` hands each new frame to the
  handler on the UI thread, borrowed for the call, newest frame wins, as `use_camera_frames`
  does. `ArFrame.image` has `CameraFrame`'s field layout, so eskiu-gpu's `<gpu/yuv>` uploads
  it by pointer (`(*GpuYuvImage)&frame.image`) and draws it with the cover fit.
- **Math in Eskiu.** Pose, view, projection, display transform and the hit-test ray are
  computed in `<ar/math>` from the image's intrinsics, the pose and the rotation, with tests
  on the computer. The native side gives raw data.
- **No view.** The session needs no native view: the app draws the picture and its 3D
  content with eskiu-gpu in whatever view it uses, and passes that view's size to
  `projection`, `display_transform` and `hit_test`.
