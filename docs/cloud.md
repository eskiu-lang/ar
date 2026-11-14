# Cloud anchors, Earth and semantics

On Android, `ar` reaches ARCore's cloud anchors and its Geospatial API, as ViroReact's
`hostCloudAnchor`, `resolveCloudAnchor` and geospatial anchors do with ARCore. ARKit has
neither, so on iOS the calls start nothing and a job reads `ArJobUnknown`; for anchors shared
across platforms use ReactVision's cloud anchors (the `spatial` package).

Both need the app's Google Cloud API key with the ARCore API enabled, in the manifest:

```toml
[android.meta_data]
"com.google.android.ar.API_KEY" = "AIza..."
```

and Earth also needs the location permission (`android.permission.ACCESS_FINE_LOCATION`).

## Cloud anchors

```eskiu
use_ar_session(ArOptions{cloud_anchors: true}, void(*ArFrame frame) { ... });
int64 job = ar_host_cloud_anchor(anchor, 30);        // an anchor from ar_anchor_add, kept 30 days
...each frame:
ArJob j;
if (ar_job(job, &j) == ArJobDone) share(j.cloud_id.cstr());
...on another device:
int64 r = ar_resolve_cloud_anchor(cloud_id);         // done: j.anchor is in frame.anchors
```

A job is ARCore's future: `ar_job` says whether it is running, done or failed (`code` is
ARCore's state: not authorized, not found, too little seen...), and tells a finished one once.
Hosting wants the anchor seen from several angles first, as ARCore asks.

## Earth

With `geospatial`, each frame has `has_earth` and `earth`: whether Earth is enabled and
tracked, and the camera's latitude, longitude, altitude (above the WGS84 ellipsoid), their
accuracies, its east-up-south rotation and the yaw's accuracy. Anchors at places on Earth:

```eskiu
float[4] up = {0.0, 0.0, 0.0, 1.0};                  // east-up-south quaternion
int64 a = ar_earth_anchor(lat, lng, altitude, &up[0]);            // at once, in frame.anchors
int64 t = ar_terrain_anchor(lat, lng, 1.5, &up[0]);   // a job: 1.5 m above the ground there
int64 r = ar_rooftop_anchor(lat, lng, 0.0, &up[0]);   // a job: on the building's roof
```

ARCore must be tracking Earth for these (`earth.tracking` normal), which needs Google's VPS
coverage or a good GPS fix.

## Scene semantics

With `semantics` (Android, ARCore's Scene Semantics, for outdoor scenes), each frame has
`semantics`: an image aligned with the camera picture, one `ArSemanticLabel` per pixel (sky,
building, tree, road, sidewalk, terrain, structure, object, vehicle, person, water) and its
confidence. `ArSemantics_label(x, y)` reads one, `ArSemantics_fraction(label)` is the share of
the picture with it (ARCore's label fraction). Sample it through `ArFrame_display_transform`
to mask content, as ViroReact's semantic masks do.
