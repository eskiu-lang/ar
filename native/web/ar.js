// <ar> on the web: the calls src/ar.esk makes, over window.eskiuAr (ar.mjs, the WebXR
// immersive-ar session). Each copies the lent frame's arrays into the app's memory in
// src/ar.esk's layouts; the camera picture (with WebXR raw camera access) and depth go into
// buffers of the app's heap kept from one frame to the next. Without WebXR AR the session
// reports "unsupported" on the "ar" channel and there are never frames.
mergeInto(LibraryManager.library, {
  $arHeap__deps: ['malloc', 'free'],
  // A buffer of the app's heap of at least `bytes`, kept under `name` (grown as needed).
  $arHeap: function(name, bytes) {
    const buffers = Module['arBuffers'] || (Module['arBuffers'] = {});
    const buffer = buffers[name] || (buffers[name] = {pointer: 0, bytes: 0});
    if (buffer.bytes < bytes) {
      if (buffer.pointer) _free(buffer.pointer);
      buffer.pointer = _malloc(bytes);
      buffer.bytes = buffer.pointer ? bytes : 0;
    }
    return buffer.pointer;
  },
  $arPut64: function(at, index, value) {
    HEAP32[(at >> 2) + index * 2] = value % 4294967296;
    HEAP32[(at >> 2) + index * 2 + 1] = Math.floor(value / 4294967296);
  },
  $arDoubles: function(at, values) {
    for (let i = 0; i < values.length; i++) HEAPF64[(at >> 3) + i] = values[i];
  },

  ar_native_session: function(on, flags, width, height) {
    if (window.eskiuAr) window.eskiuAr.session(on, flags, width, height);
    else if (on) setTimeout(() => Module.uiEmit('ar', 'unsupported'), 0);
  },

  // The image values (24 int64): the picture as one RGBA plane with raw camera access, else
  // no planes and the view's size (ArFrame.has_image false).
  ar_native_frame_acquire__deps: ['$arHeap', '$arPut64', '$arDoubles'],
  ar_native_frame_acquire: function(image, camera) {
    const frame = window.eskiuAr && window.eskiuAr.acquire();
    if (!frame) return 0;
    for (let i = 0; i < 24; i++) arPut64(image, i, 0);
    arPut64(image, 0, frame.width);
    arPut64(image, 1, frame.height);
    arPut64(image, 7, 1);
    arPut64(image, 8, frame.time);
    if (frame.rgba) {
      const pointer = arHeap('image', frame.rgba.length);
      if (!pointer) return 0;
      HEAPU8.set(frame.rgba, pointer);
      arPut64(image, 2, 4);
      arPut64(image, 3, 1);
      arPut64(image, 9, pointer);
      arPut64(image, 10, frame.width * 4);
      arPut64(image, 11, 4);
      arPut64(image, 12, frame.rgba.length);
    }
    arDoubles(camera, frame.camera);
    return 1;
  },
  ar_native_frame_release: function() { if (window.eskiuAr) window.eskiuAr.release(); },

  ar_native_plane_count: function() {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    return frame ? frame.planes.length : 0;
  },
  ar_native_plane__deps: ['$arDoubles'],
  ar_native_plane: function(index, info, polygon, maxPoints) {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    if (!frame || index < 0 || index >= frame.planes.length) return -1;
    const plane = frame.planes[index];
    arDoubles(info, plane.info);
    const n = Math.min(plane.polygon.length / 2, maxPoints);
    HEAPF32.set(plane.polygon.subarray(0, n * 2), polygon >> 2);
    return n;
  },
  ar_native_anchor_count: function() {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    return frame ? frame.anchors.length : 0;
  },
  ar_native_anchor__deps: ['$arDoubles'],
  ar_native_anchor: function(index, info) {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    arDoubles(info, frame && index >= 0 && index < frame.anchors.length ? frame.anchors[index] : new Array(18).fill(0));
  },
  // WebXR has no feature points.
  ar_native_points: function(out, maxPoints) { return 0; },
  ar_native_light__deps: ['$arDoubles'],
  ar_native_light: function(info) {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    if (!frame || !frame.light) return 0;
    arDoubles(info, frame.light);
    return 1;
  },
  // Depth in meters as 32-bit floats (format 0), lined up with the view.
  ar_native_depth__deps: ['$arHeap', '$arPut64'],
  ar_native_depth: function(info) {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    if (!frame || !frame.depth) return 0;
    const depth = frame.depth;
    const pointer = arHeap('depth', depth.meters.length * 4);
    if (!pointer) return 0;
    HEAPF32.set(depth.meters, pointer >> 2);
    arPut64(info, 0, depth.width);
    arPut64(info, 1, depth.height);
    arPut64(info, 2, 0);
    arPut64(info, 3, depth.width * 4);
    arPut64(info, 4, pointer);
    arPut64(info, 5, depth.meters.length * 4);
    return 1;
  },
  ar_native_hit_test__deps: ['$arDoubles'],
  ar_native_hit_test: function(ray, targets, out, maxHits) {
    if (!window.eskiuAr) return 0;
    const values = Array.from(HEAPF32.subarray(ray >> 2, (ray >> 2) + 6));
    const hits = window.eskiuAr.hitTest(values, targets).slice(0, maxHits);
    hits.forEach((hit, i) => arDoubles(out + i * 19 * 8, [hit.distance, hit.kind, 0, ...hit.matrix]));
    return hits.length;
  },
  ar_native_anchor_add: function(pose, plane) {
    if (!window.eskiuAr) return 0;
    return window.eskiuAr.anchorAdd(Array.from(HEAPF32.subarray(pose >> 2, (pose >> 2) + 16)));
  },
  ar_native_anchor_remove: function(id) { if (window.eskiuAr) window.eskiuAr.anchorRemove(id); },

  ar_native_images: function(count, tracked) { if (window.eskiuAr) window.eskiuAr.images(count); },
  // A target's gray pixels (copied) or its URL.
  ar_native_image_add__deps: ['$UTF8ToString'],
  ar_native_image_add: function(index, gray, width, height, path, physicalWidth) {
    if (!window.eskiuAr) return;
    const pixels = gray ? HEAPU8.slice(gray, gray + width * height) : null;
    window.eskiuAr.imageAdd(index, pixels, width, height, path ? UTF8ToString(path) : null, physicalWidth);
  },
  ar_native_image_state: function(index) { return window.eskiuAr ? window.eskiuAr.imageState(index) : 2; },
  ar_native_tracked_image_count: function() {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    return frame ? frame.images.length : 0;
  },
  ar_native_tracked_image__deps: ['$arDoubles'],
  ar_native_tracked_image: function(index, info) {
    const frame = window.eskiuAr && window.eskiuAr.lent();
    arDoubles(info, frame && index >= 0 && index < frame.images.length ? frame.images[index] : new Array(22).fill(0));
  },
});
