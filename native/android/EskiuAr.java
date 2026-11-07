// <ar> on Android: the Java side of ar.c. start (called from ar_native_session) asks for the
// camera permission and hands the activity to nativeStart, which installs ARCore if needed
// and runs the session; the activity's pause and resume pause and resume it (the camera is
// released in the background), and the display's rotation and size go to nativeDisplay.
// decodeGray reads an image target's file for ARCore's image database.
package org.eskiu.ar;

import android.Manifest;
import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.view.View;
import org.eskiu.ui.UiActivity;

public final class EskiuAr {
    static native void nativeStart(Activity activity, int sensorOrientation);
    static native void nativePause();
    static native void nativeResume(Activity activity);
    static native void nativeDisplay(int rotation, int width, int height);

    private static boolean watching;
    private static int lastRotation = -1;

    public static void setup() {}

    // Called from ar_native_session on the UI thread.
    public static void start() {
        Activity activity = UiActivity.current();
        if (activity == null) return;
        watch(activity);
        UiActivity.requestPermissions(new String[] {Manifest.permission.CAMERA}, granted -> {
            if (!granted) { UiActivity.emit("ar", "denied"); return; }
            display(activity);
            nativeStart(activity, sensorOrientation(activity));
        });
    }

    // The PNG or JPEG file at `path` as 8-bit gray (BT.601 luma, as Eskiu turns RGBA pixels
    // gray): its width and height as 4 bytes each, big-endian, then the rows. Null when the
    // file does not decode. Called from ar.c while it builds the image database.
    static byte[] decodeGray(String path) {
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap bitmap = BitmapFactory.decodeFile(path, options);
        if (bitmap == null) return null;
        int width = bitmap.getWidth();
        int height = bitmap.getHeight();
        if ((long) width * height > 8192L * 8192L) { bitmap.recycle(); return null; }
        int[] row = new int[width];
        byte[] out = new byte[8 + width * height];
        out[0] = (byte) (width >>> 24); out[1] = (byte) (width >>> 16); out[2] = (byte) (width >>> 8); out[3] = (byte) width;
        out[4] = (byte) (height >>> 24); out[5] = (byte) (height >>> 16); out[6] = (byte) (height >>> 8); out[7] = (byte) height;
        for (int y = 0; y < height; y++) {
            bitmap.getPixels(row, 0, width, 0, y, width, 1);
            for (int x = 0; x < width; x++) {
                int c = row[x];
                int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                out[8 + y * width + x] = (byte) ((77 * r + 150 * g + 29 * b + 128) >> 8);
            }
        }
        bitmap.recycle();
        return out;
    }

    // The back camera's sensor orientation: how far its picture turns clockwise to be upright
    // with the device in its natural orientation (90 on almost every phone).
    static int sensorOrientation(Context context) {
        try {
            CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics c = manager.getCameraCharacteristics(id);
                Integer facing = c.get(CameraCharacteristics.LENS_FACING);
                Integer orientation = c.get(CameraCharacteristics.SENSOR_ORIENTATION);
                if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK && orientation != null) return orientation;
            }
        } catch (Exception error) {
            // Fall through to the usual value.
        }
        return 90;
    }

    // The display's rotation (Surface.ROTATION_*) and size, for ARCore's display geometry and
    // the picture's rotation.
    static void display(Activity activity) {
        int rotation = activity.getWindowManager().getDefaultDisplay().getRotation();
        DisplayMetrics metrics = new DisplayMetrics();
        activity.getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        lastRotation = rotation;
        nativeDisplay(rotation, metrics.widthPixels, metrics.heightPixels);
    }

    // Follows the activity's lifecycle, and the display's rotation through layout changes.
    static void watch(Activity activity) {
        if (watching) return;
        watching = true;
        View root = activity.getWindow().getDecorView();
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int rotation = activity.getWindowManager().getDefaultDisplay().getRotation();
            if (rotation != lastRotation || r - l != or - ol || b - t != ob - ot) display(activity);
        });
        activity.getApplication().registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityResumed(Activity a) { if (a == activity) { display(a); nativeResume(a); } }
            @Override public void onActivityPaused(Activity a) { if (a == activity) nativePause(); }
            @Override public void onActivityCreated(Activity a, Bundle state) {}
            @Override public void onActivityStarted(Activity a) {}
            @Override public void onActivityStopped(Activity a) {}
            @Override public void onActivitySaveInstanceState(Activity a, Bundle state) {}
            @Override public void onActivityDestroyed(Activity a) {}
        });
    }
}
