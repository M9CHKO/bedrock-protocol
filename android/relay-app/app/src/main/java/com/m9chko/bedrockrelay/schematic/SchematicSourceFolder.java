package com.m9chko.bedrockrelay.schematic;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/** A persisted Storage Access Framework folder containing user schematics. */
public final class SchematicSourceFolder {
    private static final String PREFERENCES = "schematic_repository";
    private static final String KEY_TREE_URI = "source_tree_uri";
    private static final int MAX_DEPTH = 3;
    private static final int MAX_FILES = 256;
    private static final String[] COLUMNS = {
        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE,
        DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        DocumentsContract.Document.COLUMN_SIZE
    };

    private final ContentResolver resolver;
    private final SharedPreferences preferences;

    public SchematicSourceFolder(Context context) {
        Context app = context.getApplicationContext();
        resolver = app.getContentResolver();
        preferences = app.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
    }

    public void saveTree(Uri treeUri) {
        if (treeUri == null) return;
        preferences.edit().putString(KEY_TREE_URI, treeUri.toString()).apply();
    }

    public void clear() {
        preferences.edit().remove(KEY_TREE_URI).apply();
    }

    public Uri treeUri() {
        String value = preferences.getString(KEY_TREE_URI, "");
        if (value == null || value.trim().isEmpty()) return null;
        try {
            return Uri.parse(value);
        } catch (Throwable ignored) {
            return null;
        }
    }

    public boolean configured() {
        return treeUri() != null;
    }

    public ScanResult scan() {
        Uri tree = treeUri();
        if (tree == null) {
            return new ScanResult(false, "", new ArrayList<>(), "");
        }
        List<SourceEntry> entries = new ArrayList<>();
        try {
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            scanDirectory(tree, rootId, "", 0, entries);
            entries.sort(Comparator
                .comparingLong((SourceEntry value) -> value.modifiedAtMs)
                .reversed()
                .thenComparing(value -> value.relativePath));
            return new ScanResult(
                true,
                displayName(tree, rootId),
                entries,
                ""
            );
        } catch (SecurityException error) {
            return new ScanResult(
                true,
                fallbackTreeName(tree),
                entries,
                "Доступ к папке потерян — выберите её повторно"
            );
        } catch (Throwable error) {
            String message = error.getMessage();
            return new ScanResult(
                true,
                fallbackTreeName(tree),
                entries,
                message == null || message.trim().isEmpty()
                    ? "Не удалось прочитать папку"
                    : message
            );
        }
    }

    private void scanDirectory(
        Uri tree,
        String documentId,
        String parentPath,
        int depth,
        List<SourceEntry> entries
    ) {
        if (depth > MAX_DEPTH || entries.size() >= MAX_FILES) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(
            tree,
            documentId
        );
        try (Cursor cursor = resolver.query(children, COLUMNS, null, null, null)) {
            if (cursor == null) return;
            while (cursor.moveToNext() && entries.size() < MAX_FILES) {
                String childId = cursor.getString(0);
                String name = safeName(cursor.getString(1));
                String mime = cursor.getString(2);
                long modified = cursor.isNull(3) ? 0L : cursor.getLong(3);
                long size = cursor.isNull(4) ? 0L : cursor.getLong(4);
                String relative = parentPath.isEmpty()
                    ? name
                    : parentPath + "/" + name;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    scanDirectory(
                        tree,
                        childId,
                        relative,
                        depth + 1,
                        entries
                    );
                } else if (isSupportedFileName(name)) {
                    Uri document = DocumentsContract.buildDocumentUriUsingTree(
                        tree,
                        childId
                    );
                    entries.add(new SourceEntry(
                        document,
                        name,
                        relative,
                        Math.max(0L, size),
                        Math.max(0L, modified)
                    ));
                }
            }
        }
    }

    private String displayName(Uri tree, String rootId) {
        Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, rootId);
        try (Cursor cursor = resolver.query(
            document,
            new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME},
            null,
            null,
            null
        )) {
            if (cursor != null && cursor.moveToFirst()) {
                String name = safeName(cursor.getString(0));
                if (!name.isEmpty()) return name;
            }
        }
        return fallbackTreeName(tree);
    }

    public static boolean isSupportedFileName(String name) {
        if (name == null) return false;
        String value = name.toLowerCase(Locale.ROOT).trim();
        return value.endsWith(".mcstructure") || value.endsWith(".nbt") ||
            value.endsWith(".litematic") || value.endsWith(".schem") ||
            value.endsWith(".schematic");
    }

    private static String safeName(String value) {
        if (value == null) return "";
        String result = value.replaceAll("[\\p{Cntrl}/\\\\]", "_").trim();
        if (result.length() > 160) result = result.substring(0, 160);
        return result;
    }

    private static String fallbackTreeName(Uri tree) {
        String segment = tree == null ? null : tree.getLastPathSegment();
        if (segment == null || segment.trim().isEmpty()) return "Папка схем";
        int separator = segment.lastIndexOf(':');
        return safeName(separator >= 0 ? segment.substring(separator + 1) : segment);
    }

    public static final class SourceEntry {
        public final Uri uri;
        public final String name;
        public final String relativePath;
        public final long sizeBytes;
        public final long modifiedAtMs;

        SourceEntry(
            Uri uri,
            String name,
            String relativePath,
            long sizeBytes,
            long modifiedAtMs
        ) {
            this.uri = uri;
            this.name = name;
            this.relativePath = relativePath;
            this.sizeBytes = sizeBytes;
            this.modifiedAtMs = modifiedAtMs;
        }
    }

    public static final class ScanResult {
        public final boolean configured;
        public final String folderName;
        public final List<SourceEntry> entries;
        public final String errorMessage;

        ScanResult(
            boolean configured,
            String folderName,
            List<SourceEntry> entries,
            String errorMessage
        ) {
            this.configured = configured;
            this.folderName = folderName;
            this.entries = entries;
            this.errorMessage = errorMessage;
        }
    }
}
