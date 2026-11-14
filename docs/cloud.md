# Cloud anchors, Earth and semantics

`ar` reaches ARCore's cloud anchors, its Geospatial API and its scene semantics, as
ViroReact's `hostCloudAnchor`, `resolveCloudAnchor`, geospatial anchors and semantics do:
through ARCore itself on Android, and on iOS through Google's ARCore SDK for iOS running
beside ARKit (a `GARSession` fed each ARKit frame), when the app adds it. Without it the iOS
calls start nothing and a job reads `ArJobUnknown`.

All of it needs the app's Google Cloud API key with the ARCore API enabled. On Android, in the
manifest:

```toml
[android.meta_data]
"com.google.android.ar.API_KEY" = "AIza..."
```

and Earth also needs the location permission (`android.permission.ACCESS_FINE_LOCATION`).
On iOS, the SDK's Swift package (the products for what the app uses) and the key in
Info.plist as `GARAPIKey`; Earth asks for the location when in use:

```toml
[ios.spm]
ARCore = { url = "https://github.com/google-ar/arcore-ios-sdk", exact = "1.57.1", products = ["ARCoreCloudAnchors", "ARCoreGeospatial", "ARCoreSemantics"] }

[ios.plist]
GARAPIKey = "AIza..."
NSLocationWhenInUseUsageDescription = "Places content where it belongs in the world"
```

(1.57.1, not 1.57.0: the 1.57.0 manifest's checksums do not match Google's downloads. The SDK
brings Firebase and adds about 40 MB to an app.)

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

`ar_vps_check(lat, lng)` asks whether Google's VPS covers a place: the job is done when it
does and fails with code 2 when it does not.

ARCore must be tracking Earth for these (`earth.tracking` normal), which needs Google's VPS
coverage or a good GPS fix.

## Scene semantics

With `semantics` (ARCore's Scene Semantics, for outdoor scenes), each frame has
`semantics`: an image aligned with the camera picture, one `ArSemanticLabel` per pixel (sky,
building, tree, road, sidewalk, terrain, structure, object, vehicle, person, water) and its
confidence. `ArSemantics_label(x, y)` reads one, `ArSemantics_fraction(label)` is the share of
the picture with it (ARCore's label fraction). Sample it through `ArFrame_display_transform`
to mask content, as ViroReact's semantic masks do.
