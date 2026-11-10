# Changelog

All notable changes to this package are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-11-07

### Added

- `use_ar_session(ArOptions, fn(*ArFrame))`: a world-tracking session (ARKit on iOS, ARCore
  on Android) while the scope is mounted, each new frame lent to the handler on the UI
  thread, newest frame wins. `use_ar_status` reports `ArRunning`, `ArUnsupported`,
  `ArDenied`, `ArInstalling`, `ArPaused` and `ArFailed`.
- `ArFrame`: the camera picture as NV12 planes with ui-camera's `CameraFrame` layout (so
  eskiu-gpu's `<gpu/yuv>` draws it), the camera's pose and intrinsics, tracking state and
  reason, planes (alignment, center pose, extent, outline), the app's anchors, feature
  points, the light estimate (intensity, color temperature and tint; ARCore's environmental
  HDR with `light_hdr`) and depth (LiDAR on iOS, the Depth API on Android).
- `ArFrame_view`, `ArFrame_projection`, `ArFrame_display_transform` and `ArFrame_ray` for a
  viewport the picture covers, from `<ar/math>`, computed in Eskiu the same on both
  platforms.
- Hit tests from a screen point (`ArFrame_hit_test`) or a ray (`ar_hit_test_ray`) against
  planes and estimates; anchors from a pose or a hit (`ar_anchor_add`, `ar_anchor_add_hit`,
  `ar_anchor_remove`).
- ARCore runs with no GL context (hardware-buffer mode) and is opened with `dlopen`;
  `tools/check_arcore_api.sh` checks the declarations against ARCore's header and library.
- Image targets: `ArOptions.images` (`ArImageTarget`: a name, gray or RGBA pixels or a PNG
  or JPEG file, and a width in meters) and `tracked_images`; `ArFrame.images`
  (`ArTrackedImage`: target index and name, pose, size, full or last-known tracking);
  `ar_image_state`. ARKit reference images (validated first), ARCore augmented images,
  WebXR image tracking.
- The web: a WebXR `immersive-ar` session (Chrome on Android) started from a tap
  (`ArWaitingForTap`), with the view's pose and intrinsics, planes, anchors, asynchronous hit
  tests, the light probe, depth and image targets where the browser has them, and the page
  over the camera through `dom-overlay`. The browser draws the camera: `ArFrame.has_image`
  is false, or the picture is RGBA (format 4) with raw camera access. Other browsers report
  `ArUnsupported`. Drawing into the session with eskiu-gpu waits for WebXR's WebGPU binding
  (`docs/web.md`).
- One light scale on every platform: `ArLight.intensity` is linear and 1.0 in a neutral
  scene (ARKit's lumens over 1000; ARCore's gamma pixel intensity to linear space over
  middle gray), `color` a linear tint; `raw_intensity`, `source` (`ArLightSource`),
  `ar_light_from_arkit`, `ar_light_from_arcore`, `ar_gamma_to_linear`; `ar_temperature_rgb` gives
  linear RGB.
- Android declares ARCore "optional" in the manifest (`[android.meta_data]` in `ui.toml`,
  which needs the ui tool after eskiu-ui 0.3.0); an AR-only app sets "required" and the
  `android.hardware.camera.ar` feature in its own `ui.toml`.
- `examples/planes`: tap to start on the web, hit tests retried over a few frames.

### Fixed

- `ArFrame_hit_test` before the first camera frame (no intrinsics yet) returns no hit instead
  of handing ARKit a ray of NaNs, which crashed; a non-finite or zero ray is refused on iOS and
  Android too.
- iOS: the session starts once the UI update that mounted it is applied, so a screen with a
  camera it replaces has given the camera back. ARKit started while another session still
  held it showed no picture for about nine seconds.
