# Design and spike findings

Before the package was built, a spike answered one question for each platform: can an AR
session run without the app owning an OpenGL context, handing the camera picture to Eskiu
as CPU YUV planes that eskiu-gpu (Metal on iOS, Vulkan on Android, both through wgpu-native)
draws as the background? This page records what was found, how, and what still needs a
device; then what the manifest, the light scale, image targets and WebXR added.

Sources read: ARCore SDK 1.57.0 (`com.google.ar:core:1.57.0` from Google's Maven, the
newest on 2026-11-07), its `arcore_c_api.h` from google-ar/arcore-android-sdk, the
`hello_ar_vulkan_c` sample in that repository, Google's "Vulkan rendering" guide for the C
API, its "Enable ARCore" guide and `ArLightEstimate` reference, the iOS SDK's ARKit headers
and Apple's `ARLightEstimate` documentation, the WebXR specifications and explainers (hit
test, anchors, plane detection, lighting estimation, depth sensing, raw camera access, image
tracking, the WebGPU binding) and Chrome's notes on WebGPU in WebXR. Nothing from Google is kept in this repository: the
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

### The manifest: ARCore optional or required

Google's "Enable ARCore" guide has every ARCore app declare
`<meta-data android:name="com.google.ar.core" android:value="optional"/>` (or `"required"`)
in its `<application>`; an AR Required app also declares
`<uses-feature android:name="android.hardware.camera.ar"/>`, which makes Google Play list it
only on ARCore devices and install Google Play Services for AR with it. An AR Optional app
is listed everywhere and installs ARCore itself (`ArCoreApk_requestInstall`, which the
package calls). The ui tool had no `ui.toml` key for either, so eskiu-ui gained two:
`[android.meta_data]` (one `<meta-data>` per key, merged over the app and its packages, the
app's value winning, escaped) and `[android] features` (`<uses-feature>`). The package's
`ui.toml` declares ARCore optional, so an app that also runs without AR still installs on
devices without ARCore; an AR-only app sets "required" and the feature in its own
`ui.toml` ([platforms](README.md#android-arcore-optional-or-required)). The throwaway app's
APK built through Gradle has the merged entry next to ARCore's own
`com.google.ar.core.min_apk_version`, and "required" from the app's `ui.toml` replaces the
package's "optional".

### What was checked without a device

- `tools/check_arcore_api.sh` (also in CI): the 109 ARCore functions the package calls,
  declared in `native/android/ar_api.h`, compile against Google's `arcore_c_api.h` 1.57.0
  (each real function assigned to the package's pointer type with warnings as errors, each
  enum value compared), and every one is exported by `libarcore_sdk_c.so` 1.57.0
  (arm64-v8a, `llvm-nm -D`). A wrong parameter type makes the check fail.
- `native/android/ar.c` compiles with the NDK's clang (`-Wall -Wextra -Werror`, API 27).
- A throwaway app on the package built with `ui build android` goes through Gradle with the
  ARCore dependency: the APK holds `libarcore_sdk_c.so` for each ABI, the app's library
  exports `ar_native_*` and the `EskiuAr` natives, `org.eskiu.ar.EskiuAr` is in the dex and
  listed for setup (with `decodeGray` for image files), and the merged manifest has the
  camera permission, `com.google.ar.core` "optional" and ARCore's own entries
  (`com.google.ar.core.min_apk_version`, `InstallActivity`).

### What needs a device

- That `ArFrame_acquireCameraImage` returns images in hardware-buffer mode on real devices
  (the header does not tie it to a texture mode; the Vulkan sample does not use it).
- That an `ArTrackable` pointer stays the same for the same plane between frames (the
  package keys plane ids on it, as Google's `hello_ar_c` does with its plane color map).
- Light: that ARCore's pixel intensity on the package's scale (below) reads near 1.0 where
  ARKit reads near 1000 lumens in the same room.
- Image targets: detection, tracking and the pose's axes on a printed picture; which
  pictures ARCore refuses.
- That `ArSession_update` in `AR_UPDATE_MODE_LATEST_CAMERA_IMAGE` runs well from the
  package's own thread (attached to the JVM, no GL), polled every 4 ms when no new frame is
  in.
- Copy cost of the CPU image at 1280x720 (about 1.4 MB a frame) on a slow phone.

## ARKit (iOS)

ARKit needs no GL and no view: an `ARSession` with an `ARWorldTrackingConfiguration` and a
delegate gets an `ARFrame` per camera frame.

| What | Where |
|---|---|
| Camera picture | `ARFrame.capturedImage`: a `CVPixelBuffer`, `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` (NV12, full range); the matrix is read from its YCbCr attachment, BT.601 |
| Camera pose | `ARCamera.transform`: camera to world, axes relative to the image (landscape, home button right) |
| Intrinsics | `ARCamera.intrinsics` (fx, fy, cx, cy) for `ARCamera.imageResolution` |
| Projection and display transform | computed in Eskiu from the intrinsics with the aspect-fill fit that `projectionMatrixForOrientation:viewportSize:zNear:zFar:` and `displayTransformForOrientation:viewportSize:` use (comparing the two on a device is left to do) |
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

`native/ios/ar.m` compiles warning-free (`-Wall -Wextra -Werror`) for the simulator and for
devices (iOS 15 SDK target), and the throwaway app's `ui build ios` simulator build links
ARKit (`ARSession`, `ARWorldTrackingConfiguration`, `ARRaycastQuery`, `ARPlaneAnchor`,
`ARAnchor`, `ARReferenceImage`, `ARImageAnchor`) with the camera usage text in its
Info.plist. The simulator has no AR
(`ARWorldTrackingConfiguration.isSupported` is false there), so the package reports
`ArUnsupported` on it.

### What needs a device

Everything at run time: tracking, the orientation of the picture and of the pose against the
math in [math.md](math.md) on a held phone in each orientation, plane polygons, raycasts,
light, LiDAR depth, image validation and detection.

## Light: one scale

ARKit's `ARLightEstimate.ambientIntensity` is in lumens with 1000 for neutral lighting, and
is meant for physically based (linear) lighting. ARCore's
`ArLightEstimate_getPixelIntensity` is the camera view's mean pixel intensity in gamma
space, 0 to 1; its reference says to divide it by 0.466 (middle gray in gamma space) when
rendering in gamma space, and to raise it to the power 2.2 and divide by 0.18 (middle gray in
linear space) when rendering in linear space. Its color correction is in gamma space too,
green 1. The first version divided the gamma value by 0.466: 1.0 at middle gray, but not linear
(twice the light read as 1.37 times the value).

The package now has one documented scale: a linear multiplier, 1.0 in a neutral scene.
iOS: lumens / 1000. Android: `pow(p, 2.2) / 0.18`, and the color correction to the power
2.2; the color from ARKit's temperature is taken to linear space the same way. The two
middle grays agree (0.466^2.2 = 0.186), so ARCore's middle gray reads 1.036. The native
side sends the platforms' own values with their source, Eskiu converts them
(`ar_light_from_arkit`, `ar_light_from_arcore`, tested), and `raw_intensity` keeps the
platform's value. Environmental HDR (and WebXR's light probe) keep their own units in the
main light and spherical harmonics, with `intensity` 1.0.

## Image targets

- **ARKit**: `ARReferenceImage initWithCGImage:orientation:physicalWidth:` needs a physical
  width, and a session run with an image ARKit cannot track fails with
  `ARErrorCodeInvalidReferenceImage`. So each image is checked with
  `validateWithCompletionHandler:` (iOS 13) and the session is configured again with the
  valid ones once the set is checked (keeping its world). `detectionImages` with
  `maximumNumberOfTrackedImages` gives `ARImageAnchor`s with `isTracked` (followed, or the
  last pose), the reference image (named by the target's index) and `estimatedScaleFactor`.
- **ARCore**: `ArAugmentedImageDatabase_addImageWithPhysicalSize` takes 8-bit gray pixels
  whose stride equals the width (the header: other strides are `AR_ERROR_INVALID_ARGUMENT`),
  20 to 30 ms each, and refuses weak images with `AR_ERROR_IMAGE_INSUFFICIENT_QUALITY`.
  `ArConfig_setAugmentedImageDatabase` copies the database. `ArAugmentedImage_getCenterPose`
  has y out of the image, x to its right and z toward its bottom, which the package takes
  for every platform; `ArAugmentedImage_getTrackingMethod` says full tracking or last known
  pose. The database index is mapped back to the target, since refused images take no
  index.
- **The pixels**: Eskiu turns RGBA targets gray (BT.601 weights) into tight rows for both
  platforms; files are decoded natively (`UIImage`, `BitmapFactory`).
- `tools/check_arcore_api.sh` covers the new declarations (9 functions, the trackable type
  and the tracking methods) against ARCore 1.57.0's header and library.

## Web: WebXR immersive-ar

Chrome on Android runs WebXR's `immersive-ar` on ARCore, so the session data maps onto the
package's frame: the view's pose and an intrinsics matrix derived from its projection
(rotation 0: the view is upright), `plane-detection`, `anchors`, `hit-test`,
`light-estimation`, `depth-sensing` and `image-tracking` where granted. Findings:

- An immersive session needs a user activation, so the web reports `ArWaitingForTap` and
  starts from the next tap on the page.
- WebXR delivers frames only to a session with a layer: the package keeps a WebGL layer it
  clears each frame.
- Hit tests are asynchronous (hit-test sources answer on later frames), so the package keeps
  one source per viewer-space ray and answers from its latest results.
- The browser composites the camera; the page gets the picture only through raw camera
  access (`camera-access`), as a WebGL texture, which the package reads back as RGBA (format
  4, which eskiu-gpu's `<gpu/yuv>` and ui-camera's web frames already use). Without it
  `ArFrame.has_image` is false.
- Drawing: eskiu-gpu is WebGPU, and WebXR's WebGPU binding (`XRGPUBinding`) is behind flags
  in Chrome ("WebXR Projection Layers" and "WebXR/WebGPU Bindings", for developer testing
  since Chrome 135) with an Editor's Draft specification. An app cannot draw into the
  session with eskiu-gpu in shipping browsers; the page (and a transparent canvas in it)
  shows over the camera through `dom-overlay`. Details in [web](web.md).

ViroReact's web AR uses tinyvio, a visual-inertial odometry engine already ported to Eskiu
(eskiu-vision, private for now), which could still be a fallback for browsers without WebXR
AR and devices without ARCore.

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
