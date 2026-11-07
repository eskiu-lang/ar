// <ar> on Android: the Java side of ar.c. start (called from ar_native_session) asks for the
// camera permission and hands the activity to nativeStart, which installs ARCore if needed
// and runs the session; the activity's pause and resume pause and resume it (the camera is
// released in the background), and the display's rotation and size go to nativeDisplay.
package org.eskiu.ar;

import android.Manifest;
import android.app.Activity;
import android.app.Application;
import android.content.Context;
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
