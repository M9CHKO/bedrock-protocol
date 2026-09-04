package com.m9chko.bedrockrelay;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.regex.Pattern;

/** Persistent, bounded and secret-redacted relay diagnostics. */
final class DiagnosticsLog {
    private static final Object LOCK = new Object();
    private static final String CURRENT = "relay-diagnostics.log";
    private static final String PREVIOUS = "relay-diagnostics.previous.log";
    // Keep at most two small segments. Detailed packet diagnostics must never
    // be allowed to compete with the relay and Minecraft for phone storage or
    // spend minutes rewriting an ever-growing file.
    private static final long MAX_FILE_BYTES = 256L * 1024L;
    private static final long MAX_LOG_AGE_MILLIS = 24L * 60L * 60L * 1000L;
    private static final int MAX_MESSAGE_CHARS = 32 * 1024;
    private static final long DUPLICATE_WINDOW_MILLIS = 5_000L;
    private static final long DEBUG_WINDOW_MILLIS = 10_000L;
    private static final int MAX_DEBUG_LINES_PER_WINDOW = 120;

    private static final SimpleDateFormat TIMESTAMP_FORMAT =
        new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSSXXX", Locale.US);
    private static String duplicateFingerprint = "";
    private static String duplicateLevel = "INFO";
    private static String duplicateComponent = "app";
    private static long duplicateWindowStartedAt;
    private static int duplicateCount;
    private static long debugWindowStartedAt;
    private static int debugLinesInWindow;
    private static int suppressedDebugLines;

    private static final Pattern JSON_SECRET = Pattern.compile(
        "(?i)(\\\"(?:access_token|refresh_token|identitytoken|token|" +
            "content_key|authorization|cookie|client_secret|password|" +
            "device_code|user_code|code)\\\"\\s*:\\s*\\\")[^\\\"]*(\\\")"
    );
    private static final Pattern BEARER = Pattern.compile(
        "(?i)\\bBearer\\s+[A-Za-z0-9._~+/=-]+"
    );
    private static final Pattern XBL = Pattern.compile(
        "(?i)XBL3\\.0\\s+x=[^\\s\\\"']+"
    );
    private static final Pattern URL_QUERY_VALUE = Pattern.compile(
        "(?i)([?&][^=&#\\s]+)=([^&\\s#]+)"
    );
    private static final Pattern ASSIGNED_SECRET = Pattern.compile(
        "(?i)((?:access_token|refresh_token|identitytoken|token|" +
            "content_key|authorization|cookie|client_secret|password|" +
            "device_code|user_code)\\s*[:=]\\s*)(\\\"[^\\\"]*\\\"|" +
            "[^&\\s,;]+)"
    );
    private static final Pattern JWT = Pattern.compile(
        "\\beyJ[A-Za-z0-9_-]{20,}\\.[A-Za-z0-9_-]{20,}" +
            "\\.[A-Za-z0-9_-]{10,}\\b"
    );

    private DiagnosticsLog() {}

    static void append(
        Context context,
        String level,
        String component,
        String message
    ) {
        appendAt(context, level, component, message, 0);
    }

    static void appendAt(
        Context context,
        String level,
        String component,
        String message,
        long timestampMillis
    ) {
        if (context == null) return;
        if (!context.getSharedPreferences(
            RelayService.PREFERENCES,
            Context.MODE_PRIVATE
        ).getBoolean(RelayService.KEY_DETAILED_LOGS, true)) {
            return;
        }
        synchronized (LOCK) {
            try {
                long now = System.currentTimeMillis();
                String normalizedLevel = cleanTag(level, "INFO");
                String normalizedComponent = cleanTag(component, "app");
                String normalizedMessage = redact(message);
                pruneExpired(context, now);

                if (rollDebugWindow(context, normalizedLevel, now)) {
                    return;
                }
                String fingerprint = normalizedLevel + '\u0000' +
                    normalizedComponent + '\u0000' + normalizedMessage;
                if (fingerprint.equals(duplicateFingerprint) &&
                    now - duplicateWindowStartedAt <
                        DUPLICATE_WINDOW_MILLIS) {
                    ++duplicateCount;
                    return;
                }
                flushDuplicateSummary(context, now);
                duplicateFingerprint = fingerprint;
                duplicateLevel = normalizedLevel;
                duplicateComponent = normalizedComponent;
                duplicateWindowStartedAt = now;
                duplicateCount = 0;
                writeLine(
                    context,
                    normalizedLevel,
                    normalizedComponent,
                    normalizedMessage,
                    timestampMillis > 0 ? timestampMillis : now,
                    shouldSync(normalizedLevel, normalizedComponent)
                );
            } catch (Throwable ignored) {
                // Diagnostics must never terminate the foreground relay.
            }
        }
    }

