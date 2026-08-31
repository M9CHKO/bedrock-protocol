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
    private static final long MAX_FILE_BYTES = 768L * 1024L;
    private static final int MAX_MESSAGE_CHARS = 32 * 1024;

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
        if (context == null) return;
        synchronized (LOCK) {
            try {
                File current = file(context, CURRENT);
                rotateIfNeeded(context, current);
                String timestamp = new SimpleDateFormat(
                    "yyyy-MM-dd'T'HH:mm:ss.SSSXXX",
                    Locale.US
                ).format(new Date());
                String normalizedLevel = cleanTag(level, "INFO");
                String line = timestamp + " [" + normalizedLevel +
                    "] [" + cleanTag(component, "app") + "] " +
                    redact(message) + "\n";
                try (FileOutputStream output = new FileOutputStream(current, true)) {
                    output.write(line.getBytes(StandardCharsets.UTF_8));
                    // Closing the stream flushes ordinary high-volume packet
                    // breadcrumbs. Force durable storage only for failures;
                    // an fsync for every inventory packet can itself stall a
                    // low-end phone during world join.
                    if ("ERROR".equalsIgnoreCase(normalizedLevel) ||
                        "FATAL".equalsIgnoreCase(normalizedLevel)) {
                        output.getFD().sync();
                    }
                }
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
