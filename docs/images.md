# Image targets

The session can look for pictures you give it (a poster, a book cover, a label) and report
where each one is in the world: ARKit's reference images on iOS, ARCore's augmented images
on Android, and WebXR's image tracking on the web where the browser has it.

```eskiu
import <ar>;

ArImageTarget[2] targets;
uint8[256 * 256] logo_gray;          // filled from somewhere: 8-bit gray, row after row

void Gallery() {
    targets[0] = ArImageTarget{name: "poster", path: "images/poster.jpg", physical_width: 0.42};
    targets[1] = ArImageTarget{name: "logo", pixels: &logo_gray[0], width: 256, height: 256,
                               format: ArPixelsGray8, physical_width: 0.1};
    ArOptions options = ArOptions{horizontal: false};
    options.images = &targets[0];
    options.image_count = 2;
    use_ar_session(options, void(*ArFrame frame) {
        for (i in 0..frame.image_count) {
            *ArTrackedImage found = &frame.images[i];
            // found.name, found.pose (x right, z toward the picture's bottom, y out of it),
            // found.extent_x and extent_z in meters, found.method
        }
    });
}
```

## Targets

| `ArImageTarget` field | What it is |
|---|---|
| `name` | Your name for it; `frame.images[i].name` gives it back (and `index`, its place in the array). |
| `pixels`, `width`, `height`, `format`, `row_stride` | The picture: `ArPixelsGray8` (one byte a pixel) or `ArPixelsRGBA8`, `row_stride` bytes from one row to the next (0 for tight rows). RGBA is turned gray with BT.601's weights, as the platforms see it. |
| `path` | Or a PNG or JPEG file: a file of the app's `assets/` (found with ui's `ui_asset_path`), or an absolute path. On the web it is a URL, relative to the page (the assets are served next to it). |
| `physical_width` | Its width in the world, in meters. Needed: ARKit takes no reference image without one, and ARCore finds the pose at the first sight with it instead of after a few views. |

The targets are copied when the session gets them, so the array only has to live through the
render that passes it. A new array, count or field (not new pixel contents behind the same
pointer) sends them again and starts the session again with them. At most `AR_MAX_IMAGES`
(100) are taken.

`ar_image_state(index)` says where one stands:

| State | When |
|---|---|
| `ArImagePending` | Being prepared. iOS checks each image first (`ARReferenceImage validateWithCompletionHandler:`) and adds it to the session when the whole set is checked; Android adds them when the session is configured; the web when it makes the image's bitmap. |
| `ArImageReady` | The session looks for it. |
| `ArImageRejected` | No pixels or file, a file that does not decode, no physical width, or a picture the platform refuses: too few features (ARKit's validation, ARCore's `AR_ERROR_IMAGE_INSUFFICIENT_QUALITY`, WebXR's "untrackable" score). Pictures with detail, contrast and no repeated pattern work best. |

## Found images

`frame.images` holds `frame.image_count` `ArTrackedImage`s, one for each target seen so far:

| Field | What it is |
|---|---|
| `id` | Stays the same while the session knows the image. |
| `index`, `name` | Which target. |
| `pose` | The picture's center in the world: x to its right, z toward its bottom, y out of its face (ARCore's `ArAugmentedImage_getCenterPose`, ARKit's `ARImageAnchor` transform; WebXR's image space turned to match). |
| `extent_x`, `extent_z` | Its size in meters, as the platform estimates it (ARKit: the reference size times `estimatedScaleFactor`). |
| `method` | `ArImageFull` while it is followed, `ArImageLastKnown` when the pose is where it was last seen (out of view, or on iOS when other images take the `tracked_images` slots), `ArImageNotTracking`. |
| `tracking` | An `ArTracking`: normal while followed, limited at a last known pose. |

`ArOptions.tracked_images` (1 by default) is how many images ARKit follows at the same time
(`maximumNumberOfTrackedImages`); the rest are found and then keep their last pose. Following
many costs ARKit time each frame. ARCore follows every image of its database; one instance
of each picture is reported on every platform.

## Platforms

- **iOS**: `ARWorldTrackingConfiguration.detectionImages` with
  `maximumNumberOfTrackedImages`; each target an `ARReferenceImage` made from a gray
  `CGImage` (or the file through `UIImage`), named by its index.
- **Android**: an `ArAugmentedImageDatabase` built when the session is configured, each
  image added with `ArAugmentedImageDatabase_addImageWithPhysicalSize` from tight 8-bit gray
  pixels (a file is decoded by `BitmapFactory` in `EskiuAr.decodeGray`); found images are the
  `AR_TRACKABLE_AUGMENTED_IMAGE` trackables with their tracking method (full or last known).
  ARCore spends 20 to 30 ms on each image when the database is built.
- **Web**: WebXR's `image-tracking` feature with `trackedImages` (an `ImageBitmap` and its
  width each), read with `XRFrame.getImageTrackingResults()`. Chrome has it behind the
  WebXR Incubations flag; without it the session runs and finds no images
  ([web](web.md)).

## Tests

`tests/session.esk` checks, with stand-ins for the native side, that the targets reach it
before the session starts (RGBA turned gray, an absolute path as it is, a target with no
physical width refused), that the same targets are not sent again and new ones restart the
session, the states, and that a found image arrives with its name, method, pose and size.
What needs a device: detection and tracking themselves on each platform, the pose's axes
on a real picture, and ARKit's validation of a weak picture.
