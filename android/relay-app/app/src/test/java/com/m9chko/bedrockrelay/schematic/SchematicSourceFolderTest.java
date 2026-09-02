package com.m9chko.bedrockrelay.schematic;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class SchematicSourceFolderTest {
    @Test
    public void recognizesEverySupportedSchematicExtension() {
        assertTrue(SchematicSourceFolder.isSupportedFileName("house.mcstructure"));
        assertTrue(SchematicSourceFolder.isSupportedFileName("HOUSE.NBT"));
        assertTrue(SchematicSourceFolder.isSupportedFileName("base.litematic"));
        assertTrue(SchematicSourceFolder.isSupportedFileName("build.schem"));
        assertTrue(SchematicSourceFolder.isSupportedFileName("old.schematic"));
    }

    @Test
    public void rejectsUnrelatedFilesAndSuffixTricks() {
        assertFalse(SchematicSourceFolder.isSupportedFileName("pack.zip"));
        assertFalse(SchematicSourceFolder.isSupportedFileName("house.nbt.txt"));
        assertFalse(SchematicSourceFolder.isSupportedFileName(null));
    }
}
