# Changelog

All notable changes to this package are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.2] - 2026-11-18

### Fixed

- Android: the camera config is the smallest that holds the wished size, else the largest up to
  1920x1080, and the rear camera's at 30 fps. Above 1080p the CPU image costs more than some
  mid-range phones sustain, and their tracking paused many times a second (ViroReact 3.2.1).

### Changed

- Depends on ui 0.6.2.

## [0.2.1]

### Changed

- The package moved to the eskiu-packages organization; the old URL redirects. Depends on ui 0.6.1.

## [0.2.0]

### Added

- Front camera with face tracking (ViroReact's `frontCameraEnabled`): `ArOptions.front_camera`,
  `frame.faces` (`ArFace`: pose, mesh, 52 blend shapes and eyes on iOS, region poses on
  Android). Android uses ARCore's Augmented Faces; iOS needs the eskiu-packages/face package
  (`face_install`), which holds the TrueDepth calls (App Store guideline 2.5.1).
- Body tracking on iOS: `ArOptions.body`, `frame.bodies` (`ArBody`, ARKit's 91 joints),
  `ar_body_joint`.
- Object targets on iOS (ViroReact's ViroARObjectMarker): `ArOptions.objects`
  (`ArObjectTarget`, ARKit `.arobject` files) and `frame.objects` (`ArTrackedObject`).
- ARCore on iOS: with Google's ARCore SDK for iOS in the app ([ios.spm]) and `GARAPIKey`,
  cloud anchors, Earth, terrain and rooftop anchors, VPS availability and scene semantics
  work on iOS as on Android (a GARSession fed each ARKit frame).
- ARCore's scene semantics on Android: `ArOptions.semantics`, `frame.semantics`
  (`ArSemantics`: a label and a confidence per pixel), `ArSemantics_label`,
  `ArSemantics_fraction`.
- ARCore's cloud anchors and Earth on Android: `ArOptions.cloud_anchors` and `geospatial`,
  `ar_host_cloud_anchor`, `ar_resolve_cloud_anchor`, `ar_earth_anchor`, `ar_terrain_anchor`,
  `ar_rooftop_anchor`, `ar_vps_check` (VPS availability), jobs followed with `ar_job`, and `frame.earth` (the camera's
  geospatial pose). iOS answers that it has none.
- Depth without LiDAR on iOS: `ArOptions.depth_model` runs a CoreML depth model (Depth
  Anything V2 metric) on the picture; `ArDepth.estimated` tells its depth from a sensor's,
  `ArOptions.depth_source` picks the sensor, the model, or the sensor where there is one.
- `ArOptions.mesh`, `ArFrame.meshes` (`ArMesh`): the world's mesh from LiDAR scene
  reconstruction on iOS (ViroReact's world mesh), read in place from ARKit's buffers.
- `ArFrame_depth_hit` and `AR_HIT_DEPTH`: hit tests against the depth image, with the
  surface's normal (ViroReact's depth hit tests).
- `ArOptions.people`: the depth of the people in the picture in `frame.depth` (ARKit's
  person segmentation with depth, merged with LiDAR's scene depth where there is one), for
  occlusion by people.
- `ArPlane.classification` (`ArPlaneClass`): floor, table, wall, ceiling, seat, window or
  door, from ARKit's plane classification and WebXR's semantic labels (ARCore does not
  classify).
- What the device supports, before or without a session: `ar_supported` (ARKit's world
  tracking; ARCore supported, installed or installable), `ar_depth_supported` and
  `ar_semantics_supported`.
- `ArOptions.autofocus` (false keeps the focus fixed) and `ArOptions.world_alignment`
  (`ArAlignGravity`, `ArAlignGravityAndHeading`, `ArAlignCamera`), ViroReact's autofocus and
  worldAlignment.
- `ArOptions.depth_model_scale` calibrates the depth model's distances and
  `depth_model_fps` caps how often it runs (the device's thermal state lowers it).
- A host without eskiu-packages/ui runs a session with `ar_session_start` and `ar_poll_frame`,
  and resolves asset paths with `ar_set_asset_resolver`.

### Changed

- The ui hooks `use_ar_session` and `use_ar_status` moved to `<ar/ui>`: an app that uses
  them imports `<ar/ui>`. `<ar>` itself no longer needs eskiu-packages/ui's hooks.
- Depends on ui 0.6.0.
- The sources are split by topic, with the module's parts in `src/_ar/` and the native
  hosts in included parts. No API change beyond `<ar/ui>`.

### Fixed

- iOS: Earth anchors start once the location permission is granted, not before.
- Android: a session says why when geospatial mode or the configuration fails.

## [0.1.0]

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
