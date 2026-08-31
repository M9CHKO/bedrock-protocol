package com.m9chko.bedrockrelay;

/** Process-local JNI entry points. The foreground service owns their lifetime. */
public final class NativeBridge {
    static {
        System.loadLibrary("bedrock_relay_android");
    }

    private NativeBridge() {}

    public static native String startRelay(
        String destinationHost,
        int destinationPort,
        String authCacheDirectory
    );

    public static native void stopRelay();

    public static native String snapshot();

    public static native String pollEvents();

    // Called by AndroidXboxTokenHttpClient from a native authentication
    // worker. No request or response body is logged on either side.
    public static String httpFetch(String requestJson) throws Exception {
        return HttpTransport.fetch(requestJson);
    }
}
