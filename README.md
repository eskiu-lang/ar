<p align="center">
  <img src="https://raw.githubusercontent.com/eskiu-lang/eskiu/main/assets/logo.png" alt="Eskiu, a kitten with a ball of yarn" width="140">
</p>

<h2 align="center">ar</h2>
<p align="center">Augmented reality for apps on eskiu-lang/ui: ARKit on iOS, ARCore on Android.</p>
<p align="center"><sub>Part of the <a href="https://github.com/eskiu-lang">Eskiu</a> project</sub></p>

<p align="center">
  <a href="https://github.com/eskiu-lang/ar/actions/workflows/ci.yml"><img src="https://github.com/eskiu-lang/ar/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/version-0.1.0-6448d4" alt="Version 0.1.0">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="License: MIT"></a>
</p>

---

A world-tracking session that hands each camera frame to Eskiu: the picture as YUV planes
for eskiu-gpu to draw, the camera's pose, planes, anchors, hit tests, feature points, the
light estimate and depth. The view, projection and hit-test rays are computed in Eskiu, the
same on both platforms. Neither platform needs a GL context.

```eskiu
import <ar>;

bool tapped;          // set by a tap at (tap_x, tap_y) in the 390 x 844 view
float tap_x;
float tap_y;

void World() {
    use_ar_session(ArOptions{horizontal: true}, void(*ArFrame frame) {
        // Borrowed for this call. Draw frame.image with <gpu/yuv>, then the world with:
        ArMat4 view = frame.view();
        ArMat4 projection = frame.projection(390.0, 844.0, 0.05, 100.0);
        ArHit[1] hit;
        if (tapped && frame.hit_test(tap_x, tap_y, 390.0, 844.0, AR_HIT_PLANES, &hit[0], 1) > 0) {
            ar_anchor_add_hit(&hit[0]);
            tapped = false;
        }
    });
}
```

## Documentation

- [The session](docs/session.md): options, status, frames, planes, anchors, hit tests, threads
- [Drawing](docs/drawing.md): the camera picture and the world with eskiu-gpu
- [The geometry](docs/math.md): pose, projection, display transform, rays
- [Design](docs/design.md): ARCore without GL, ARKit frames, what needs a device
- [Platforms, the example and tests](docs/README.md)
- [API reference](docs/api.md): every public function and type

## Install

```bash
eskiuc add ar --git https://github.com/eskiu-lang/ar --rev v0.1.0
```

## License

MIT
