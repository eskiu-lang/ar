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
- The web reports `ArUnsupported`.
- `examples/planes`.
