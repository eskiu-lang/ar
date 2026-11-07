# AR on the web

On the web the session is a WebXR `immersive-ar` session: Chrome on Android with Google Play
Services for AR (Chrome builds WebXR AR on ARCore). Other browsers (desktop Chrome, Firefox,
Safari on iOS, which has no WebXR AR) report `ArUnsupported`, as before.

The session data works as on the phones: the camera's pose and projection, planes, anchors,
hit tests, the light estimate, depth and image targets, each where the browser has the
feature. What differs is the picture and drawing, below.

## Starting: a tap

A browser starts an immersive session only from a user's tap (a "user activation"), and a
session is started when the screen mounts, not from a tap. So on the web the status goes to
`ArWaitingForTap` and the next tap or click anywhere on the page starts the session:

```eskiu
use_ar_status(void(int status) {
    if (status == ArWaitingForTap) { /* show "Tap to start AR" */ }
});
```

The same happens when the user leaves AR (the back button) while the screen is still
mounted, and when new options or new image targets need a new session: WebXR fixes a
session's features when it starts.

## What the session asks for

```js
navigator.xr.requestSession('immersive-ar', {
  requiredFeatures: ['local'],
  optionalFeatures: ['local-floor', 'hit-test', 'anchors', 'dom-overlay', 'camera-access',
                     'plane-detection',      // with horizontal or vertical
                     'light-estimation',     // with light or light_hdr
                     'depth-sensing',        // with depth: cpu-optimized
                     'image-tracking'],      // with image targets
  domOverlay: {root: document.querySelector('#app')},
});
```

The world is the `local` reference space: its origin is where the phone was when the
session started, y up, as ARKit's and ARCore's. A feature the browser does not grant is left
out without failing the session.

| `ArFrame` field | On the web |
|---|---|
| `camera`, `intrinsics` | The view's pose (`XRView.transform`) and intrinsics derived from its projection matrix for the view's size, so `frame.projection`, `frame.ray` and `frame.hit_test` work unchanged. `image.rotation` is 0: the view is already upright. |
| `tracking` | Normal; limited (`ArReasonRelocalizing`) when the browser emulates the position (`emulatedPosition`). |
| `image`, `has_image` | See below. |
| `planes` | `XRFrame.detectedPlanes` ('plane-detection'), the pose moved to the outline's middle; no `subsumed_by`. |
| `anchors` | `XRFrame.createAnchor` ('anchors'), made on the frame after `ar_anchor_add`. Without the feature an anchor keeps the pose it was given. |
| Hit tests | Asynchronous in WebXR (`XRHitTestSource`): see below. Results have no plane id (0). |
| `points` | None: WebXR has no feature points. |
| `light` | The light probe ('light-estimation'): `source` `ArLightWebXR`, `intensity` 1.0, the main light's direction and RGB intensity, 9 RGB spherical harmonics, all in WebXR's linear units. |
| `depth` | 'depth-sensing' with CPU access, resampled to the view in meters (`ArDepthMeters32`) with `getDepthInMeters`. |
| `images` | 'image-tracking', behind Chrome's WebXR Incubations flag ([images](images.md)). |

### Hit tests

WebXR answers a hit test from a hit-test source, on frames after the one that asked. The
package keeps one source per ray in the viewer's space (the same screen point gives the
same ray, whatever the phone's pose), answers from its latest results, and ends a source
that nobody asked for in about a second. So the first call for a new point returns no hits
and later calls for the same point do: ask each frame (a reticle in the middle of the
screen), or keep asking for a few frames after a tap, as `examples/planes` does.

## The picture

In WebXR AR the browser composites the camera itself; the page never gets the camera
frames. `frame.has_image` says whether this frame has a picture:

- **Without raw camera access** (most of the time): `has_image` is false, `image` has no
  planes, and its width and height are the view's (the framebuffer's pixels). Draw nothing
  for the background: the camera is already there.
- **With raw camera access** (`camera-access`, granted by Chrome on Android; it asks the
  user for the camera): the view's camera image is a WebGL texture, which the package draws
  into a texture of the size `image_width` x `image_height` wishes for and reads back each
  frame as RGBA (`image.format` 4, one plane, rows top first). It is the picture of the view,
  so the intrinsics fit it. Reading it back costs time each frame (a copy from the GPU);
  use it for computer vision on the frame, not to draw the background.

## Drawing

What the app can draw in AR on the web is limited by the browsers, not by the session:

- **The session's own layer.** WebXR shows only what is drawn into the session's layers. The
  package gives the session a WebGL layer it clears each frame, so the camera shows through;
  a WebXR session delivers no frames without a layer.
- **WebGPU in WebXR.** eskiu-gpu draws with WebGPU (emdawnwebgpu on the web). Drawing into
  a WebXR session with WebGPU needs the WebXR/WebGPU binding (`XRGPUBinding`), which in
  Chrome is an experiment behind two flags ("WebXR Projection Layers" and "WebXR/WebGPU
  Bindings", since Chrome 135 on Android and Windows) and whose specification is an
  Editor's Draft. Shipping browsers do not have it, so an app cannot draw 3D
  content with eskiu-gpu into the session today, and the package does not try.
- **The page over the camera.** With 'dom-overlay' (Chrome on Android) the app's page shows
  over the camera during the session, so its interface (buttons, text, the status) stays
  usable. A transparent eskiu-gpu canvas in the page is shown too, drawn with
  `frame.projection` and `frame.view` like on the phones, but the overlay is composited
  apart from the camera, so content may trail the camera by a frame or so; it suits labels
  and markers more than objects that must sit still on a table.

When the WebGPU binding ships, drawing into the session belongs in eskiu-gpu (a surface
over an `XRGPUBinding` projection layer); the session data here stays the same.

## What was checked, what needs a phone

Checked in headless Chrome against the example's web build (`ui build web`): without WebXR
AR the app shows `ArUnsupported`; with a stand-in `navigator.xr` (a session, a viewer pose,
a plane, a light probe, hit-test sources and anchors), the status goes to
`ArWaitingForTap`, a click starts the session with the features above, frames reach the
handler with the camera's position, the view's size, the plane and the light, and an anchor
is placed from a hit test answered on a later frame.

Needs Chrome on an ARCore phone: the session itself, the projection and pose against the
camera the browser draws, planes, anchors, light, depth and hit tests on real surfaces, the
camera readback (and its row order), image tracking with the flag, and how the DOM overlay
lines up with the camera.
