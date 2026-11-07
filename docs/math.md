# The geometry

`<ar/math>` computes the view, the projection, the display transform and hit-test rays from
three things each platform gives: the camera picture's intrinsics, the camera's pose, and how
far the picture turns to be upright on the screen. The same code runs on iOS and Android, and
`tests/math.esk` checks it on the computer.

## Conventions

- **World.** Right handed, y up, meters, as ARKit and ARCore have it.
- **Camera pose.** Camera to world. Its axes follow the picture as the sensor gives it: x to
  the picture's right, y to its top, looking down -z. This is ARKit's `ARCamera.transform`
  and ARCore's `ArCamera_getPose` ("right and up relative to the image readout").
- **Picture.** Pixels from the top left, as the sensor gives it (landscape on a phone held
  upright). `rotation` (0, 90, 180, 270) turns it clockwise to be upright on the screen: on
  iOS from the interface orientation (portrait 90, landscape right 0, landscape left 180,
  upside down 270); on Android the back camera's sensor orientation minus the display's
  rotation.
- **Intrinsics.** `fx`, `fy`, `cx`, `cy` in pixels of the picture, for its `width` x `height`.
  A camera-space point (X, Y, Z) is at x = fx X / -Z + cx, y = cy - fy Y / -Z.
- **Viewport.** The app's view, `width` x `height` in any unit, from the top left. The
  upright picture covers it: scaled by s = max(width / W', height / H') (W', H' the upright
  size), centered, what overflows cropped. This is eskiu-gpu's `GPU_YUV_COVER` and what
  ARKit's `displayTransform` and ARCore's display geometry do.
- **Matrices.** 4x4, column major (`m[c * 4 + r]`), eskiu-gpu's `Mat4` layout.

## Upright intrinsics

Turning the picture turns its intrinsics (`ArIntrinsics_upright`):

| rotation | fx', fy' | cx', cy' | size |
|---|---|---|---|
| 0 | fx, fy | cx, cy | W x H |
| 90 | fy, fx | H - cy, cx | H x W |
| 180 | fx, fy | W - cx, H - cy | W x H |
| 270 | fy, fx | cy, W - cx | H x W |

## Projection

`ArIntrinsics_projection` scales the upright intrinsics into the viewport (fx_v = s fx',
cx_v = (cx' - W'/2) s + width/2, the same for y) and builds:

```
| 2 fx_v / width   0                 1 - 2 cx_v / width     0               |
| 0                2 fy_v / height   2 cy_v / height - 1    0               |
| 0                0                 far / (near - far)     near far / (near - far) |
| 0                0                 -1                     0               |
```

Depth goes from 0 at `near` to 1 at `far` (WebGPU and Metal; for OpenGL's -1 to 1, scale
and shift z). It is eskiu-gpu's `mat4_perspective` when the principal point is centered.

## View

`ar_view_matrix(camera, rotation)` is the inverse of `camera * Rz(rotation)`: the camera
turned about its z axis by the same quarter turns as the picture, so x is right and y up on
the screen. Projection times view puts a world point at the viewport pixel where the
covering picture shows it.

## Display transform

`ArIntrinsics_display_transform` maps normalized viewport coordinates (0 to 1 from the top
left) to normalized coordinates of the picture as stored. A viewport point goes to the
upright picture by the cover fit, then back through the rotation:

| rotation | stored x, y from upright x', y' |
|---|---|
| 0 | x', y' |
| 90 | y', H - x' |
| 180 | W - x', H - y' |
| 270 | W - y', x' |

The result is an `ArAffine` (Core Graphics' convention), built from where three viewport
corners land.

## Rays

`ArIntrinsics_ray` takes a viewport point to a picture pixel (the display transform), then
to the camera-space direction ((x - cx) / fx, -(y - cy) / fy, -1), then through the pose:
origin the camera's position, direction normalized. Hit tests send this ray to ARKit
(`ARRaycastQuery`) and ARCore (`ArFrame_hitTestRay`), so neither needs the view's size.

## Tests

`tests/math.esk` checks, for 400 random cameras (two picture sizes, every rotation, portrait,
landscape and square viewports) and a random point in front of each, that projection times
view puts it at the viewport pixel the display transform maps to its picture pixel (within
0.05 pixels), that the affine form agrees, that its depth is between 0 and 1, and that the
ray under that pixel passes within 0.1 mm of it. It also checks the viewport's center and
corners, portrait's corner mapping, the depth at near and far, the color temperature
tint, and the light scale ([session](session.md#light)): ARKit's 1000 lumens and ARCore's
middle gray are 1.0, and both double when the light doubles.

What needs a device: that each platform's picture, pose and rotation follow these
conventions on a phone held in each orientation (a point placed with a hit test should stay
on the surface it was placed on as the phone turns).
