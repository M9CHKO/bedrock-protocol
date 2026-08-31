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
        String version,
        String authCacheDirectory
    );

    public static native String supportedVersions();

    public static native void stopRelay();

    public static native void configureRuntime(
        boolean detailedLogging,
        boolean chunkRetentionEnabled,
        int retainedRadiusChunks
    );

    public static native String snapshot();

    public static native String pollEvents();

    // Called by AndroidXboxTokenHttpClient from a native authentication
    // worker. No request or response body is logged on either side.
    public static String httpFetch(String requestJson) throws Exception {
        try {
            return HttpTransport.fetch(requestJson);
        } catch (Exception error) {
            DiagnosticsLog.appendError(
                RelayApplication.context(),
                "http",
                "Android authentication HTTPS request failed; request omitted",
                error
            );
            throw error;
        }
    }
}