    static void appendError(
        Context context,
        String component,
        String message,
        Throwable error
    ) {
        String detail = message == null ? "Error" : message;
        if (error != null) detail += "\n" + stackTrace(error);
        append(context, "ERROR", component, detail);
    }

    static String readAll(Context context) {
        synchronized (LOCK) {
            pruneExpired(context, System.currentTimeMillis());
            StringBuilder result = new StringBuilder();
            File previous = file(context, PREVIOUS);
            if (previous.isFile()) {
                result.append("=== previous run segment ===\n");
                result.append(readFile(previous));
            }
            File current = file(context, CURRENT);
            if (current.isFile()) {
                if (result.length() != 0) result.append('\n');
                result.append("=== current log ===\n");
                result.append(readFile(current));
            }
            return result.length() == 0
                ? "Журнал пока пуст."
                : result.toString();
        }
    }

    static String readTail(Context context, int maximumBytes) {
        synchronized (LOCK) {
            pruneExpired(context, System.currentTimeMillis());
            File current = file(context, CURRENT);
            if (!current.isFile()) return "Журнал пока пуст.";
            byte[] bytes = readBytes(current);
            int start = Math.max(0, bytes.length - Math.max(maximumBytes, 1024));
            while (start < bytes.length && start > 0 && bytes[start] != '\n') {
                ++start;
            }
            String prefix = start == 0 ? "" : "… более ранние строки скрыты …\n";
            return prefix + new String(
                bytes,
                Math.min(start, bytes.length),
                bytes.length - Math.min(start, bytes.length),
                StandardCharsets.UTF_8
            );
        }
    }

    static void clear(Context context) {
        synchronized (LOCK) {
            deleteQuietly(file(context, CURRENT));
            deleteQuietly(file(context, PREVIOUS));
            duplicateFingerprint = "";
            duplicateWindowStartedAt = 0L;
            duplicateCount = 0;
            debugWindowStartedAt = 0L;
            debugLinesInWindow = 0;
            suppressedDebugLines = 0;
        }
    }

    static String path(Context context) {
        return file(context, CURRENT).getAbsolutePath();
    }

    private static void rotateIfNeeded(Context context, File current) {
        if (!current.isFile() || current.length() < MAX_FILE_BYTES) return;
        File previous = file(context, PREVIOUS);
        deleteQuietly(previous);
        if (!current.renameTo(previous)) {
            deleteQuietly(current);
        }
    }

    private static void pruneExpired(Context context, long now) {
        File current = file(context, CURRENT);
        File previous = file(context, PREVIOUS);
        if (isExpired(current, now)) deleteQuietly(current);
        if (isExpired(previous, now)) deleteQuietly(previous);
    }

    private static boolean isExpired(File file, long now) {
        return file.isFile() && file.lastModified() > 0L &&
            now - file.lastModified() > MAX_LOG_AGE_MILLIS;
    }

