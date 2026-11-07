# ar documentation

Guides and reference for ar. The [README](../README.md) has the overview.

<!-- pages -->
- [The session](session.md)
- [Drawing](drawing.md)
- [Image targets](images.md)
- [AR on the web](web.md)
- [The geometry](math.md)
- [Design](design.md)
- [API reference](api.md): every public function and type
<!-- /pages -->

## Platforms

| Platform | Session | Camera picture | Needs |
|---|---|---|---|
| iOS | `ARSession` with `ARWorldTrackingConfiguration`, no view | `ARFrame.capturedImage`, NV12, lent by pointer | iOS 15, an A9 chip or newer; LiDAR for depth |
| Android | ARCore's C API in hardware-buffer mode, no GL, opened with `dlopen` | The CPU image (`YUV_420_888`), copied as NV12 | Android 8.1 (API 27), an ARCore device, Google Play Services for AR |
| Web | WebXR `immersive-ar`, started from a tap | Drawn by the browser; RGBA with raw camera access ([web](web.md)) | Chrome on an ARCore phone; elsewhere `ArUnsupported` |

The package's `ui.toml` brings ARKit and a camera usage text on iOS, and the camera
permission and `com.google.ar:core` (through Gradle) on Android. Put your own usage text in
the app's `ui.toml`, which wins over the package's:

```toml
[ios.plist]
NSCameraUsageDescription = "Places furniture in your room"
```

The iOS simulator and the Android emulator without ARCore report `ArUnsupported`.

## Android: ARCore optional or required

ARCore apps say in their manifest whether they need ARCore. The package's `ui.toml` declares
it optional:

```toml
[android.meta_data]
"com.google.ar.core" = "optional"
```

so an app that also works without AR installs on every device, and its session reports
`ArUnsupported` where ARCore cannot run (the session offers to install Google Play Services
for AR where it can). An app that is only AR says "required" in its own `ui.toml` (the
app's value wins) and adds the AR camera feature, so Google Play lists it only on ARCore
devices and installs Google Play Services for AR with it:

```toml
[android]
features = ["android.hardware.camera.ar"]

[android.meta_data]
"com.google.ar.core" = "required"
```

Both keys need the ui tool after eskiu-ui 0.3.0 (`[android.meta_data]` and `[android]
features`); an older ui tool ignores them.

## The example

`examples/planes` shows the session's status and tracking, the planes it finds, the light
estimate, and anchors placed where the middle of the picture meets a plane:

```bash
cd examples/planes
ui run ios        # or android, on a device
```

## Tests

`eskiuc test` runs on this computer:

- `tests/math.esk`: the projection, view, display transform and rays agree with each other
  for 400 random cameras in every rotation and several viewports; near and far depths; the
  color temperature tint; the light scale from ARKit's lumens and ARCore's gamma pixel
  intensity ([math](math.md#tests)).
- `tests/session.esk`: `use_ar_session` and `use_ar_status` against stand-ins for the native
  side that lend a frame with planes, an anchor, points, light and depth; the checks on what
  the platform lends (short planes, strides, odd rotations, NaN poses, depth rows); hit tests
  and anchors; image targets (sent before the session, RGBA turned gray, refused ones,
  found images); frames without a picture and the web's RGBA picture; the light on one scale;
  new options; unmounting; and that `ArCameraImage` lines up with eskiu-gpu's `GpuYuvImage`.

`tools/check_arcore_api.sh` checks `native/android/ar_api.h` against ARCore's header and
library (downloaded to a temporary directory; needs the Android NDK), the augmented image
functions included, and that the header's own types declare every function without
Google's header.
