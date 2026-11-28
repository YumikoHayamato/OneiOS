// SPDX-License-Identifier: MIT
// The display helper's Java half: a root, framework-independent SurfaceControl layer on the built-in
// display (the pattern of the OneOS fork's OneOSBootStatus, which draws while system_server is down).
// Run by app_process through bin/oneios-display; only surfaceflinger has to be running.
// Every android.* class is reached by reflection, so this compiles with a JDK alone and a method a
// release renamed is a fallback here, not a build error. The socket, the mapped frame and the copy
// into the layer's buffers are native (src/jni.c).

import java.lang.reflect.Method;

public final class OneiOSDisplay {
    private static native long nOpen(String socket, int waitMs);
    private static native boolean nHello(long handle, int width, int height, int hz);
    private static native int[] nInfo(long handle);                    // width, height, stride, format
    private static native int[] nFit(int sw, int sh, int dw, int dh);  // x, y, w, h
    private static native int[] nBuffer(int frameFormat);              // buffer format, dataspace
    // ready: SESSION/ready with --session (the first post waits for it: src/xhd.h xhd_hold_first), null with --socket
    private static native int nRun(long handle, Object surface, int format, int dataspace, String ready);
    private static native boolean nChild(Object surfaceControl);   // ONEIOS_DISPLAY_PRESENT=ahb: the frames' own layer
    private static native void nClose(long handle);

    private static final int RGBX_8888 = 2, BGRA_8888 = 5, RGBA_FP16 = 22;
    private static final int FRAME_BGRX = 1, FRAME_RGBA16F = 2;        // the hello's formats (src/xhd.h)
    // --dataspace's names and values (ADataSpace; src/xhd.h). -1: the frame format's own (auto).
    private static final String[] DATASPACE_NAMES = {"auto", "unknown", "srgb", "p3", "scrgb", "scrgb-linear"};
    private static final int[] DATASPACES = {-1, 0, 142671872, 143261696, 411107328, 406913024};
    private static final int TOP = Integer.MAX_VALUE - 1;
    private static Class<?> SC, TX;

    private static void log(String line) { System.err.println("oneios-display: " + line); }

    public static void main(String[] args) {
        int code;
        try { code = run(args); } catch (Throwable failure) { failure.printStackTrace(System.err); code = 5; }
        System.err.flush();
        System.exit(code);
    }

    private static int usage() {
        log("usage: oneios-display (--session DIR | --socket PATH | --crosvm-display NAME --size WxH [--rate-min HZ --rate-hold-file PATH]) [--takeover] [--wait SECONDS] [--logical WxH]"
            + " [--hz N] [--format bgra|rgbx] [--dataspace auto|unknown|srgb|p3|scrgb|scrgb-linear] [--no-projection]");
        return 2;
    }

    /** The value of a --dataspace name; -2 for a name that is not one. */
    static int dataspace(String name) {
        for (int i = 0; i < DATASPACE_NAMES.length; i++) if (DATASPACE_NAMES[i].equals(name)) return DATASPACES[i];
        return -2;
    }

    /**
     * The layer's buffer format for a frame format: the frame's own layout (B,G,R,A for 8-bit frames, or R,G,B,X when
     * --format rgbx asks; RGBA_FP16 for half floats, which no option narrows: the copy does that only when the phone
     * gives no such buffer). 0 for a frame format this helper does not know.
     */
    static int bufferFormat(int frameFormat, int asked) {
        if (frameFormat == FRAME_RGBA16F) return RGBA_FP16;
        if (frameFormat == FRAME_BGRX) return asked == RGBX_8888 ? RGBX_8888 : BGRA_8888;
        return 0;
    }

    // ---- crosvm's own display (--crosvm-display NAME) ----------------------------------------------
    // Droid-VM's crosvm shows a VM's virtio-gpu scanout in an Android Surface it is given through the service it registers
    // as NAME (crosvm --android-display-service NAME: android.crosvm.ICrosvmAndroidDisplayService of Droid-VM's
    // Virtualization fork, libs/android_display_backend; setSurface(inout Surface, boolean forCursor) is its first
    // transaction, removeSurface(boolean forCursor) its third). This makes the layer and its buffer queue's Surface and
    // hands that over: crosvm then blits each frame the guest flushes into it on the GPU, and nothing is copied here. It ends
    // when crosvm's binder dies (the VM ended) or on SIGTERM; the layer goes with it (docs/native.md "The GPU in the
    // shared-RAM VM").
    private static final String CROSVM_DISPLAY = "android.crosvm.ICrosvmAndroidDisplayService";
    private static final int SET_SURFACE = 1, REMOVE_SURFACE = 3, GET_DISPLAY_CONFIG = 6;
    // The layer, its buffer queue and its Surface stay reachable for the process's life: crosvm draws through the queue,
    // which a collected BLASTBufferQueue would take away (its cleaner destroys the native queue).
    private static Object[] sCrosvmDisplay;

    // The panel's power (--screen-file, a takeover's: displayctl touch makes the file when power is clicked and removes it at
    // the next click): POWER_MODE_OFF (0) while it is there, POWER_MODE_NORMAL (2) otherwise, and normal again at the end.
    private static Object sPanelToken;
    private static volatile boolean sPanelOff;

    private static boolean setPanel(boolean on) {
        if (sPanelToken == null) return false;
        try {
            SC.getMethod("setDisplayPowerMode", Class.forName("android.os.IBinder"), int.class).invoke(null, sPanelToken, on ? 2 : 0);
            return true;
        } catch (Throwable refused) {
            log("the panel's power could not be set (" + refused + ")");
            return false;
        }
    }