    /** Returns true when this DEBUG line was intentionally dropped. */
    private static boolean rollDebugWindow(
        Context context,
        String level,
        long now
    ) throws Exception {
        if (debugWindowStartedAt == 0L ||
            now - debugWindowStartedAt >= DEBUG_WINDOW_MILLIS) {
            flushDebugSummary(context, now);
            debugWindowStartedAt = now;
            debugLinesInWindow = 0;
        } else if (!"DEBUG".equalsIgnoreCase(level) &&
            suppressedDebugLines != 0) {
            flushDebugSummary(context, now);
        }
        if (!"DEBUG".equalsIgnoreCase(level)) return false;
        if (debugLinesInWindow >= MAX_DEBUG_LINES_PER_WINDOW) {
            ++suppressedDebugLines;
            return true;
        }
        ++debugLinesInWindow;
        return false;
    }

    private static void flushDebugSummary(Context context, long now)
        throws Exception {
        if (suppressedDebugLines == 0) return;
        writeLine(
            context,
            "WARN",
            "log",
            "Suppressed " + suppressedDebugLines +
                " high-frequency DEBUG entries",
            now,
            false
        );
        suppressedDebugLines = 0;
    }

    private static void flushDuplicateSummary(Context context, long now)
        throws Exception {
        if (duplicateCount == 0) return;
        writeLine(
            context,
            duplicateLevel,
            duplicateComponent,
            "Previous identical entry repeated " + duplicateCount +
                " times",
            now,
            false
        );
        duplicateCount = 0;
    }

    private static void writeLine(
        Context context,
        String level,
        String component,
        String message,
        long timestampMillis,
        boolean forceSync
    ) throws Exception {
        File current = file(context, CURRENT);
        rotateIfNeeded(context, current);
        String timestamp = TIMESTAMP_FORMAT.format(new Date(timestampMillis));
        String line = timestamp + " [" + level + "] [" + component + "] " +
            message + "\n";
        try (FileOutputStream output = new FileOutputStream(current, true)) {
            output.write(line.getBytes(StandardCharsets.UTF_8));
            // Ordinary ERROR events are flushed by close. Only an actual
            // process crash needs an expensive durable sync before Android
            // terminates the process.
            if (forceSync) output.getFD().sync();
        }
    }

    private static boolean shouldSync(String level, String component) {
        return "FATAL".equalsIgnoreCase(level) ||
            "crash".equalsIgnoreCase(component);
    }

    private static File file(Context context, String name) {
        return new File(context.getFilesDir(), name);
    }

    private static String readFile(File file) {
        return new String(readBytes(file), StandardCharsets.UTF_8);
    }

    private static byte[] readBytes(File file) {
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count != 0) output.write(buffer, 0, count);
            }
            return output.toByteArray();
        } catch (Throwable ignored) {
            return new byte[0];
        }
    }

    private static String stackTrace(Throwable error) {
        StringWriter text = new StringWriter();
        error.printStackTrace(new PrintWriter(text));
        return text.toString();
    }

    private static String redact(String value) {
        String result = value == null ? "" : value;
        result = JSON_SECRET.matcher(result).replaceAll("$1<redacted>$2");
        result = ASSIGNED_SECRET.matcher(result).replaceAll("$1<redacted>");
        result = BEARER.matcher(result).replaceAll("Bearer <redacted>");
        result = XBL.matcher(result).replaceAll("XBL3.0 x=<redacted>");
        result = URL_QUERY_VALUE.matcher(result).replaceAll("$1=<redacted>");
        result = JWT.matcher(result).replaceAll("<redacted-jwt>");
        result = result.replace("\r\n", "\n").replace('\r', '\n');
        if (result.length() > MAX_MESSAGE_CHARS) {
            result = result.substring(0, MAX_MESSAGE_CHARS) + "…<truncated>";
        }
        return result;
    }

    private static String cleanTag(String value, String fallback) {
        String result = value == null || value.isEmpty() ? fallback : value;
        return result.replaceAll("[^A-Za-z0-9_.-]", "_");
    }

    private static void deleteQuietly(File file) {
        try {
            if (file.exists()) file.delete();
        } catch (Throwable ignored) {
        }
    }
}
