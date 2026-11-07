# The session

`use_ar_session` runs a world-tracking session (ARKit on iOS, ARCore on Android) while its
scope is mounted and hands each new frame to its handler on the UI thread. `use_ar_status`
says how the session is doing.

```eskiu
import <ar>;

void World() {
    use_ar_status(void(int status) {
        if (status == ArUnsupported) { /* no AR here: show something else */ }
    });
    use_ar_session(ArOptions{horizontal: true, vertical: true}, void(*ArFrame frame) {
        // Borrowed for this call: read it, draw it, copy what you keep, then return.
        if (frame.tracking != ArTrackingNormal) { return; }
        for (i in 0..frame.plane_count) {
            *ArPlane plane = &frame.planes[i];
            // plane.pose, plane.extent_x, plane.extent_z, plane.polygon[0..polygon_count]
        }
    });
}
```

## Options

| Field | Default | What it does |
|---|---|---|
| `horizontal`, `vertical` | `true`, `false` | Plane detection: floors, tables and ceilings; walls. |
| `light` | `true` | The light estimate in `frame.light`. |
| `light_hdr` | `false` | ARCore's environmental HDR estimate instead (main light and spherical harmonics). iOS treats it as `light`. |
| `depth` | `false` | A depth image in `frame.depth` where the device has one: LiDAR on iOS (`sceneDepth`), the Depth API on Android. |
| `points` | `false` | The feature points in `frame.points`. |
| `image_width`, `image_height` | 1280, 720 | The camera picture's size, a wish as the sensor sees it (landscape). iOS picks the smallest video format that holds it, Android the smallest CPU image config; both fall back to the largest. |

New options start the session again with them: on iOS the world is kept (same session, new
configuration), on Android the session is configured again, and a new picture size makes a
new session.

## Status

`use_ar_status` gets each `ArStatus` while its scope is mounted. `ar_last_status` keeps the
last one.

| Status | When |
|---|---|
| `ArStarting` | Before the platform answers. |
| `ArRunning` | The session runs; frames come. |
| `ArUnsupported` | No AR: the device cannot (no ARCore support, Android before 8.1, an old iPhone), the iOS simulator, the browser. |
| `ArDenied` | The camera permission was refused. |
| `ArInstalling` | Android is installing or updating Google Play Services for AR; the session starts when the app comes back. |
| `ArPaused` | The app is in the background (or another app took the camera). |
| `ArFailed` | Anything else. |

## A frame

`ArFrame` is borrowed for the handler's call, and so is everything it points to.

| Field | What it is |
|---|---|
| `image` | The camera picture: `ArCameraImage`, with the field layout of ui-camera's `CameraFrame` (NV12 on both platforms; see [drawing](drawing.md)). `image.rotation` turns it upright for the screen. |
| `camera` | The camera's pose, camera to world, x right and y up along the picture as the sensor gives it. `ArFrame_view` turns it for the screen. |
| `intrinsics` | The picture's focal lengths and principal point in pixels, and its size. |
| `tracking`, `reason` | `ArTrackingNormal`, `ArTrackingLimited` (with an `ArReason`) or `ArTrackingNone` (the pose means nothing). |
| `planes`, `plane_count` | `ArPlane`s: `id`, `alignment` (`ArHorizontalUp`, `ArHorizontalDown`, `ArVertical`), `tracking`, `pose` (the center, y along the normal), `extent_x`, `extent_z`, the outline `polygon` (x, z about the center), `subsumed_by` (Android: the plane that took this one over). |
| `anchors`, `anchor_count` | The app's anchors with their current poses. |
| `points`, `point_count` | Feature points (`position`, `confidence`; 1 on iOS). |
| `light` | `ArLight`: `valid`, `intensity` (1.0 is a neutral scene), `temperature` (kelvin, iOS), `color` (a tint), and with `light_hdr` on Android the main light and 9 RGB spherical harmonics. |
| `depth` | `ArDepth`: size, format (`ArDepthMeters32` on iOS, `ArDepthMillimeters16` on Android), rows, `data` (null when none). `ArDepth_meters(x, y)` reads one pixel. It lines up with the picture as the sensor gives it. |

### Geometry for your viewport

The app draws the picture and its content in a view of its own size. Give that size (in any
unit, the same as the point you test):

```eskiu
ArMat4 view = frame.view();                                  // world to camera, for the screen
ArMat4 projection = frame.projection(w, h, 0.05, 100.0);     // depth 0..1 (WebGPU, Metal)
ArAffine uv = frame.display_transform(w, h);                 // viewport 0..1 -> picture 0..1
ArRay ray = frame.ray(x, y, w, h);                           // the ray under (x, y)
```

They assume the picture covers the viewport (aspect fill, cropped about the center), as
eskiu-gpu's `GPU_YUV_COVER` draws it. [math.md](math.md) has the formulas.

## Hit tests and anchors

```eskiu
ArHit[4] hits;
int n = frame.hit_test(x, y, w, h, AR_HIT_PLANES | AR_HIT_ESTIMATED, &hits[0], 4);
if (n > 0) {
    int64 id = ar_anchor_add_hit(&hits[0]);   // frame.anchors has it from the next frame
}
```

`ArFrame_hit_test` casts the ray under the point against the planes found so far (inside their
outline: `AR_HIT_PLANES`) and estimates (`AR_HIT_ESTIMATED`: ARKit's estimated planes,
ARCore's feature and depth points), nearest first. `ar_hit_test_ray` takes a ray of your own.
Each `ArHit` has the distance, the kind, the plane's id and the pose (y along the surface's
normal).

`ar_anchor_add(pose)` adds an anchor anywhere, `ar_anchor_add_hit(&hit)` at a hit (on
Android an anchor on a plane is attached to it and moves with it), `ar_anchor_remove(id)`
removes one. Anchors live as long as the session.

On Android a hit test runs on the session's current frame, which may be a frame or two newer
than the one lent; on iOS it runs on the session too. The difference is a few milliseconds of
movement.

## Threads and timing

The handler runs on the UI thread, so it can set signals. A frame that arrives while the
handler runs, or while its event waits, replaces the one waiting: a slow handler sees fewer,
newer frames, never a queue.

- **iOS.** The session's delegate runs on the main queue and keeps the newest `ARFrame`. The
  acquire retains it and locks `capturedImage` read-only; the release lets it go. At most two
  frames are held, so ARKit never runs out of buffers.
- **Android.** A thread of the package calls `ArSession_update` (non-blocking mode) and, for
  each new frame, copies the CPU image (as NV12) and reads the camera, planes, anchors,
  points, light and depth into one of three snapshots: one being filled, the newest waiting,
  the one lent. ARCore's images are released at once.

## Platform notes

- **iOS.** iOS 15 or later; world tracking needs an A9 chip or newer. Depth needs LiDAR. The
  simulator reports `ArUnsupported`. The package's `ui.toml` adds ARKit and a camera usage
  text; put your own in the app's `ui.toml`.
- **Android.** Android 8.1 (API 27) or later on a device that supports ARCore, with Google
  Play Services for AR, which the session offers to install. The package's `ui.toml` adds
  the camera permission and `com.google.ar:core`, so the app builds through Gradle. The app's
  library opens ARCore with `dlopen` and needs no GL ([design](design.md)).
- **Web.** No AR in this version: `ArUnsupported`.
