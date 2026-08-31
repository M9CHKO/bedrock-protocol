package com.m9chko.bedrockrelay;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.GZIPInputStream;

/** HTTPS byte transport used by the C++ Xbox Live token managers. */
final class HttpTransport {
    private static final int CONNECT_TIMEOUT_MS = 30_000;
    private static final int READ_TIMEOUT_MS = 45_000;

    private HttpTransport() {}

    static String fetch(String requestJson) throws Exception {
        JSONObject request = new JSONObject(requestJson);
        String urlText = request.getString("url");
        URI uri = URI.create(urlText);
        if (!"https".equalsIgnoreCase(uri.getScheme())) {
            throw new SecurityException("Authentication transport requires HTTPS");
        }
        String method = request.optString("method", "POST")
            .toUpperCase(Locale.ROOT);
        String endpoint = uri.getHost() +
            (uri.getPath() == null ? "" : uri.getPath());
        long startedAt = System.currentTimeMillis();
        DiagnosticsLog.append(
            RelayApplication.context(),
            "DEBUG",
            "http",
            method + " https://" + endpoint + " started; body/headers omitted"
        );

        HttpURLConnection connection = (HttpURLConnection)
            new URL(urlText).openConnection();
        connection.setConnectTimeout(CONNECT_TIMEOUT_MS);
        connection.setReadTimeout(READ_TIMEOUT_MS);
        connection.setUseCaches(false);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestMethod(method);

        JSONArray headers = request.optJSONArray("headers");
        if (headers != null) {
            for (int index = 0; index < headers.length(); ++index) {
                JSONArray pair = headers.getJSONArray(index);
                String name = pair.getString(0);
                // HttpURLConnection owns this restricted header. The fixed
                // length mode below emits the identical byte count.
                if ("content-length".equalsIgnoreCase(name)) {
                    continue;
                }
                connection.addRequestProperty(name, pair.getString(1));
            }
        }

        byte[] body = request.optString("body", "")
            .getBytes(StandardCharsets.UTF_8);
        if (body.length != 0) {
            connection.setDoOutput(true);
            connection.setFixedLengthStreamingMode(body.length);
            try (OutputStream output = connection.getOutputStream()) {
                output.write(body);
            }
        }

        int status = connection.getResponseCode();
        DiagnosticsLog.append(
            RelayApplication.context(),
            status >= 400 ? "WARN" : "DEBUG",
            "http",
            method + " https://" + endpoint + " status=" + status +
                " elapsedMs=" + (System.currentTimeMillis() - startedAt) +
                "; response body omitted"
        );
        InputStream input = status >= 400
            ? connection.getErrorStream()
            : connection.getInputStream();
        byte[] responseBytes = readAll(
            maybeDecompress(input, connection.getHeaderField("Content-Encoding"))
        );

        JSONObject response = new JSONObject();
        response.put("status", status);
        response.put(
            "statusText",
            connection.getResponseMessage() == null
                ? ""
                : connection.getResponseMessage()
        );
        response.put("bodyText", new String(responseBytes, StandardCharsets.UTF_8));

        JSONArray responseHeaders = new JSONArray();
        for (Map.Entry<String, List<String>> entry
                : connection.getHeaderFields().entrySet()) {
            if (entry.getKey() == null || entry.getValue() == null) {
                continue;
            }
            for (String value : entry.getValue()) {
                JSONArray pair = new JSONArray();
                pair.put(entry.getKey());
                pair.put(value == null ? "" : value);
                responseHeaders.put(pair);
            }
        }
        response.put("headers", responseHeaders);
        connection.disconnect();
        return response.toString();
    }

    private static InputStream maybeDecompress(
        InputStream input,
        String contentEncoding
    ) throws Exception {
        if (input == null) {
            return null;
        }
        if (contentEncoding != null &&
            contentEncoding.toLowerCase(Locale.ROOT).contains("gzip")) {
            return new GZIPInputStream(input);
        }
        return input;
    }

    private static byte[] readAll(InputStream input) throws Exception {
        if (input == null) {
            return new byte[0];
        }
        try (InputStream source = input;
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = source.read(buffer)) >= 0) {
                if (count != 0) {
                    output.write(buffer, 0, count);
                }
            }
            return output.toByteArray();
        }
    }
}
