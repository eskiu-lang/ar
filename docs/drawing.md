# Drawing the camera and the world

The package draws nothing: an AR app draws the camera picture as the background and its own
content over it, with [eskiu-gpu](https://github.com/eskiu-lang/gpu). `ArFrame.image` has the
field layout of ui-camera's `CameraFrame` and eskiu-gpu's `GpuYuvImage`, so `<gpu/yuv>`
uploads it by pointer, with no copy and no conversion on the CPU, and turns the YUV into RGB
in a shader.

```eskiu
import <gpu>;
import <gpu/yuv>;
import <ar>;

Gpu gpu;
GpuSurface surface;
GpuYuv background;      // gpu.yuv(surface.format), once

Result<int, string> draw(*ArFrame frame) {
    background.upload((*GpuYuvImage)&frame.image)?;    // read while upload runs
    float w = (float)surface.width;
    float h = (float)surface.height;
    GpuFrame f = surface.frame()?;
    GpuPass pass = gpu.pass(f.view, GpuColor{0.0, 0.0, 0.0, 1.0}, null);   // a depth view for 3D content
    GpuYuvDraw d = GpuYuvDraw{width: w, height: h, fit: GPU_YUV_COVER};
    background.draw(&pass, &d);
    // The world, lined up with the picture: the same viewport size, the same cover fit.
    ArMat4 vp = ar_mat4_mul(frame.projection(w, h, 0.05, 100.0), frame.view());
    draw_content(&pass, (*Mat4)&vp, frame);
    pass.end()?;
    surface.present(&f);
    return Ok<int, string>(0);
}
```

- **Upload in the handler.** The picture's planes are borrowed for the handler's call; `upload`
  reads them while it runs, so the frame can go back as soon as it returns. Call `draw` from
  the `use_ar_session` handler.
- **Cover, upright, not mirrored.** `GPU_YUV_COVER` fills the viewport and crops about the
  center, and `upright` (the default) turns the picture by `image.rotation`: the same fit
  `ArFrame_projection`, `ArFrame_display_transform` and `ArFrame_ray` assume. The back camera
  is not mirrored (`mirror` stays false).
- **Matrices.** `ArMat4` has `Mat4`'s layout, so `(*Mat4)&m` reads one as the other. Depth
  goes from 0 at `near` to 1 at `far`, as eskiu-gpu's own projections.
- **Content on a plane or an anchor.** A model placed at `plane.pose` or `anchor.pose`
  (times its own transform) sits on that surface; the pose's y axis is the surface's normal.
- **Light.** Multiply the model's lighting (in linear space) by `frame.light.intensity` and
  `frame.light.color`, both linear and 1.0 in a neutral scene ([session](session.md#light));
  with `light_hdr` on Android, `main_direction` and `main_intensity` give a directional light
  and `harmonics` the ambient light (9 RGB coefficients, ARCore's order and scale).
- **The web.** The browser draws the camera (`frame.has_image` is false): skip the
  background. eskiu-gpu cannot draw into a WebXR session in shipping browsers (WebGPU's
  WebXR binding is behind flags); a transparent canvas in the page shows over the camera
  through the DOM overlay ([web](web.md#drawing)).
- **Occlusion.** `frame.depth` lines up with the picture as stored; sample it through
  `ArFrame_display_transform` to hide content behind real surfaces.

## A background shader of your own

Without `<gpu/yuv>`, sample the picture's two textures (Y as `R8Unorm` of `width` x `height`,
UV as `RG8Unorm` of half that, uploaded with each plane's `row_stride`) at the coordinates
`ArFrame_display_transform` gives for each viewport corner, and convert with full-range
BT.601:

```
R = Y + 1.402 (V - 0.5)
G = Y - 0.344136 (U - 0.5) - 0.714136 (V - 0.5)
B = Y + 1.772 (U - 0.5)
```