    private static int crosvmDisplay(String name, int width, int height, int hz, int waitSeconds, boolean takeover, String screenFile,
                                     int rateMin, String holdFile) throws Exception {
        SC = Class.forName("android.view.SurfaceControl");
        TX = Class.forName("android.view.SurfaceControl$Transaction");
        Class<?> parcelClass = Class.forName("android.os.Parcel"), binderClass = Class.forName("android.os.IBinder");
        Method check = Class.forName("android.os.ServiceManager").getMethod("checkService", String.class);
        Object binder = null;
        for (int tries = 0; tries < waitSeconds * 10 && binder == null; tries++) {
            binder = check.invoke(null, name);
            if (binder == null) Thread.sleep(100);
        }
        if (binder == null) { log("crosvm registered no " + name + " in " + waitSeconds + " s: no screen"); return 3; }
        log("crosvm's display service " + name + " is there");
        Object transaction = TX.getConstructor().newInstance();
        // --takeover (One UI's framework down, vm-boot --takeover): the panel in its native mode and the projection set here, as the
        // hosted session's takeover does; beside One UI the display is the framework's
        // before panel() can hold the rate: whatever ends this after that, One UI's specs come back
        Runtime.getRuntime().addShutdownHook(new Thread(OneiOSDisplay::endSpecs));
        sHoldRate = true;      // crosvm presents; nothing here sees touches or frames to raise the rate again
        int[] panel = panel(transaction, takeover, takeover, null);
        if (hz <= 0) hz = panel[2];
        // the frame fitted to the panel, centred (the guest's frame is normally the panel's own size)
        float scale = Math.min(panel[0] / (float) width, panel[1] / (float) height);
        int shownW = Math.round(width * scale), shownH = Math.round(height * scale);
        int x = (panel[0] - shownW) / 2, y = (panel[1] - shownH) / 2;
        Object layer = layer("OneiOS display (crosvm)");
        Object queue = queue("OneiOS display (crosvm)", layer, width, height, RGBX_8888);
        Object surface = queue.getClass().getMethod("createSurface").invoke(queue);
        sCrosvmDisplay = new Object[]{layer, queue, surface};
        final Object service = binder, s = surface, l = layer;
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            if (sPanelOff) setPanel(true);
            endSpecs();
            try { transact(service, REMOVE_SURFACE, null); } catch (Throwable ignored) { }
            try {
                Object t = TX.getConstructor().newInstance();
                if (!tx(t, "remove", new Class<?>[]{SC}, l)) tx(t, "hide", new Class<?>[]{SC}, l);
                tx(t, "apply", new Class<?>[]{});
            } catch (Throwable ignored) { }
            quiet(s, "release");
            System.err.println("oneios-display: crosvm's layer removed");
        }));
        Class<?>[] f4 = {SC, float.class, float.class, float.class, float.class};
        if (shownW != width || shownH != height) {
            if (!tx(transaction, "setMatrix", f4, layer, scale, 0f, 0f, scale)
                && !tx(transaction, "setScale", new Class<?>[]{SC, float.class, float.class}, layer, scale, scale))
                log("the layer could not be scaled: the frame shows at its own size");
        }
        tx(transaction, "setPosition", new Class<?>[]{SC, float.class, float.class}, layer, (float) x, (float) y);
        tx(transaction, "setOpaque", new Class<?>[]{SC, boolean.class}, layer, true);
        if (!tx(transaction, "setFrameRate", new Class<?>[]{SC, float.class, int.class}, layer, (float) hz, 0))
            log("no frame rate vote: surfaceflinger keeps its own refresh rate");
        place(transaction, layer, TOP);
        tx(transaction, "apply", new Class<?>[]{});
        log("layer up: the guest's " + width + "x" + height + " at " + x + "," + y + " " + shownW + "x" + shownH + " on the panel's "
            + panel[0] + "x" + panel[1] + ", " + hz + " Hz");
        // setSurface(inout Surface surface, boolean forCursor = false)
        Object data = parcelClass.getMethod("obtain").invoke(null);
        Object reply = parcelClass.getMethod("obtain").invoke(null);
        parcelClass.getMethod("writeInterfaceToken", String.class).invoke(data, CROSVM_DISPLAY);
        parcelClass.getMethod("writeInt", int.class).invoke(data, 1);
        surface.getClass().getMethod("writeToParcel", parcelClass, int.class).invoke(surface, data, 0);
        parcelClass.getMethod("writeInt", int.class).invoke(data, 0);
        boolean sent;
        try {
            sent = (Boolean) binderClass.getMethod("transact", int.class, parcelClass, parcelClass, int.class)
                .invoke(binder, SET_SURFACE, data, reply, 0);
            if (sent) parcelClass.getMethod("readException").invoke(reply);
        } catch (java.lang.reflect.InvocationTargetException refused) {
            // The surface is an inout argument: crosvm keeps the window it was given (setNativeSurface), so its stub then
            // has no window to write back and the reply fails with BAD_VALUE, which Java throws as IllegalArgumentException
            // (AOSP's Terminal app sees the same and goes on). The call itself was made: the window is crosvm's.
            if (!(refused.getCause() instanceof IllegalArgumentException)) throw refused;
            log("setSurface's reply could not carry the Surface back (crosvm keeps it): taken");
            sent = true;
        }
        parcelClass.getMethod("recycle").invoke(data);
        parcelClass.getMethod("recycle").invoke(reply);
        if (!sent) { log("crosvm did not take the Surface (setSurface's transaction failed)"); return 4; }
        int[] scanout = displayConfig(binder);
        log("the Surface is crosvm's: it draws the VM's screen there (its scanout " + scanout[0] + "x" + scanout[1] + ")");
        Method alive = binderClass.getMethod("isBinderAlive");
        if (screenFile != null) {
            try { sPanelToken = display()[1]; } catch (Throwable unavailable) { log("no display token: the panel stays on (" + unavailable + ")"); }
        }
        java.io.File switchFile = screenFile != null ? new java.io.File(screenFile) : null;
        // take19, --rate-min N (vm-boot --panel-rate-min): the panel's rate free between N and the native mode's (surfaceflinger
        // lowers it when crosvm posts nothing and raises it for the layer's 120 Hz vote when frames come), and held at the top
        // while the hold file is there (displayctl touch's boost.state: a touch in the last 1.5 s), because with One UI's
        // framework down nothing tells surfaceflinger of a touch. Looked at every 10 ms.
        java.io.File hold = rateMin > 0 && holdFile != null ? new java.io.File(holdFile) : null;
        boolean held = true;
        long holds = 0, heldNs = 0, heldSince = System.nanoTime(), freeSince = 0;
        if (hold != null) {
            if (sModeToken != null && nativeModeMin(sModeToken, sModeId, sModeHz, rateMin, false)) {
                held = false;
                freeSince = System.nanoTime();
                heldNs += freeSince - heldSince;
                log("the panel's rate is free from " + rateMin + " to " + sModeHz + " Hz, held at " + sModeHz + " while " + holdFile + " is there");
            } else {
                log("the panel's rate could not be freed: it stays held at its top");
                hold = null;
            }
        }
        // take18 (YMGPwcca's bounded lock-screen wake): One UI's keyguard may have darkened the panel before its framework
        // stopped, and nothing else wakes it now; with a screen file (a takeover's) the panel is the helper's, so it is put
        // on once at the start, unless power was clicked already (the file is there).
        if (switchFile != null && !switchFile.exists() && setPanel(true)) log("the panel is on (the takeover's start)");
        // The panel's own rate, looked at every 5 s (one call to surfaceflinger): a line when it changes, and the session's
        // share of each rate at the end. The guest paces its frames by --hz whatever the panel does.
        Object[] watched = null;
        try { watched = display(); } catch (Throwable unavailable) { log("the panel's rate is not watched (" + unavailable + ")"); }
        java.util.TreeMap<Integer, Integer> rateSeen = new java.util.TreeMap<>();
        int rateNow = -1, rateLines = 0;
        long rateAt = 0;
        while ((Boolean) alive.invoke(binder)) {
            if (watched != null && !sPanelOff && System.nanoTime() - rateAt >= 5_000_000_000L) {
                rateAt = System.nanoTime();
                try {
                    int[] m = mode((Long) watched[0], watched[1]);
                    rateSeen.merge(m[7], 1, Integer::sum);
                    if (m[7] != rateNow) {
                        rateNow = m[7];
                        if (rateLines++ < 200) log("the panel runs at " + m[7] + " Hz (mode " + m[8] + ", " + m[0] + "x" + m[1] + "); the guest draws for " + hz + " Hz"
                            + (m[7] < hz ? ": at most " + m[7] + " of its frames a second are shown" : ""));
                    }
                } catch (Throwable unreadable) { /* the next look */ }
            }
            if (switchFile != null) {
                boolean off = switchFile.exists();
                if (off != sPanelOff && setPanel(!off)) {
                    sPanelOff = off;
                    log(off ? "the panel is off (power clicked)" : "the panel is on");
                }
            }
            if (hold != null) {
                boolean want = hold.exists();
                if (want != held && nativeModeMin(sModeToken, sModeId, sModeHz, want ? sModeHz : rateMin, false)) {
                    long now = System.nanoTime();
                    if (want) { holds++; heldSince = now; if (holds <= 3) log("touch: the panel held at " + sModeHz + " Hz (hold " + holds + ")"); }
                    else heldNs += now - heldSince;
                    held = want;
                }
            }
            Thread.sleep(hold != null ? 10 : switchFile != null ? 50 : 500);
        }
        if (hold != null) {
            long now = System.nanoTime(), all = now - freeSince;
            if (held) heldNs += now - heldSince;
            log("the panel's rate was held at " + sModeHz + " Hz " + holds + " times, " + (all > 0 ? Math.round(heldNs * 100.0 / all) : 0)
                + " % of the session; free from " + rateMin + " Hz the rest");
        }
        if (sPanelOff && setPanel(true)) { sPanelOff = false; log("the panel is on again"); }
        log("the panel's rate over the session: " + rateShares(rateSeen));
        log("crosvm's display service is gone: the VM ended");
        return 0;
    }

    /** "120 Hz 98 %, 60 Hz 2 %" from the looks at the panel's rate (rate -> looks), highest rate first. */
    static String rateShares(java.util.SortedMap<Integer, Integer> seen) {
        long all = 0;
        for (int n : seen.values()) all += n;
        if (all == 0) return "not read";
        StringBuilder out = new StringBuilder();
        java.util.ArrayList<Integer> rates = new java.util.ArrayList<>(seen.keySet());
        for (int i = rates.size() - 1; i >= 0; i--)
            out.append(out.length() > 0 ? ", " : "").append(rates.get(i)).append(" Hz ").append(Math.round(seen.get(rates.get(i)) * 100.0 / all)).append(" %");
        return out.append(" (").append(all).append(" looks, 5 s apart)").toString();
    }

    /** getDisplayConfig (the service's sixth transaction, of Droid-VM's fork): the guest's scanout width and height. */
    private static int[] displayConfig(Object binder) {
        try {
            Class<?> parcelClass = Class.forName("android.os.Parcel");
            Object data = parcelClass.getMethod("obtain").invoke(null), reply = parcelClass.getMethod("obtain").invoke(null);
            parcelClass.getMethod("writeInterfaceToken", String.class).invoke(data, CROSVM_DISPLAY);
            Class.forName("android.os.IBinder").getMethod("transact", int.class, parcelClass, parcelClass, int.class)
                .invoke(binder, GET_DISPLAY_CONFIG, data, reply, 0);
            parcelClass.getMethod("readException").invoke(reply);
            Method readInt = parcelClass.getMethod("readInt");
            int[] out = {0, 0};
            if ((Integer) readInt.invoke(reply) != 0) {          // non-null, then the parcelable's size, then its fields
                readInt.invoke(reply);
                out[0] = (Integer) readInt.invoke(reply);
                out[1] = (Integer) readInt.invoke(reply);
            }
            parcelClass.getMethod("recycle").invoke(data);
            parcelClass.getMethod("recycle").invoke(reply);
            return out;
        } catch (Throwable unknown) {
            return new int[]{-1, -1};
        }
    }

    /** One of the service's transactions that takes only forCursor = false (removeSurface). */
    private static void transact(Object binder, int code, Object unused) throws Exception {
        Class<?> parcelClass = Class.forName("android.os.Parcel");
        Object data = parcelClass.getMethod("obtain").invoke(null), reply = parcelClass.getMethod("obtain").invoke(null);
        parcelClass.getMethod("writeInterfaceToken", String.class).invoke(data, CROSVM_DISPLAY);
        parcelClass.getMethod("writeInt", int.class).invoke(data, 0);
        Class.forName("android.os.IBinder").getMethod("transact", int.class, parcelClass, parcelClass, int.class).invoke(binder, code, data, reply, 0);
        parcelClass.getMethod("recycle").invoke(data);
        parcelClass.getMethod("recycle").invoke(reply);
    }

    /**
     * --specs-probe: the takeover's mode request alone, for a few seconds, beside One UI or without it: the panel's
     * mode and surfaceflinger's policy, the native mode held at its rate, both again, and One UI's specs given back.
     */
    private static int specsProbe() throws Exception {
        SC = Class.forName("android.view.SurfaceControl");
        Object[] display = display();
        Object token = display[1];
        int[] mode = mode((Long) display[0], token);
        log("probe: active " + mode[0] + "x" + mode[1] + " at " + mode[7] + " Hz (mode " + mode[8] + "), native mode " + mode[6] + " at "
            + mode[5] + " Hz; policy: " + policyText(policy(token)));
        Runtime.getRuntime().addShutdownHook(new Thread(OneiOSDisplay::endSpecs));
        boolean asked;
        try {
            asked = nativeMode(token, mode[6], mode[5], true);
            for (int i = 0; i < 6; i++) {
                Thread.sleep(500);
                mode = mode((Long) display[0], token);
                log("probe: +" + (i + 1) * 500 + " ms: " + mode[7] + " Hz (mode " + mode[8] + "); policy: " + policyText(policy(token)));
            }
        } finally {
            restoreSpecs();
        }
        Thread.sleep(1000);
        mode = mode((Long) display[0], token);
        log("probe: after the way back: " + mode[7] + " Hz (mode " + mode[8] + "); policy: " + policyText(policy(token)));
        return asked ? 0 : 1;
    }

    private static int run(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("--specs-probe")) return specsProbe();
        // --crosvm-display NAME --size WxH [--hz N] [--wait SECONDS] [--takeover] [--screen-file PATH]: crosvm's own display
        // (above); no socket, no frame copy
        for (int i = 0; i + 1 < args.length; i++) {
            if (!args[i].equals("--crosvm-display")) continue;
            String name = args[i + 1];
            int width = 0, height = 0, hz = 0, waitSeconds = 60, rateMin = 0;
            String screenFile = null, holdFile = null;
            boolean takeover = false;
            for (String a : args) if (a.equals("--takeover")) takeover = true;
            for (int j = 0; j + 1 < args.length; j++) {
                if (args[j].equals("--size")) { String[] p = args[j + 1].split("x"); width = Integer.parseInt(p[0]); height = Integer.parseInt(p[1]); }
                else if (args[j].equals("--hz")) hz = Integer.parseInt(args[j + 1]);
                else if (args[j].equals("--wait")) waitSeconds = Integer.parseInt(args[j + 1]);
                else if (args[j].equals("--screen-file")) screenFile = args[j + 1];
                else if (args[j].equals("--rate-min")) rateMin = Integer.parseInt(args[j + 1]);
                else if (args[j].equals("--rate-hold-file")) holdFile = args[j + 1];
            }
            if (width <= 0 || height <= 0) return usage();
            // take19: a variable panel rate needs a takeover (the panel is the helper's) and the file that says a touch is recent
            if (rateMin > 0 && (!takeover || holdFile == null)) { log("--rate-min needs --takeover and --rate-hold-file: the rate stays held"); rateMin = 0; }
            return crosvmDisplay(name, width, height, hz, waitSeconds, takeover, screenFile, rateMin, holdFile);
        }
        String socket = null, lib = null, ready = null;
        int wait = 30, hz = 0, format = BGRA_8888, dataspace = -1;
        boolean takeover = false, projection = true;
        int[] logical = null;
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            if (a.equals("--takeover")) { takeover = true; continue; }
            if (a.equals("--no-projection")) { projection = false; continue; }
            if (i + 1 >= args.length) return usage();
            String v = args[++i];
            if (a.equals("--session")) { socket = v + "/display.sock"; ready = v + "/ready"; }
            else if (a.equals("--socket")) { socket = v; ready = null; }
            else if (a.equals("--lib")) lib = v;
            else if (a.equals("--wait")) wait = Integer.parseInt(v);
            else if (a.equals("--hz")) hz = Integer.parseInt(v);
            else if (a.equals("--logical")) { String[] p = v.split("x"); logical = new int[]{Integer.parseInt(p[0]), Integer.parseInt(p[1])}; }
            else if (a.equals("--format")) format = v.equals("rgbx") ? RGBX_8888 : BGRA_8888;
            else if (a.equals("--dataspace")) { dataspace = dataspace(v); if (dataspace == -2) return usage(); }
            else return usage();
        }
        if (socket == null || lib == null) return usage();
        System.load(lib);
        SC = Class.forName("android.view.SurfaceControl");
        TX = Class.forName("android.view.SurfaceControl$Transaction");

        // The socket first: without xnuhost there is nothing to show and no layer is made.
        long handle = nOpen(socket, wait * 1000);
        if (handle == 0) return 3;

        // Then the panel, then the frame: in a session xnuhost sizes the guest's frame from this report.
        Object transaction = TX.getConstructor().newInstance();
        int[] panel = panel(transaction, takeover && projection, takeover, logical);
        log("reporting the panel to xnuhost: " + panel[0] + "x" + panel[1] + " at " + panel[2] + " Hz");
        if (!nHello(handle, panel[0], panel[1], panel[2])) { nClose(handle); return 3; }
        int[] guest = nInfo(handle);
        int[] own = nBuffer(guest[3]);
        format = bufferFormat(guest[3], format);
        if (dataspace == -1) dataspace = own[1];
        log("guest frame " + guest[0] + "x" + guest[1] + ", stride " + guest[2] + ", format " + guest[3]
            + (guest[3] == FRAME_RGBA16F ? " (R,G,B,A half float, linear extended sRGB)" : " (B,G,R,X 8-bit sRGB)")
            + "; layer buffers in format " + format + ", dataspace " + Integer.toHexString(dataspace));
        if (hz <= 0) hz = panel[2];
        int[] fit = nFit(guest[0], guest[1], panel[0], panel[1]);
        log("panel " + panel[0] + "x" + panel[1] + " at " + hz + " Hz; the frame at " + fit[0] + "," + fit[1] + " " + fit[2] + "x" + fit[3]);

        Object back = null, backQueue = null, backSurface = null, layer = null, queue = null, surface = null, child = null;
        int result;
        try {
            // Black behind the frame where its aspect leaves bars (and under its first buffer).
            if (fit[2] != panel[0] || fit[3] != panel[1]) {
                back = layer("OneiOS display background");
                backQueue = queue("OneiOS display background", back, panel[0], panel[1], RGBX_8888);
                backSurface = backQueue.getClass().getMethod("createSurface").invoke(backQueue);
                place(transaction, back, TOP - 1);
                tx(transaction, "apply", new Class<?>[]{});
                Class<?> rect = Class.forName("android.graphics.Rect");
                Object canvas = backSurface.getClass().getMethod("lockCanvas", rect).invoke(backSurface, (Object) null);
                canvas.getClass().getMethod("drawColor", int.class).invoke(canvas, 0xff000000);
                backSurface.getClass().getMethod("unlockCanvasAndPost", Class.forName("android.graphics.Canvas")).invoke(backSurface, canvas);
            }
            // The frame's layer: buffers of the guest's size. The guest renders the panel's native mode,
            // so in a session the layer covers the panel pixel for pixel, with no matrix and no bars;
            // a frame of another size (a mismatch) is scaled to the fit by the compositor (one sampling
            // on the GPU or in an overlay; this process only copies rows).
            layer = layer("OneiOS display");
            queue = queue("OneiOS display", layer, guest[0], guest[1], format);
            surface = queue.getClass().getMethod("createSurface").invoke(queue);
            boolean exact = fit[2] == guest[0] && fit[3] == guest[1];
            float sx = fit[2] / (float) guest[0], sy = fit[3] / (float) guest[1];
            Class<?>[] f4 = {SC, float.class, float.class, float.class, float.class};
            if (exact) log("the frame is the panel's size: shown 1:1, not scaled");
            else if (!tx(transaction, "setMatrix", f4, layer, sx, 0f, 0f, sy)
                && !tx(transaction, "setScale", new Class<?>[]{SC, float.class, float.class}, layer, sx, sy))
                log("the layer could not be scaled: the frame shows at its own size");
            if (!tx(transaction, "setPosition", new Class<?>[]{SC, float.class, float.class}, layer, (float) fit[0], (float) fit[1]))
                log("the layer could not be placed");
            tx(transaction, "setOpaque", new Class<?>[]{SC, boolean.class}, layer, true);   // the guest's X byte is not alpha
            if (!tx(transaction, "setFrameRate", new Class<?>[]{SC, float.class, int.class}, layer, (float) hz, 0))
                log("no frame rate vote: surfaceflinger keeps its own refresh rate");
            place(transaction, layer, TOP);
            tx(transaction, "apply", new Class<?>[]{});
            log("layer up");
            // ONEIOS_DISPLAY_PRESENT=ahb (displayctl pool): the frames go to a child of the layer, in buffers of the native
            // side's own that the CPU only writes (jni.c, "the present path without the CPU caches' maintenance"); the
            // layer's matrix and position place the child too. Without it, or if the child cannot be had, the window path.
            if ("ahb".equals(System.getenv("ONEIOS_DISPLAY_PRESENT"))) {
                try {
                    child = layerUnder("OneiOS frame", layer);
                    if (!nChild(child)) log("the frames' child layer was not taken: the window path");
                } catch (Exception | LinkageError e) {
                    log("no child layer for the frames (" + e + "): the window path");
                }
            }
            result = nRun(handle, surface, format, dataspace, ready);
        } finally {
            for (Object gone : new Object[]{child, layer, back}) {
                if (gone == null) continue;
                if (!tx(transaction, "remove", new Class<?>[]{SC}, gone)) tx(transaction, "hide", new Class<?>[]{SC}, gone);
            }
            tx(transaction, "apply", new Class<?>[]{});
            for (Object s : new Object[]{surface, backSurface}) if (s != null) quiet(s, "release");
            for (Object q : new Object[]{queue, backQueue}) if (q != null) quiet(q, "destroy");
            nClose(handle);
            restoreSpecs();
        }
        log("layer removed");
        return result < 0 ? 4 : 0;
    }

    // ---- layers -------------------------------------------------------------------------------

    private static Object layer(String name) throws Exception {
        Object builder = Class.forName("android.view.SurfaceControl$Builder").getConstructor().newInstance();
        builder.getClass().getMethod("setName", String.class).invoke(builder, name);
        builder.getClass().getMethod("setBLASTLayer").invoke(builder);
        return builder.getClass().getMethod("build").invoke(builder);
    }

    private static Object layerUnder(String name, Object parent) throws Exception {
        Object builder = Class.forName("android.view.SurfaceControl$Builder").getConstructor().newInstance();
        builder.getClass().getMethod("setName", String.class).invoke(builder, name);
        builder.getClass().getMethod("setBLASTLayer").invoke(builder);
        builder.getClass().getMethod("setParent", SC).invoke(builder, parent);
        return builder.getClass().getMethod("build").invoke(builder);
    }

    private static Object queue(String name, Object layer, int width, int height, int format) throws Exception {
        Class<?> blast = Class.forName("android.graphics.BLASTBufferQueue");
        try {
            return blast.getConstructor(String.class, SC, int.class, int.class, int.class).newInstance(name, layer, width, height, format);
        } catch (NoSuchMethodException changed) {
            Object queue = blast.getConstructor(String.class, boolean.class).newInstance(name, false);
            blast.getMethod("update", SC, int.class, int.class, int.class).invoke(queue, layer, width, height, format);
            return queue;
        }
    }

    /** Top of the built-in display's stack, and visible. */
    private static void place(Object transaction, Object layer, int z) throws Exception {
        if (!tx(transaction, "setLayer", new Class<?>[]{SC, int.class}, layer, z)) throw new IllegalStateException("setLayer is gone");
        if (!tx(transaction, "setLayerStack", new Class<?>[]{SC, int.class}, layer, 0)) log("no setLayerStack: the layer stays on the default stack");
        if (!tx(transaction, "show", new Class<?>[]{SC}, layer)
            && !tx(transaction, "setVisibility", new Class<?>[]{SC, boolean.class}, layer, true))
            throw new IllegalStateException("the layer cannot be shown");
    }

    private static boolean tx(Object transaction, String name, Class<?>[] types, Object... args) {
        try { TX.getMethod(name, types).invoke(transaction, args); return true; }
        catch (Throwable missing) { return false; }
    }

    private static void quiet(Object target, String method) {
        try { target.getClass().getMethod(method).invoke(target); } catch (Throwable ignored) { }
    }

    // ---- the panel ----------------------------------------------------------------------------

    /**
     * Width and height of the space layers are placed in, and the refresh rate to ask for.
     * In a takeover session (system_server gone, surfaceflinger restarted at the return) the panel is put in
     * its native mode, the largest it has, at that mode's highest refresh rate (One UI's resolution setting may
     * have left it at 1080x2340), and the display's projection is set to the mode, unrotated: the layer space
     * is then the panel's own pixels. Otherwise nothing of the display is changed: the logical size comes
     * from the display manager (One UI running), --logical, or the active mode.
     */
    private static int[] panel(Object transaction, boolean project, boolean takeover, int[] logical) {
        Object token = null;
        int[] mode = null;                 // active width, height, hz; native width, height, hz, mode id
        try {
            Object[] display = display();
            token = display[1];
            mode = mode((Long) display[0], token);
            float[] policy = policy(token);
            log("panel modes: active " + mode[0] + "x" + mode[1] + " at " + mode[7] + " Hz (mode " + mode[8] + "; up to " + mode[2]
                + " Hz at that size), native " + mode[3] + "x" + mode[4] + " at " + mode[5] + " Hz; surfaceflinger's policy: " + policyText(policy));
            if (takeover) { sModeToken = token; sModeId = mode[6]; sModeHz = mode[5]; }   // take19: --rate-min's mode
            if (takeover && wantsSpecs(mode, policy, sHoldRate)) {
                if (nativeMode(token, mode[6], mode[5], sHoldRate)) {
                    // the switch happens at a vsync of surfaceflinger's: wait for it to be the active mode
                    for (int tries = 0; tries < 20; tries++) {
                        Thread.sleep(50);
                        mode = mode((Long) display[0], token);
                        if (mode[0] == mode[3] && mode[1] == mode[4] && (!sHoldRate || mode[7] >= mode[5])) break;
                    }
                }
                if (mode[0] == mode[3] && mode[1] == mode[4]) log("the panel is in its native mode " + mode[0] + "x" + mode[1] + " at " + mode[7]
                    + " Hz (mode " + mode[8] + "); surfaceflinger's policy: " + policyText(policy(token)));
                else log("the panel stays at " + mode[0] + "x" + mode[1] + ": the guest's frame is scaled to it");
                if (sHoldRate && mode[7] < mode[5]) log("the panel shows " + mode[7] + " frames a second, not the " + mode[5]
                    + " the guest is told: surfaceflinger did not take the rate");
            }
        } catch (Throwable unavailable) {
            log("the display's modes are not readable (" + unavailable + ")");
        }
        int hz = mode != null && mode[2] > 0 ? mode[2] : 120;
        if (project && token != null && mode != null) {
            try {
                Class<?> rect = Class.forName("android.graphics.Rect");
                Object full = rect.getConstructor(int.class, int.class, int.class, int.class).newInstance(0, 0, mode[0], mode[1]);
                TX.getMethod("setDisplayProjection", Class.forName("android.os.IBinder"), int.class, rect, rect)
                    .invoke(transaction, token, 0, full, full);
                TX.getMethod("apply").invoke(transaction);
                log("display projection set to the active mode " + mode[0] + "x" + mode[1]);
                return new int[]{mode[0], mode[1], hz};
            } catch (Throwable refused) {
                log("the display projection could not be set (" + refused + ")");
            }
        }
        if (!takeover) {
            try {
                Class<?> dm = Class.forName("android.hardware.display.DisplayManagerGlobal");
                Object instance = dm.getMethod("getInstance").invoke(null);
                Object info = dm.getMethod("getDisplayInfo", int.class).invoke(instance, 0);
                return new int[]{info.getClass().getField("logicalWidth").getInt(info), info.getClass().getField("logicalHeight").getInt(info), hz};
            } catch (Throwable noFramework) { /* system_server is not there: the next source */ }
        }
        if (logical != null) return new int[]{logical[0], logical[1], hz};
        if (mode != null) return new int[]{mode[0], mode[1], hz};
        log("panel size unknown: assuming 1440x3120");
        return new int[]{1440, 3120, hz};
    }

    /**
     * Asks surfaceflinger for the panel's native mode: the display's desired mode specs as they are, with the
     * default mode replaced and the refresh ranges opened up to that mode's rate (the object's shape changed
     * between Android releases, so it is read and written back rather than built). system_server, which
     * otherwise owns these specs, is gone in a session; the return restarts surfaceflinger, which starts from
     * its own boot mode again and gets One UI's specs when the framework is back.
     */
    private static boolean nativeMode(Object token, int id, int hz, boolean hold) {
        sModeToken = token;
        sModeId = id;
        sModeHz = hz;
        return nativeModeMin(token, id, hz, hold ? hz : 0, true);
    }

    // take19: the mode last asked for, for --rate-min's later changes of the range's bottom
    private static Object sModeToken;
    private static int sModeId, sModeHz;

    /** nativeMode with the ranges' bottom at `min` (0: open, hz: held); `say`: the log line (not for --rate-min's holds). */
    private static synchronized boolean nativeModeMin(Object token, int id, int hz, int min, boolean say) {
        if (sEnding) return false;      // the way back has run (a shutdown hook): no new hold after it
        boolean hold = min >= hz;
        try {
            Class<?> binder = Class.forName("android.os.IBinder");
            Object specs = SC.getMethod("getDesiredDisplayModeSpecs", binder).invoke(null, token);
            if (specs == null) throw new IllegalStateException("no desired mode specs");
            // a second copy, untouched, for the way back (restoreSpecs): only the first set of a run is One UI's
            if (sSpecsBefore == null) {
                sSpecsBefore = SC.getMethod("getDesiredDisplayModeSpecs", binder).invoke(null, token);
                sSpecsToken = token;
            }
            specs.getClass().getField("defaultMode").setInt(specs, id);
            try { specs.getClass().getField("allowGroupSwitching").setBoolean(specs, true); } catch (NoSuchFieldException older) { /* Android 11 */ }
            for (String name : new String[]{"primaryRanges", "appRequestRanges"}) {
                try {
                    Object ranges = specs.getClass().getField(name).get(specs);
                    for (String part : new String[]{"physical", "render"}) {
                        Object range = ranges.getClass().getField(part).get(ranges);
                        range.getClass().getField("min").setFloat(range, (float) Math.min(min, hz));
                        range.getClass().getField("max").setFloat(range, (float) hz);
                    }
                } catch (NoSuchFieldException older) {
                    // Android 13 and earlier: four floats on the specs themselves
                    String prefix = name.equals("primaryRanges") ? "primaryRefreshRate" : "appRequestRefreshRate";
                    specs.getClass().getField(prefix + "Min").setFloat(specs, (float) Math.min(min, hz));
                    specs.getClass().getField(prefix + "Max").setFloat(specs, (float) hz);
                }
            }
            Object set = setSpecs(token, specs);
            if (set instanceof Boolean && !((Boolean) set)) throw new IllegalStateException("surfaceflinger refused the mode specs");
            if (say) log("asked surfaceflinger for the native mode (id " + id + (hold ? ") held at " : min > 0 ? ") from " + min + " Hz up to " : "), up to ") + hz + " Hz");
            return true;
        } catch (Throwable refused) {
            log("the native mode could not be asked for (" + refused + ")");
            return false;
        }
    }

    /**
     * SurfaceControl.setDesiredDisplayModeSpecs in either shape: (display token, specs) up to Android 16, and from
     * Android 17 (the S25's One UI 9, SDK 37; read from its framework.jar) (apply token, specs[]) with the display's
     * token in each specs' displayToken. The apply token only names the caller to surfaceflinger: a Binder of ours.
     */
    private static Object setSpecs(Object token, Object specs) throws Exception {
        Class<?> binder = Class.forName("android.os.IBinder");
        try {
            return SC.getMethod("setDesiredDisplayModeSpecs", binder, specs.getClass()).invoke(null, token, specs);
        } catch (NoSuchMethodException newer) {
            Object list = java.lang.reflect.Array.newInstance(specs.getClass(), 1);
            java.lang.reflect.Array.set(list, 0, specs);
            specs.getClass().getField("displayToken").set(specs, token);
            if (sApplyToken == null) sApplyToken = Class.forName("android.os.Binder").getConstructor().newInstance();
            return SC.getMethod("setDesiredDisplayModeSpecs", binder, list.getClass()).invoke(null, sApplyToken, list);
        }
    }

    private static Object sApplyToken;
    private static Object sSpecsBefore, sSpecsToken;
    // The rate held at the native mode's (the ranges' lowest raised to it), not only allowed: the crosvm mode's. The
    // session's helper (xnuhost's frames) opens the ranges from 0 as before, and its vote asks for the rate.
    private static boolean sHoldRate;

    // Set with the last restoreSpecs: the main thread goes on while the shutdown hooks run.
    private static boolean sEnding;

    /** restoreSpecs at the end of the process: nativeMode() holds nothing after it. */
    private static synchronized void endSpecs() {
        sEnding = true;
        restoreSpecs();
    }

    /** The desired mode specs as they were before nativeMode() changed them (One UI's last), given back to surfaceflinger. */
    private static synchronized void restoreSpecs() {
        Object specs = sSpecsBefore, token = sSpecsToken;
        if (specs == null) return;
        sSpecsBefore = null;
        try {
            if (Boolean.FALSE.equals(setSpecs(token, specs))) throw new IllegalStateException("surfaceflinger refused them");
            System.err.println("oneios-display: surfaceflinger's policy is One UI's again: " + policyText(policy(token)));
        } catch (Throwable refused) {
            System.err.println("oneios-display: the mode specs could not be given back (" + refused + "): One UI's framework sets its own at its start");
        }
    }

    /**
     * surfaceflinger's policy for the display, as the display manager last set it: {the default mode's id, the highest
     * refresh rate the panel may run at (the primary physical range's top), its lowest}; -1 each when not readable.
     */
    private static float[] policy(Object token) {
        try {
            Object specs = SC.getMethod("getDesiredDisplayModeSpecs", Class.forName("android.os.IBinder")).invoke(null, token);
            float id = specs.getClass().getField("defaultMode").getInt(specs), min, max;
            try {
                Object ranges = specs.getClass().getField("primaryRanges").get(specs);
                Object range = ranges.getClass().getField("physical").get(ranges);
                min = range.getClass().getField("min").getFloat(range);
                max = range.getClass().getField("max").getFloat(range);
            } catch (NoSuchFieldException older) {
                min = specs.getClass().getField("primaryRefreshRateMin").getFloat(specs);
                max = specs.getClass().getField("primaryRefreshRateMax").getFloat(specs);
            }
            return new float[]{id, max, min};
        } catch (Throwable unreadable) {
            return new float[]{-1, -1, -1};
        }
    }

    static String policyText(float[] policy) {
        if (policy[1] < 0) return "not readable";
        return "default mode " + Math.round(policy[0]) + ", the panel at " + Math.round(policy[2]) + " to " + Math.round(policy[1]) + " Hz";
    }

    /**
     * Whether a takeover sets the display's mode specs: the panel is not in its native size, or runs below the native
     * mode's rate, or surfaceflinger's policy holds it below that rate (One UI's last word before its framework stopped:
     * 60 Hz with power saving or the standard motion setting, where the layer's vote for 120 counts for nothing), or,
     * with hold, lets it fall below it (with the framework gone nothing tells surfaceflinger of a touch, and its idle
     * timer would lower the rate between gestures). mode is pick()'s, policy is policy()'s; one that could not be read
     * (-1) is taken as too low.
     */
    static boolean wantsSpecs(int[] mode, float[] policy, boolean hold) {
        if (mode[0] != mode[3] || mode[1] != mode[4]) return true;
        if (policy[1] < mode[5] || Math.round(policy[0]) != mode[6]) return true;
        return hold && (mode[7] < mode[5] || policy[2] < mode[5]);
    }

    /** The built-in display: {Long id, IBinder token}. */
    private static Object[] display() throws Exception {
        try {
            // Android 14 and later: the calls moved to DisplayControl in services.jar (scrcpy's way in).
            String jars = System.getenv("SYSTEMSERVERCLASSPATH");
            if (jars == null || jars.isEmpty()) jars = "/system/framework/services.jar";
            ClassLoader loader = (ClassLoader) Class.forName("dalvik.system.PathClassLoader")
                .getConstructor(String.class, ClassLoader.class).newInstance(jars, ClassLoader.getSystemClassLoader());
            Class<?> control = loader.loadClass("com.android.server.display.DisplayControl");
            Method load = Runtime.class.getDeclaredMethod("loadLibrary0", Class.class, String.class);
            load.setAccessible(true);
            load.invoke(Runtime.getRuntime(), control, "android_servers");
            long[] ids = (long[]) control.getMethod("getPhysicalDisplayIds").invoke(null);
            return new Object[]{ids[0], control.getMethod("getPhysicalDisplayToken", long.class).invoke(null, ids[0])};
        } catch (Throwable older) {
            long[] ids = (long[]) SC.getMethod("getPhysicalDisplayIds").invoke(null);
            return new Object[]{ids[0], SC.getMethod("getPhysicalDisplayToken", long.class).invoke(null, ids[0])};
        }
    }

    /**
     * {active width, height, the highest refresh rate among the modes of that size;
     *  native width, height, its highest refresh rate, the id of the native mode at that rate;
     *  the active mode's own refresh rate, its id}.
     * The native mode is the largest the panel has, not the one it is in.
     */
    private static int[] mode(long id, Object token) throws Exception {
        Object info;
        try { info = SC.getMethod("getDynamicDisplayInfo", long.class).invoke(null, id); }
        catch (NoSuchMethodException older) { info = SC.getMethod("getDynamicDisplayInfo", Class.forName("android.os.IBinder")).invoke(null, token); }
        if (!colourLogged) { colourLogged = true; colour(info); }
        int active = info.getClass().getField("activeDisplayModeId").getInt(info);
        Object[] modes = (Object[]) info.getClass().getField("supportedDisplayModes").get(info);
        int count = modes.length;
        int[] ids = new int[count], widths = new int[count], heights = new int[count], rates = new int[count];
        for (int i = 0; i < count; i++) {
            Object m = modes[i];
            ids[i] = m.getClass().getField("id").getInt(m);
            widths[i] = m.getClass().getField("width").getInt(m);
            heights[i] = m.getClass().getField("height").getInt(m);
            float rate;
            try { rate = m.getClass().getField("peakRefreshRate").getFloat(m); }
            catch (NoSuchFieldException older) { rate = m.getClass().getField("refreshRate").getFloat(m); }
            rates[i] = Math.round(rate);
        }
        int[] picked = pick(ids, widths, heights, rates, active);
        if (picked == null) throw new IllegalStateException("no active mode");
        return picked;
    }

    private static boolean colourLogged;

    /**
     * What the panel says of its colours, read only (nothing is set): the colour mode One UI left it in, the modes
     * it has, and its HDR capabilities (the luminance surfaceflinger plans with). A layer of a wide dataspace may
     * make surfaceflinger pick another colour mode while the layer is shown; it goes back to its own choice when
     * the layer is gone, and the return restarts surfaceflinger.
     */
    private static void colour(Object info) {
        try {
            int active = info.getClass().getField("activeColorMode").getInt(info);
            int[] modes = (int[]) info.getClass().getField("supportedColorModes").get(info);
            StringBuilder list = new StringBuilder();
            for (int m : modes) list.append(list.length() > 0 ? "," : "").append(colourModeName(m));
            String line = "panel colour mode " + colourModeName(active) + " (of " + list + ")";
            try {
                Object hdr = info.getClass().getField("hdrCapabilities").get(info);
                if (hdr != null) {
                    int[] types = (int[]) hdr.getClass().getMethod("getSupportedHdrTypes").invoke(hdr);
                    float max = (Float) hdr.getClass().getMethod("getDesiredMaxLuminance").invoke(hdr);
                    float average = (Float) hdr.getClass().getMethod("getDesiredMaxAverageLuminance").invoke(hdr);
                    float min = (Float) hdr.getClass().getMethod("getDesiredMinLuminance").invoke(hdr);
                    line += "; HDR types " + java.util.Arrays.toString(types) + ", luminance max " + max + " nits, max average "
                        + average + ", min " + min;
                }
            } catch (Throwable noHdr) { line += "; HDR capabilities not readable (" + noHdr + ")"; }
            log(line);
        } catch (Throwable unavailable) {
            log("the panel's colour modes are not readable (" + unavailable + ")");
        }
    }

    /** Android's ColorMode values (graphics.common), by name where this helper knows them. */
    static String colourModeName(int mode) {
        switch (mode) {
            case 0: return "NATIVE";
            case 7: return "SRGB";
            case 8: return "ADOBE_RGB";
            case 9: return "DISPLAY_P3";
            case 10: return "BT2020";
            case 11: return "BT2100_PQ";
            case 12: return "BT2100_HLG";
            case 13: return "DISPLAY_BT2020";
            default: return Integer.toString(mode);
        }
    }

    /** The choice among the modes, apart from the framework (see mode()); null without the active mode. */
    static int[] pick(int[] ids, int[] widths, int[] heights, int[] rates, int active) {
        int width = 0, height = 0, rate = 0;
        for (int i = 0; i < ids.length; i++) if (ids[i] == active) { width = widths[i]; height = heights[i]; rate = rates[i]; }
        if (width <= 0 || height <= 0) return null;
        int best = 0, nativeAt = -1;
        for (int i = 0; i < ids.length; i++) {
            if (widths[i] == width && heights[i] == height && rates[i] > best) best = rates[i];
            if (widths[i] <= 0 || heights[i] <= 0) continue;
            long area = (long) widths[i] * heights[i], most = nativeAt < 0 ? 0 : (long) widths[nativeAt] * heights[nativeAt];
            if (nativeAt < 0 || area > most || (area == most && widths[i] == widths[nativeAt] && rates[i] > rates[nativeAt])) nativeAt = i;
        }
        return new int[]{width, height, best, widths[nativeAt], heights[nativeAt], rates[nativeAt], ids[nativeAt], rate, active};
    }
}
