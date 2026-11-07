// <ar> on the web: no AR in this version (WebXR's immersive-ar is not supported yet). Starting
// a session reports "unsupported" on the "ar" channel; there are never frames.
mergeInto(LibraryManager.library, {
  ar_native_session: function(on, flags, width, height) {
    if (on) {
      console.warn('AR is not supported in the browser');
      setTimeout(() => Module.uiEmit('ar', 'unsupported'), 0);
    }
  },
  ar_native_frame_acquire: function(image, camera) { return 0; },
  ar_native_frame_release: function() {},
  ar_native_plane_count: function() { return 0; },
  ar_native_plane: function(index, info, polygon, maxPoints) { return -1; },
  ar_native_anchor_count: function() { return 0; },
  ar_native_anchor: function(index, info) {},
  ar_native_points: function(out, maxPoints) { return 0; },
  ar_native_light: function(info) { return 0; },
  ar_native_depth: function(info) { return 0; },
  ar_native_hit_test: function(ray, targets, out, maxHits) { return 0; },
  ar_native_anchor_add: function(pose, plane) { return 0; },
  ar_native_anchor_remove: function(id) {},
});
