# ar documentation

Guides and reference for ar. The [README](../README.md) has the overview.

<!-- pages -->
- [The session](session.md)
- [Drawing](drawing.md)
- [The geometry](math.md)
- [Design](design.md)
- [API reference](api.md): every public function and type
<!-- /pages -->

## Platforms

| Platform | Session | Camera picture | Needs |
|---|---|---|---|
| iOS | `ARSession` with `ARWorldTrackingConfiguration`, no view | `ARFrame.capturedImage`, NV12, lent by pointer | iOS 15, an A9 chip or newer; LiDAR for depth |
| Android | ARCore's C API in hardware-buffer mode, no GL, opened with `dlopen` | The CPU image (`YUV_420_888`), copied as NV12 | Android 8.1 (API 27), an ARCore device, Google Play Services for AR |
| Web | none | none | `ArUnsupported` |

The package's `ui.toml` brings ARKit and a camera usage text on iOS, and the camera
permission and `com.google.ar:core` (through Gradle) on Android. Put your own usage text in
the app's `ui.toml`, which wins over the package's:

```toml
[ios.plist]
NSCameraUsageDescription = "Places furniture in your room"
```

The iOS simulator and the Android emulator without ARCore report `ArUnsupported`.

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
  color temperature tint ([math](math.md#tests)).
- `tests/session.esk`: `use_ar_session` and `use_ar_status` against stand-ins for the native
  side that lend a frame with planes, an anchor, points, light and depth; the checks on what
  the platform lends (short planes, strides, odd rotations, NaN poses, depth rows); hit tests
  and anchors; new options; unmounting; and that `ArCameraImage` lines up with eskiu-gpu's
  `GpuYuvImage`.

`tools/check_arcore_api.sh` checks `native/android/ar_api.h` against ARCore's header and
library (downloaded to a temporary directory; needs the Android NDK).
