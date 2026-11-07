// <ar> on the web: a WebXR immersive-ar session (Chrome on Android), the logic ar.js calls
// through window.eskiuAr. The browser composites the camera itself; this module keeps the
// newest frame's data as plain arrays in src/ar.esk's layouts (the camera's pose and the
// view's intrinsics, planes, anchors, the light probe, depth, image targets) and posts
// "frame" on the "ar_frame" channel when none is waiting. The camera picture comes only with
// WebXR raw camera access ('camera-access'), read back from its WebGL texture as RGBA.
//
// An immersive session starts only from a user's tap: after "tap" on the "ar" channel the
// next tap (or click) anywhere on the page starts it. Features the browser lacks are left
// out: planes ('plane-detection'), anchors ('anchors': without them an anchor keeps the pose
// it was given), light ('light-estimation'), depth ('depth-sensing'), image targets
// ('image-tracking', still behind a flag in Chrome). Hit tests are asynchronous in WebXR: a
// ray asked for starts a hit-test source in the viewer's space and the results come from the
// next frames, so the first call for a new screen point finds nothing.
//
// The session draws nothing into its WebGL layer but a clear (WebXR delivers frames only to a
// session with a layer); the page shows over the camera through 'dom-overlay' when the
// browser has it. eskiu-gpu (WebGPU) cannot draw into the session itself: WebXR's WebGPU
// binding is behind flags in Chrome (docs/web.md).
export function setup(ui) {
  const emit = (channel, payload) => ui.emit(channel, payload);
  const xr = navigator.xr;

  const state = {
    wanted: false, flags: 0, wishWidth: 1280, wishHeight: 720,
    session: null, refSpace: null, viewerSpace: null, gl: null, glBinding: null, layer: null,
    armed: null, starting: false, probe: null, posted: false,
    latest: null, lent: null,
    planeIds: new Map(), nextId: 1,
    anchors: [], hitSources: new Map(), frameCount: 0,
    targets: [], targetBitmaps: [], targetStates: [], imagesTracked: 1, imageIds: new Map(), imageScores: null,
    readback: null,
  };

  const FLAG_HORIZONTAL = 1, FLAG_VERTICAL = 2, FLAG_LIGHT = 4, FLAG_DEPTH = 8, FLAG_LIGHT_HDR = 32, FLAG_IMAGES = 64;

  // A column-major 4x4 matrix's rotation as a quaternion (x, y, z, w).
  const quaternion = m => {
    const trace = m[0] + m[5] + m[10];
    let x, y, z, w;
    if (trace > 0) {
      const s = Math.sqrt(trace + 1) * 2;
      w = 0.25 * s; x = (m[6] - m[9]) / s; y = (m[8] - m[2]) / s; z = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
      const s = Math.sqrt(1 + m[0] - m[5] - m[10]) * 2;
      w = (m[6] - m[9]) / s; x = 0.25 * s; y = (m[4] + m[1]) / s; z = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
      const s = Math.sqrt(1 + m[5] - m[0] - m[10]) * 2;
      w = (m[8] - m[2]) / s; x = (m[4] + m[1]) / s; y = 0.25 * s; z = (m[9] + m[6]) / s;
    } else {
      const s = Math.sqrt(1 + m[10] - m[0] - m[5]) * 2;
      w = (m[1] - m[4]) / s; x = (m[8] + m[2]) / s; y = (m[9] + m[6]) / s; z = 0.25 * s;
    }
    return {x, y, z, w};
  };

  const rigid = m => new XRRigidTransform({x: m[12], y: m[13], z: m[14], w: 1}, quaternion(m));

  // a * b, column-major.
  const multiply = (a, b) => {
    const out = new Array(16).fill(0);
    for (let c = 0; c < 4; c++)
      for (let r = 0; r < 4; r++)
        for (let k = 0; k < 4; k++) out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return out;
  };

  // The inverse of a rigid transform (rotation and translation).
  const rigidInverse = m => {
    const out = [m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0, 0, 0, 0, 1];
    for (let r = 0; r < 3; r++) out[12 + r] = -(out[r] * m[12] + out[4 + r] * m[13] + out[8 + r] * m[14]);
    return out;
  };

  const point = (m, p) => [0, 1, 2].map(r => m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
  const direction = (m, d) => [0, 1, 2].map(r => m[r] * d[0] + m[4 + r] * d[1] + m[8 + r] * d[2]);

  const status = text => emit('ar', text);

  // The session's features, as the flags ask.
  const sessionInit = () => {
    const optional = ['local-floor', 'hit-test', 'anchors', 'dom-overlay', 'camera-access'];
    const init = {requiredFeatures: ['local'], optionalFeatures: optional};
    if (state.flags & (FLAG_HORIZONTAL | FLAG_VERTICAL)) optional.push('plane-detection');
    if (state.flags & (FLAG_LIGHT | FLAG_LIGHT_HDR)) optional.push('light-estimation');
    if (state.flags & FLAG_DEPTH) {
      optional.push('depth-sensing');
      init.depthSensing = {usagePreference: ['cpu-optimized'], dataFormatPreference: ['luminance-alpha', 'float32']};
    }
    const images = [];
    if (state.flags & FLAG_IMAGES) {
      state.targetBitmaps.forEach((bitmap, index) => {
        if (bitmap && state.targetStates[index] !== 2) images.push({index, image: bitmap, widthInMeters: state.targets[index].meters});
      });
    }
    if (images.length > 0) {
      optional.push('image-tracking');
      init.trackedImages = images.map(t => ({image: t.image, widthInMeters: t.widthInMeters}));
      state.trackedIndex = images.map(t => t.index);
    } else state.trackedIndex = [];
    const overlay = document.querySelector('#app') || document.body;
    init.domOverlay = {root: overlay};
    return init;
  };

  // Starts the session from a tap (the user activation an immersive session needs).
  const start = async () => {
    if (state.session || state.starting || !state.wanted) return;
    state.starting = true;
    disarm();
    let session;
    try {
      await Promise.all(state.targetBitmaps.map(b => b && b.ready));
      session = await xr.requestSession('immersive-ar', sessionInit());
    } catch (error) {
      state.starting = false;
      if (!state.wanted) return;
      status(error && error.name === 'NotAllowedError' ? 'denied' : 'failed');
      if (error && error.name === 'NotAllowedError') arm(false);
      return;
    }
    state.starting = false;
    if (!state.wanted) { session.end(); return; }
    try {
      const canvas = document.createElement('canvas');
      const gl = canvas.getContext('webgl2', {xrCompatible: true, alpha: true}) ||
                 canvas.getContext('webgl', {xrCompatible: true, alpha: true});
      state.gl = gl;
      state.layer = new XRWebGLLayer(session, gl, {alpha: true});
      session.updateRenderState({baseLayer: state.layer});
      if (session.enabledFeatures?.includes('camera-access') && typeof XRWebGLBinding !== 'undefined') {
        state.glBinding = new XRWebGLBinding(session, gl);
      }
      state.refSpace = await session.requestReferenceSpace('local');
      state.viewerSpace = await session.requestReferenceSpace('viewer');
      state.probe = null;
      if ((state.flags & (FLAG_LIGHT | FLAG_LIGHT_HDR)) && session.requestLightProbe) {
        session.requestLightProbe().then(probe => { state.probe = probe; }, () => {});
      }
      if (state.trackedIndex.length > 0 && session.getTrackedImageScores) {
        session.getTrackedImageScores().then(scores => {
          scores.forEach((score, i) => { state.targetStates[state.trackedIndex[i]] = score === 'untrackable' ? 2 : 1; });
        }, () => {});
      }
    } catch (error) {
      session.end();
      status('failed');
      return;
    }
    state.session = session;
    state.planeIds.clear();
    state.imageIds.clear();
    state.anchors = [];
    state.hitSources.clear();
    session.addEventListener('end', () => {
      state.session = null;
      state.latest = null;
      state.readback = null;
      // The user left AR (the back button), or new options ended it: the next tap starts it.
      if (state.wanted) arm(true);
    });
    session.addEventListener('visibilitychange', () => {
      if (session.visibilityState === 'hidden') status('paused');
      else if (session.visibilityState === 'visible') status('running');
    });
    session.requestAnimationFrame(onFrame);
    status('running');
  };

  const onTap = () => { start(); };

  // Waits for the next tap to start the session (and says so on the "ar" channel).
  const arm = say => {
    if (!state.armed) {
      state.armed = onTap;
      addEventListener('click', onTap, true);
      addEventListener('touchend', onTap, true);
    }
    if (say) status('tap');
  };

  const disarm = () => {
    if (!state.armed) return;
    removeEventListener('click', state.armed, true);
    removeEventListener('touchend', state.armed, true);
    state.armed = null;
  };

  // The view's intrinsics for an image of width x height, from its projection matrix (the
  // inverse of src/math.esk's projection for rotation 0).
  const intrinsics = (p, width, height) => [p[0] * width / 2, p[5] * height / 2, (1 - p[8]) * width / 2, (1 + p[9]) * height / 2];

  // The camera picture through WebXR raw camera access: the view's camera texture (opaque:
  // it can only be sampled) drawn into a texture of the size wished for, rows top first,
  // and read back as RGBA. Null without camera access.
  const readCamera = (frame, view) => {
    const gl = state.gl;
    if (!state.glBinding || !view.camera) return null;
    const texture = state.glBinding.getCameraImage(view.camera);
    if (!texture) return null;
    const cw = view.camera.width, ch = view.camera.height;
    const longest = Math.max(state.wishWidth, state.wishHeight);
    const scale = Math.min(1, longest / Math.max(cw, ch));
    const width = Math.max(1, Math.round(cw * scale)), height = Math.max(1, Math.round(ch * scale));
    let rb = state.readback;
    if (!rb || rb.width !== width || rb.height !== height) {
      if (!rb) {
        const shader = (type, text) => {
          const s = gl.createShader(type);
          gl.shaderSource(s, text);
          gl.compileShader(s);
          return s;
        };
        const program = gl.createProgram();
        gl.attachShader(program, shader(gl.VERTEX_SHADER,
          'attribute vec2 p; varying vec2 uv; void main() { uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }'));
        gl.attachShader(program, shader(gl.FRAGMENT_SHADER,
          'precision mediump float; varying vec2 uv; uniform sampler2D t; void main() { gl_FragColor = texture2D(t, uv); }'));
        gl.linkProgram(program);
        const quad = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, quad);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
        rb = {program, quad, where: gl.getAttribLocation(program, 'p'), framebuffer: gl.createFramebuffer(), color: gl.createTexture()};
      }
      rb.width = width;
      rb.height = height;
      rb.pixels = new Uint8Array(width * height * 4);
      gl.bindTexture(gl.TEXTURE_2D, rb.color);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.bindFramebuffer(gl.FRAMEBUFFER, rb.framebuffer);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, rb.color, 0);
      state.readback = rb;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, rb.framebuffer);
    gl.viewport(0, 0, width, height);
    gl.useProgram(rb.program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.bindBuffer(gl.ARRAY_BUFFER, rb.quad);
    gl.enableVertexAttribArray(rb.where);
    gl.vertexAttribPointer(rb.where, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, rb.pixels);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return {width, height, rgba: rb.pixels.slice()};
  };

  // The detected planes: the pose moved to the outline's middle, the outline about it.
  const readPlanes = frame => {
    const out = [];
    const seen = new Set();
    if (!frame.detectedPlanes) return out;
    for (const plane of frame.detectedPlanes) {
      const vertical = plane.orientation === 'vertical';
      if (vertical ? !(state.flags & FLAG_VERTICAL) : !(state.flags & FLAG_HORIZONTAL)) continue;
      const pose = frame.getPose(plane.planeSpace, state.refSpace);
      if (!pose) continue;
      seen.add(plane);
      if (!state.planeIds.has(plane)) state.planeIds.set(plane, state.nextId++);
      let minX = Infinity, maxX = -Infinity, minZ = Infinity, maxZ = -Infinity;
      for (const p of plane.polygon) {
        minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x);
        minZ = Math.min(minZ, p.z); maxZ = Math.max(maxZ, p.z);
      }
      if (!(maxX >= minX)) { minX = maxX = minZ = maxZ = 0; }
      const cx = (minX + maxX) / 2, cz = (minZ + maxZ) / 2;
      const m = Array.from(pose.transform.matrix);
      const center = multiply(m, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, cx, 0, cz, 1]);
      const info = new Array(22).fill(0);
      info[0] = state.planeIds.get(plane);
      info[1] = vertical ? 2 : m[5] < 0 ? 1 : 0;
      info[2] = pose.emulatedPosition ? 1 : 2;
      for (let i = 0; i < 16; i++) info[3 + i] = center[i];
      info[19] = maxX - minX;
      info[20] = maxZ - minZ;
      const polygon = new Float32Array(Math.min(plane.polygon.length, 256) * 2);
      for (let i = 0; i < polygon.length / 2; i++) {
        polygon[i * 2] = plane.polygon[i].x - cx;
        polygon[i * 2 + 1] = plane.polygon[i].z - cz;
      }
      out.push({info, polygon});
    }
    for (const plane of state.planeIds.keys()) if (!seen.has(plane)) state.planeIds.delete(plane);
    return out;
  };

  // The app's anchors: made on this frame when they were asked for since the last one; a
  // browser without anchors keeps each one at the pose it was given.
  const readAnchors = frame => {
    const out = [];
    for (const entry of state.anchors) {
      if (!entry.asked && frame.createAnchor && state.session.enabledFeatures?.includes('anchors')) {
        entry.asked = true;
        frame.createAnchor(rigid(entry.pose), state.refSpace).then(anchor => {
          if (entry.removed) anchor.delete();
          else entry.anchor = anchor;
        }, () => { entry.failed = true; });
      }
      const info = new Array(18).fill(0);
      info[0] = entry.id;
      let pose = entry.pose, tracking = 2;
      if (entry.anchor) {
        const tracked = frame.trackedAnchors ? frame.trackedAnchors.has(entry.anchor) : true;
        const p = tracked ? frame.getPose(entry.anchor.anchorSpace, state.refSpace) : null;
        if (p) {
          pose = Array.from(p.transform.matrix);
          entry.pose = pose;
          tracking = p.emulatedPosition ? 1 : 2;
        } else tracking = 0;
      }
      info[1] = tracking;
      for (let i = 0; i < 16; i++) info[2 + i] = pose[i];
      out.push(info);
    }
    return out;
  };

  // The light probe's estimate: the main light (its direction toward the light in the
  // session's space) and the 9 RGB spherical harmonics, source 4 (WebXR).
  const readLight = frame => {
    if (!state.probe || !frame.getLightEstimate) return null;
    const estimate = frame.getLightEstimate(state.probe);
    if (!estimate) return null;
    const info = new Array(42).fill(0);
    info[0] = 1;
    info[1] = 1;
    info[2] = 6500;
    info[3] = info[4] = info[5] = 1;
    const probePose = frame.getPose(state.probe.probeSpace, state.refSpace);
    const d = estimate.primaryLightDirection;
    let dir = [d.x, d.y, d.z];
    if (probePose) dir = direction(Array.from(probePose.transform.matrix), dir);
    info[6] = 1;
    for (let k = 0; k < 3; k++) info[7 + k] = dir[k];
    const i3 = estimate.primaryLightIntensity;
    info[10] = i3.x; info[11] = i3.y; info[12] = i3.z;
    const sh = estimate.sphericalHarmonicsCoefficients;
    if (sh && sh.length >= 27) {
      info[13] = 1;
      for (let k = 0; k < 27; k++) info[14 + k] = sh[k];
    }
    info[41] = 4;
    return info;
  };

  // The view's depth in meters (32-bit floats), resampled to the view's orientation through
  // getDepthInMeters (normalized view coordinates), so it lines up with the picture.
  const readDepth = (frame, view, viewWidth, viewHeight) => {
    if (!frame.getDepthInformation) return null;
    let depth;
    try { depth = frame.getDepthInformation(view); } catch (error) { return null; }
    if (!depth || !depth.getDepthInMeters) return null;
    let width = depth.width, height = depth.height;
    if ((width > height) !== (viewWidth > viewHeight)) [width, height] = [height, width];
    const meters = new Float32Array(width * height);
    for (let y = 0; y < height; y++)
      for (let x = 0; x < width; x++) {
        const value = depth.getDepthInMeters((x + 0.5) / width, (y + 0.5) / height);
        meters[y * width + x] = value > 0 ? value : 0;
      }
    return {width, height, meters};
  };

  // The image targets found ('image-tracking', behind Chrome's WebXR Incubations flag): pose,
  // how they are followed (tracked: full; emulated: last known) and the width measured (the
  // height from the picture's aspect).
  const readImages = frame => {
    const out = [];
    if (!frame.getImageTrackingResults) return out;
    for (const result of frame.getImageTrackingResults()) {
      const index = state.trackedIndex[result.index];
      if (index === undefined) continue;
      const pose = frame.getPose(result.imageSpace, state.refSpace);
      if (!pose) continue;
      if (!state.imageIds.has(index)) state.imageIds.set(index, state.nextId++);
      const info = new Array(22).fill(0);
      info[0] = state.imageIds.get(index);
      info[1] = index;
      const tracked = result.trackingState === 'tracked';
      info[2] = tracked ? 2 : 1;
      info[3] = tracked ? 1 : 2;
      // WebXR's image space has x to the right, y to the top and z out of the image; the
      // package's has y out of it and z toward the bottom (ARKit's and ARCore's).
      const m = pose.transform.matrix;
      const axes = [m[0], m[1], m[2], 0, m[8], m[9], m[10], 0, -m[4], -m[5], -m[6], 0, m[12], m[13], m[14], 1];
      for (let i = 0; i < 16; i++) info[4 + i] = axes[i];
      const target = state.targets[index];
      const width = result.measuredWidthInMeters || target.meters;
      info[20] = width;
      info[21] = target.aspect > 0 ? width * target.aspect : width;
      out.push(info);
    }
    return out;
  };

  // The hit-test sources' results on this frame; sources not asked for in a second end.
  const readHits = frame => {
    for (const [key, source] of state.hitSources) {
      if (state.frameCount - source.asked > 60) {
        source.planes?.cancel(); source.points?.cancel();
        state.hitSources.delete(key);
        continue;
      }
      const results = [];
      for (const [kind, test] of [[1, source.planes], [2, source.points]]) {
        if (!test) continue;
        for (const result of frame.getHitTestResults(test)) {
          const pose = result.getPose(state.refSpace);
          if (pose) results.push({kind, matrix: Array.from(pose.transform.matrix)});
        }
      }
      source.results = results;
    }
  };

  const onFrame = (time, frame) => {
    const session = frame.session;
    if (session !== state.session) return;
    session.requestAnimationFrame(onFrame);
    state.frameCount++;
    const gl = state.gl;
    const layer = session.renderState.baseLayer;
    const viewer = frame.getViewerPose(state.refSpace);
    const view = viewer && viewer.views[0];
    const picture = view ? readCamera(frame, view) : null;
    // A clear layer: the camera, which the browser draws, shows through.
    gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (!view) return;
    const viewport = layer.getViewport(view);
    const width = picture ? picture.width : viewport.width;
    const height = picture ? picture.height : viewport.height;
    const camera = new Array(24).fill(0);
    camera[0] = viewer.emulatedPosition ? 1 : 2;
    camera[1] = viewer.emulatedPosition ? 5 : 0;
    const m = view.transform.matrix;
    for (let i = 0; i < 16; i++) camera[2 + i] = m[i];
    const k = intrinsics(view.projectionMatrix, width, height);
    for (let i = 0; i < 4; i++) camera[18 + i] = k[i];
    camera[22] = width;
    camera[23] = height;
    readHits(frame);
    state.viewerMatrix = Array.from(viewer.transform.matrix);
    state.latest = {
      width, height, time: Math.round(time * 1e6), rgba: picture ? picture.rgba : null, camera,
      planes: (state.flags & (FLAG_HORIZONTAL | FLAG_VERTICAL)) ? readPlanes(frame) : [],
      anchors: readAnchors(frame),
      light: (state.flags & (FLAG_LIGHT | FLAG_LIGHT_HDR)) ? readLight(frame) : null,
      depth: (state.flags & FLAG_DEPTH) ? readDepth(frame, view, viewport.width, viewport.height) : null,
      images: (state.flags & FLAG_IMAGES) ? readImages(frame) : [],
    };
    if (!state.posted) {
      state.posted = true;
      emit('ar_frame', 'frame');
    }
  };

  const end = () => {
    disarm();
    if (state.session) state.session.end();
    state.session = null;
    state.latest = null;
    state.posted = false;
  };

  // An image target's picture as an ImageBitmap (from gray pixels, or fetched from a URL),
  // with `ready` settling when it is made.
  const makeBitmap = (index, gray, width, height, url) => {
    const entry = {bitmap: null, ready: null};
    const make = async () => {
      let bitmap;
      if (gray) {
        const rgba = new Uint8ClampedArray(width * height * 4);
        for (let i = 0; i < width * height; i++) {
          rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = gray[i];
          rgba[i * 4 + 3] = 255;
        }
        bitmap = await createImageBitmap(new ImageData(rgba, width, height));
      } else {
        const response = await fetch(url);
        if (!response.ok) throw new Error('not found');
        bitmap = await createImageBitmap(await response.blob());
      }
      return bitmap;
    };
    entry.ready = make().then(bitmap => {
      state.targetBitmaps[index] = bitmap;
      state.targets[index].aspect = bitmap.height / bitmap.width;
      state.targetStates[index] = 1;
    }, () => { state.targetStates[index] = 2; state.targetBitmaps[index] = null; });
    return entry;
  };

  window.eskiuAr = {
    session(on, flags, width, height) {
      if (!on) { state.wanted = false; end(); return; }
      const restart = state.session && (flags !== state.flags || state.imagesChanged);
      state.imagesChanged = false;
      state.wanted = true;
      state.flags = flags;
      state.wishWidth = width;
      state.wishHeight = height;
      if (restart) {
        // WebXR fixes a session's features when it starts: a new set needs a new session,
        // and so a new tap.
        state.session.end();
        return;
      }
      if (state.session) return;
      if (!xr || typeof XRWebGLLayer === 'undefined') {
        console.warn('AR is not supported in this browser (no WebXR)');
        setTimeout(() => status('unsupported'), 0);
        return;
      }
      xr.isSessionSupported('immersive-ar').then(ok => {
        if (!state.wanted || state.session) return;
        if (!ok) { status('unsupported'); return; }
        arm(true);
      }, () => status('unsupported'));
    },

    // The newest frame, lent until release (or null).
    acquire() {
      state.posted = false;
      state.lent = state.latest;
      return state.lent;
    },
    release() { state.lent = null; },
    lent() { return state.lent; },

    // Hit tests along a world ray, from the sources asked for with the same ray in the
    // viewer's space (results from the latest frame); a new ray starts its sources.
    hitTest(ray, targets) {
      if (!state.session || !state.viewerMatrix) return [];
      const toViewer = rigidInverse(state.viewerMatrix);
      const origin = point(toViewer, ray.slice(0, 3));
      const dir = direction(toViewer, ray.slice(3, 6));
      const key = [...origin, ...dir].map(v => v.toFixed(3)).join(',') + ':' + targets;
      let source = state.hitSources.get(key);
      if (!source) {
        source = {asked: state.frameCount, results: [], planes: null, points: null};
        state.hitSources.set(key, source);
        const offsetRay = new XRRay({x: origin[0], y: origin[1], z: origin[2], w: 1}, {x: dir[0], y: dir[1], z: dir[2], w: 0});
        const ask = (kind, types) => state.session.requestHitTestSource?.({space: state.viewerSpace, offsetRay, entityTypes: types})
          .then(test => { if (state.hitSources.get(key) === source) source[kind] = test; else test.cancel(); }, () => {});
        if (targets & 1) ask('planes', ['plane']);
        if (targets & 2) ask('points', ['point']);
      }
      source.asked = state.frameCount;
      return source.results.map(hit => {
        const dx = hit.matrix[12] - ray[0], dy = hit.matrix[13] - ray[1], dz = hit.matrix[14] - ray[2];
        return {distance: Math.sqrt(dx * dx + dy * dy + dz * dz), kind: hit.kind, matrix: hit.matrix};
      }).sort((a, b) => a.distance - b.distance);
    },

    anchorAdd(matrix) {
      if (!state.session) return 0;
      const entry = {id: state.nextId++, pose: matrix, anchor: null, asked: false, removed: false};
      state.anchors.push(entry);
      return entry.id;
    },

    anchorRemove(id) {
      const at = state.anchors.findIndex(entry => entry.id === id);
      if (at < 0) return;
      const entry = state.anchors[at];
      entry.removed = true;
      if (entry.anchor) entry.anchor.delete();
      state.anchors.splice(at, 1);
    },

    // A new set of image targets: each one's bitmap is made at once (WebXR takes them when the
    // session starts, so a running session takes a new set from its next start).
    images(count) {
      state.imagesChanged = true;
      state.targets = new Array(count).fill(null).map(() => ({meters: 0, aspect: 0}));
      state.targetBitmaps = new Array(count).fill(null);
      state.targetStates = new Array(count).fill(2);
    },

    imageAdd(index, gray, width, height, url, meters) {
      if (index < 0 || index >= state.targets.length || !(meters > 0) || (!gray && !url)) return;
      state.targets[index].meters = meters;
      state.targetStates[index] = 0;
      state.targetBitmaps[index] = makeBitmap(index, gray, width, height, url);
    },

    imageState(index) { return index >= 0 && index < state.targetStates.length ? state.targetStates[index] : 2; },
  };
}
