package com.m9chko.bedrockrelay;

import android.content.Context;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Makes the packaged minecraft-data files available to the native registry. */
final class MinecraftDataAssets {
    static final String PACKAGED_VERSION = "1.21.100";
    private static final String[] NATIVE_FILES = {
        "blocks.json",
        "blockStates.json",
        "blockCollisionShapes.json"
    };

    private MinecraftDataAssets() {}

    static String prepareNativeDirectory(Context context, String version)
        throws IOException {
        if (!PACKAGED_VERSION.equals(version)) return "";
        File directory = new File(
            context.getFilesDir(),
            "minecraft-data/" + version + "-apk-" + BuildConfig.VERSION_CODE
        );
        if (!directory.isDirectory() && !directory.mkdirs() &&
            !directory.isDirectory()) {
            throw new IOException("Не удалось создать каталог minecraft-data");
        }
        for (String name : NATIVE_FILES) {
            File destination = new File(directory, name);
            if (destination.isFile() && destination.length() > 0) continue;
            copyAssetAtomically(
                context,
                "minecraft-data/" + version + "/" + name,
                destination
            );
        }
        return directory.getAbsolutePath();
    }

    private static void copyAssetAtomically(
        Context context,
        String asset,
        File destination
    ) throws IOException {
        File temporary = new File(destination.getParentFile(),
            destination.getName() + ".pending");
        if (temporary.exists() && !temporary.delete()) {
            throw new IOException("Не удалось обновить " + destination.getName());
        }
        byte[] buffer = new byte[32 * 1024];
        try (InputStream input = new BufferedInputStream(
                context.getAssets().open(asset),
                buffer.length
            );
            BufferedOutputStream output = new BufferedOutputStream(
                new FileOutputStream(temporary),
                buffer.length
            )) {
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count > 0) output.write(buffer, 0, count);
            }
        } catch (Throwable error) {
            temporary.delete();
            if (error instanceof IOException) throw (IOException) error;
            throw new IOException("Не удалось распаковать " + asset, error);
        }
        if (temporary.length() == 0 ||
            (!temporary.renameTo(destination) &&
                (!destination.delete() || !temporary.renameTo(destination)))) {
            temporary.delete();
            throw new IOException("Не удалось сохранить " + destination.getName());
        }
    }
}
