package app.aemu.stub;

import android.content.ClipData;
import android.os.IBinder;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.InputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * Puts a text file's content on the guest's clipboard, the way scrcpy does: straight into the clipboard service,
 * as the shell. The host's "paste" then only has to press the paste key, instead of typing the text key by key
 * (slow, ASCII only, and it lands wherever the focus has moved to meanwhile).
 *
 * app_process /system/bin app.aemu.stub.Run app.aemu.stub.Clip /sdcard/.aemu-paste.txt
 */
public final class Clip {
    public static void main(String[] args) throws Throwable {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        try (InputStream in = new FileInputStream(args[0])) {
            byte[] b = new byte[8192];
            for (int n; (n = in.read(b)) > 0; ) buf.write(b, 0, n);
        }
        ClipData clip = ClipData.newPlainText("AEmulator", new String(buf.toByteArray(), "UTF-8"));
        IBinder svc = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "clipboard");
        Object cb = Class.forName("android.content.IClipboard$Stub").getMethod("asInterface", IBinder.class).invoke(null, svc);
        Method set = null;
        for (Method m : cb.getClass().getMethods()) if (m.getName().equals("setPrimaryClip")) set = m;
        if (set == null) throw new IllegalStateException("no setPrimaryClip");
        Throwable last = null;
        // (the service checks that the package belongs to the calling user id: the shell's, or the system's)
        for (String pkg : new String[] { "com.android.shell", "android" }) {
            try {
                set.invoke(cb, params(set.getParameterTypes(), clip, pkg));
                return;
            } catch (InvocationTargetException e) {
                last = e.getCause() != null ? e.getCause() : e;
            }
        }
        throw last;
    }

    /** The signature changed with the releases: (clip, package), + user id (7), + attribution tag (12), + device id (14). */
    private static Object[] params(Class<?>[] types, ClipData clip, String pkg) {
        Object[] a = new Object[types.length];
        int strings = 0;
        for (int i = 0; i < types.length; i++) {
            if (types[i] == ClipData.class) a[i] = clip;
            else if (types[i] == String.class) a[i] = strings++ == 0 ? pkg : null;
            else if (types[i] == int.class) a[i] = 0;
        }
        return a;
    }
}
