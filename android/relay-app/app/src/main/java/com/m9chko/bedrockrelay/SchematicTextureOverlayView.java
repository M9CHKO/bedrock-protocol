package com.m9chko.bedrockrelay;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.View;

import com.m9chko.bedrockrelay.schematic.SchematicModel;

import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Map;

/**
 * Click-through textured schematic projection. Unlike Bedrock's falling-block
 * entity renderer, this draws the collision geometry of the exact transformed
 * block state, so stairs, slabs and other non-cubes keep their orientation.
 */
final class SchematicTextureOverlayView extends View {
    private static final double NEAR_PLANE = 0.10;
    private static final float SHAPE_EPSILON = 0.0001f;
    private static final int MAX_RENDERED_BLOCKS = 1_200;
    private static final int MAX_RENDERED_PARTS = 3_600;
    private static final int MAX_TEXTURED_PARTS = 1_200;
    private static final float[] FULL_BLOCK = {0f, 0f, 0f, 1f, 1f, 1f};
    private static final Comparator<ProjectedPart> FAR_TO_NEAR =
        (left, right) -> Double.compare(right.depth, left.depth);

    private static final int[] TOP_FACE = {4, 5, 7, 6};
    private static final int[] BOTTOM_FACE = {0, 1, 3, 2};
    private static final int[] X_MIN_FACE = {4, 6, 2, 0};
    private static final int[] X_MAX_FACE = {5, 7, 3, 1};
    private static final int[] Z_MIN_FACE = {4, 5, 1, 0};
    private static final int[] Z_MAX_FACE = {6, 7, 3, 2};
    private static final int FACE_TOP = 1;
    private static final int FACE_BOTTOM = 1 << 1;
    private static final int FACE_X_MIN = 1 << 2;
    private static final int FACE_X_MAX = 1 << 3;
    private static final int FACE_Z_MIN = 1 << 4;
    private static final int FACE_Z_MAX = 1 << 5;

    private final PacketCameraTracker camera = new PacketCameraTracker();
    private final Paint texturePaint = new Paint();
    private final Path path = new Path();
    private final Matrix textureMatrix = new Matrix();
    private final float[] sourceQuad = new float[8];
    private final float[] targetQuad = new float[8];
    private final float[] quadU = new float[4];
    private final float[] quadV = new float[4];
    private final double[] screenX = new double[8];
    private final double[] screenY = new double[8];
    private final ProjectedPart[] renderQueue =
        new ProjectedPart[MAX_RENDERED_PARTS];
    private final Map<String, float[]> collisionShapeCache = new HashMap<>();
    private final SchematicTextureAtlas textureAtlas;

    private SchematicModel model;
    private int fieldOfView = 70;
    private int opacityPercent = 42;
    private int maximumDistance = 96;
    private int rotation;
    private boolean mirrored;
    private int selectedLayer = -1;
    private int anchorX;
    private int anchorY;
    private int anchorZ;
    private SchematicPlacementTransform transform;
    private int[] worldBlockIndices;
    private byte[] worldStates;
    private String[] transformedBedrockPalette;

    SchematicTextureOverlayView(Context context) {
        super(context);
        textureAtlas = new SchematicTextureAtlas(context, this::requestFrame);
        setBackgroundColor(Color.TRANSPARENT);
        setWillNotDraw(false);
        texturePaint.setAntiAlias(false);
        texturePaint.setFilterBitmap(false);
        texturePaint.setDither(false);
        for (int index = 0; index < renderQueue.length; ++index) {
            renderQueue[index] = new ProjectedPart();
        }
    }

    void setModel(SchematicModel value) {
        model = value;
        clearWorldStates();
        rebuildTransform();
        requestFrame();
    }

    void configure(
        int fov,
        int opacity,
        int distance,
        int rotation,
        boolean mirrored,
        int layer
    ) {
        fieldOfView = fov;
        opacityPercent = opacity;
        maximumDistance = distance;
        this.rotation = Math.floorMod(rotation, 4);
        this.mirrored = mirrored;
        selectedLayer = layer;
        rebuildTransform();
        requestFrame();
    }

    void setAnchor(int x, int y, int z) {
        anchorX = x;
        anchorY = y;
        anchorZ = z;
        rebuildTransform();
        requestFrame();
    }

    void setWorldStates(
        SchematicModel source,
        int[] blockIndices,
        byte[] states,
        int correct,
        int wrong,
        String[] bedrockPalette
    ) {
        if (model != source || blockIndices == null || states == null ||
            states.length != blockIndices.length || bedrockPalette == null ||
            bedrockPalette.length != source.paletteSize()) {
            return;
        }
        worldBlockIndices = blockIndices;
        worldStates = states;
        transformedBedrockPalette = bedrockPalette;
        requestFrame();
    }

    void clearWorldStates() {
        worldBlockIndices = null;
        worldStates = null;
        transformedBedrockPalette = null;
        requestFrame();
    }

    void submitCamera(EntityOutlineOverlayController.CameraSample value) {
        camera.update(value);
        requestFrame();
    }

    private void rebuildTransform() {
        SchematicModel current = model;
        transform = current == null
            ? null
            : new SchematicPlacementTransform(
                anchorX,
                anchorY,
                anchorZ,
                current.sizeX(),
                rotation,
                mirrored
            );
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        SchematicModel current = model;
        SchematicPlacementTransform placement = transform;
        int[] blockIndices = worldBlockIndices;
        byte[] states = worldStates;
        String[] palette = transformedBedrockPalette;
        if (current == null || placement == null || blockIndices == null ||
            states == null || palette == null ||
            blockIndices.length != states.length || getWidth() <= 0 ||
            getHeight() <= 0) {
            return;
        }

        PacketCameraTracker.State cameraState = camera.frame(
            System.nanoTime(),
            fieldOfView,
            false
        );
        if (cameraState == null) return;
        double yaw = Math.toRadians(cameraState.yaw);
        double pitch = Math.toRadians(MotionSmoother.clamp(
            cameraState.pitch,
            -90.0,
            90.0
        ));
        double sinYaw = Math.sin(yaw);
        double cosYaw = Math.cos(yaw);
        double sinPitch = Math.sin(pitch);
        double cosPitch = Math.cos(pitch);
        double focal = ProjectionMath.focalPixels(
            getHeight(),
            cameraState.verticalFov
        );

        int activeBlocks = 0;
        for (int index = 0; index < states.length; ++index) {
            if ((states[index] == SchematicDebugMarkerPlanner.BLOCK_UNKNOWN ||
                    states[index] == SchematicDebugMarkerPlanner.BLOCK_MISSING) &&
                (selectedLayer < 0 || current.yFromIndex(blockIndices[index]) ==
                    selectedLayer)) {
                ++activeBlocks;
            }
        }
        int stride = Math.max(
            1,
            (activeBlocks + MAX_RENDERED_BLOCKS - 1) / MAX_RENDERED_BLOCKS
        );
        int activeIndex = 0;
        int queued = 0;
        double maximumDistanceSquared =
            (double) maximumDistance * maximumDistance;
        for (int listIndex = 0; listIndex < blockIndices.length; ++listIndex) {
            byte worldState = states[listIndex];
            if (worldState != SchematicDebugMarkerPlanner.BLOCK_UNKNOWN &&
                worldState != SchematicDebugMarkerPlanner.BLOCK_MISSING) {
                continue;
            }
            int blockIndex = blockIndices[listIndex];
            int localY = current.yFromIndex(blockIndex);
            if (selectedLayer >= 0 && localY != selectedLayer) continue;
            if (activeIndex++ % stride != 0) continue;
            int localX = current.xFromIndex(blockIndex);
            int localZ = current.zFromIndex(blockIndex);
            int paletteIndex = current.paletteIndexAtLinear(blockIndex);
            String blockState = paletteIndex >= 0 && paletteIndex < palette.length
                ? palette[paletteIndex]
                : current.paletteState(paletteIndex);
            SchematicPlacementTransform.BlockPosition world =
                placement.worldBlock(localX, localY, localZ);
            int cellFaces = exposedCellFaces(current, placement, world);
            float[] boxes = collisionBoxes(blockState);
            for (int offset = 0; offset + 5 < boxes.length; offset += 6) {
                double minimumX = world.x() + boxes[offset];
                double minimumY = world.y() + boxes[offset + 1];
                double minimumZ = world.z() + boxes[offset + 2];
                double maximumX = world.x() + boxes[offset + 3];
                double maximumY = world.y() + boxes[offset + 4];
                double maximumZ = world.z() + boxes[offset + 5];
                double centerX = (minimumX + maximumX) * 0.5;
                double centerY = (minimumY + maximumY) * 0.5;
                double centerZ = (minimumZ + maximumZ) * 0.5;
                double dx = centerX - cameraState.x;
                double dy = centerY - cameraState.y;
                double dz = centerZ - cameraState.z;
                if (dx * dx + dy * dy + dz * dz > maximumDistanceSquared) {
                    continue;
                }
                double depth = ProjectionMath.depth(
                    dx,
                    dy,
                    dz,
                    sinYaw,
                    cosYaw,
                    sinPitch,
                    cosPitch
                );
                if (depth <= NEAR_PLANE) continue;
                renderQueue[queued++].set(
                    blockState,
                    minimumX,
                    minimumY,
                    minimumZ,
                    maximumX,
                    maximumY,
                    maximumZ,
                    boxes[offset],
                    boxes[offset + 1],
                    boxes[offset + 2],
                    boxes[offset + 3],
                    boxes[offset + 4],
                    boxes[offset + 5],
                    centerX,
                    centerY,
                    centerZ,
                    depth,
                    exposedShapeFaces(cellFaces, boxes, offset)
                );
                if (queued >= MAX_RENDERED_PARTS) break;
            }
            if (queued >= MAX_RENDERED_PARTS) break;
        }

        Arrays.sort(renderQueue, 0, queued, FAR_TO_NEAR);
        int firstTextured = Math.max(0, queued - MAX_TEXTURED_PARTS);
        texturePaint.setAlpha(Math.max(
            26,
            Math.min(255, Math.round(255f * opacityPercent / 100f))
        ));
        for (int index = firstTextured; index < queued; ++index) {
            ProjectedPart part = renderQueue[index];
            if (!projectPart(
                    part,
                    cameraState,
                    sinYaw,
                    cosYaw,
                    sinPitch,
                    cosPitch,
                    focal
                )) {
                continue;
            }
            drawPart(canvas, part, cameraState);
        }
        if (cameraState.animating) requestFrame();
    }

    private float[] collisionBoxes(String blockState) {
        String key = blockState == null ? "" : blockState;
        float[] cached = collisionShapeCache.get(key);
        if (cached != null) return cached;
        float[] boxes = null;
        try {
            boxes = NativeBridge.schematicCollisionBoxes(key);
        } catch (Throwable ignored) {
        }
        if (!validCollisionBoxes(boxes)) boxes = FULL_BLOCK;
        collisionShapeCache.put(key, boxes);
        return boxes;
    }

    private static boolean validCollisionBoxes(float[] boxes) {
        if (boxes == null || boxes.length == 0 || boxes.length % 6 != 0 ||
            boxes.length > 18) {
            return false;
        }
        for (int offset = 0; offset < boxes.length; offset += 6) {
            for (int index = 0; index < 6; ++index) {
                if (!Float.isFinite(boxes[offset + index])) return false;
            }
            if (boxes[offset + 3] <= boxes[offset] ||
                boxes[offset + 4] <= boxes[offset + 1] ||
                boxes[offset + 5] <= boxes[offset + 2]) {
                return false;
            }
        }
        return true;
    }

    private int exposedCellFaces(
        SchematicModel current,
        SchematicPlacementTransform placement,
        SchematicPlacementTransform.BlockPosition world
    ) {
        int faces = 0;
        if (selectedLayer >= 0 || isSchematicAir(
                current,
                placement,
                world.x(),
                world.y() + 1,
                world.z()
            )) faces |= FACE_TOP;
        if (selectedLayer >= 0 || isSchematicAir(
                current,
                placement,
                world.x(),
                world.y() - 1,
                world.z()
            )) faces |= FACE_BOTTOM;
        if (isSchematicAir(current, placement, world.x() - 1, world.y(), world.z())) {
            faces |= FACE_X_MIN;
        }
        if (isSchematicAir(current, placement, world.x() + 1, world.y(), world.z())) {
            faces |= FACE_X_MAX;
        }
        if (isSchematicAir(current, placement, world.x(), world.y(), world.z() - 1)) {
            faces |= FACE_Z_MIN;
        }
        if (isSchematicAir(current, placement, world.x(), world.y(), world.z() + 1)) {
            faces |= FACE_Z_MAX;
        }
        return faces;
    }

    private static boolean isSchematicAir(
        SchematicModel current,
        SchematicPlacementTransform placement,
        int worldX,
        int worldY,
        int worldZ
    ) {
        SchematicPlacementTransform.BlockPosition local = placement.localBlock(
            worldX,
            worldY,
            worldZ
        );
        return current.isAirAt(local.x(), local.y(), local.z());
    }

    private static int exposedShapeFaces(
        int cellFaces,
        float[] boxes,
        int offset
    ) {
        int faces = 0;
        if (boxes[offset + 4] < 1f - SHAPE_EPSILON ||
            (cellFaces & FACE_TOP) != 0) faces |= FACE_TOP;
        if (boxes[offset + 1] > SHAPE_EPSILON ||
            (cellFaces & FACE_BOTTOM) != 0) faces |= FACE_BOTTOM;
        if (boxes[offset] > SHAPE_EPSILON ||
            (cellFaces & FACE_X_MIN) != 0) faces |= FACE_X_MIN;
        if (boxes[offset + 3] < 1f - SHAPE_EPSILON ||
            (cellFaces & FACE_X_MAX) != 0) faces |= FACE_X_MAX;
        if (boxes[offset + 2] > SHAPE_EPSILON ||
            (cellFaces & FACE_Z_MIN) != 0) faces |= FACE_Z_MIN;
        if (boxes[offset + 5] < 1f - SHAPE_EPSILON ||
            (cellFaces & FACE_Z_MAX) != 0) faces |= FACE_Z_MAX;
        return faces;
    }

    private boolean projectPart(
        ProjectedPart part,
        PacketCameraTracker.State cameraState,
        double sinYaw,
        double cosYaw,
        double sinPitch,
        double cosPitch,
        double focal
    ) {
        double minimumScreenX = Double.POSITIVE_INFINITY;
        double maximumScreenX = Double.NEGATIVE_INFINITY;
        double minimumScreenY = Double.POSITIVE_INFINITY;
        double maximumScreenY = Double.NEGATIVE_INFINITY;
        for (int corner = 0; corner < 8; ++corner) {
            double worldX = (corner & 1) != 0 ? part.maximumX : part.minimumX;
            double worldZ = (corner & 2) != 0 ? part.maximumZ : part.minimumZ;
            double worldY = (corner & 4) != 0 ? part.maximumY : part.minimumY;
            double dx = worldX - cameraState.x;
            double dy = worldY - cameraState.y;
            double dz = worldZ - cameraState.z;
            double depth = ProjectionMath.depth(
                dx, dy, dz,
                sinYaw, cosYaw, sinPitch, cosPitch
            );
            if (depth <= NEAR_PLANE) return false;
            double x = getWidth() * 0.5 + ProjectionMath.viewX(
                dx, dz, sinYaw, cosYaw
            ) * focal / depth;
            double y = getHeight() * 0.5 - ProjectionMath.viewY(
                dx, dy, dz,
                sinYaw, cosYaw, sinPitch, cosPitch
            ) * focal / depth;
            screenX[corner] = x;
            screenY[corner] = y;
            minimumScreenX = Math.min(minimumScreenX, x);
            maximumScreenX = Math.max(maximumScreenX, x);
            minimumScreenY = Math.min(minimumScreenY, y);
            maximumScreenY = Math.max(maximumScreenY, y);
        }
        return maximumScreenX >= -20 && minimumScreenX <= getWidth() + 20 &&
            maximumScreenY >= -20 && minimumScreenY <= getHeight() + 20;
    }

    private void drawPart(
        Canvas canvas,
        ProjectedPart part,
        PacketCameraTracker.State cameraState
    ) {
        if (cameraState.y >= part.centerY) {
            if ((part.exposedFaces & FACE_TOP) != 0) {
                drawTexturedFace(canvas, part, "top", TOP_FACE);
            }
        } else if ((part.exposedFaces & FACE_BOTTOM) != 0) {
            drawTexturedFace(canvas, part, "bottom", BOTTOM_FACE);
        }
        if ((part.exposedFaces & FACE_X_MIN) != 0 &&
            normalFacingCamera(-1.0, 0.0, part, cameraState)) {
            drawTexturedFace(canvas, part, "west", X_MIN_FACE);
        }
        if ((part.exposedFaces & FACE_X_MAX) != 0 &&
            normalFacingCamera(1.0, 0.0, part, cameraState)) {
            drawTexturedFace(canvas, part, "east", X_MAX_FACE);
        }
        if ((part.exposedFaces & FACE_Z_MIN) != 0 &&
            normalFacingCamera(0.0, -1.0, part, cameraState)) {
            drawTexturedFace(canvas, part, "north", Z_MIN_FACE);
        }
        if ((part.exposedFaces & FACE_Z_MAX) != 0 &&
            normalFacingCamera(0.0, 1.0, part, cameraState)) {
            drawTexturedFace(canvas, part, "south", Z_MAX_FACE);
        }
    }

    private static boolean normalFacingCamera(
        double normalX,
        double normalZ,
        ProjectedPart part,
        PacketCameraTracker.State cameraState
    ) {
        return normalX * (cameraState.x - part.centerX) +
            normalZ * (cameraState.z - part.centerZ) > 0.0;
    }

    private void drawTexturedFace(
        Canvas canvas,
        ProjectedPart part,
        String faceName,
        int[] face
    ) {
        Bitmap texture = textureAtlas.textureFor(
            part.blockState,
            textureFace(part.blockState, faceName)
        );
        if (texture == null || texture.isRecycled()) return;
        path.reset();
        path.moveTo((float) screenX[face[0]], (float) screenY[face[0]]);
        for (int index = 1; index < face.length; ++index) {
            path.lineTo(
                (float) screenX[face[index]],
                (float) screenY[face[index]]
            );
        }
        path.close();
        prepareSourceQuad(part, faceName, texture.getWidth(), texture.getHeight());
        for (int index = 0; index < 4; ++index) {
            targetQuad[index * 2] = (float) screenX[face[index]];
            targetQuad[index * 2 + 1] = (float) screenY[face[index]];
        }
        textureMatrix.reset();
        if (!textureMatrix.setPolyToPoly(
                sourceQuad,
                0,
                targetQuad,
                0,
                4
            )) {
            return;
        }
        int save = canvas.save();
        canvas.clipPath(path);
        canvas.drawBitmap(texture, textureMatrix, texturePaint);
        canvas.restoreToCount(save);
    }

    private void prepareSourceQuad(
        ProjectedPart part,
        String face,
        float width,
        float height
    ) {
        float[] u = quadU;
        float[] v = quadV;
        switch (face) {
            case "top":
            case "bottom":
                u[0] = part.localMinimumX;
                v[0] = part.localMinimumZ;
                u[1] = part.localMaximumX;
                v[1] = part.localMinimumZ;
                u[2] = part.localMaximumX;
                v[2] = part.localMaximumZ;
                u[3] = part.localMinimumX;
                v[3] = part.localMaximumZ;
                for (int index = 0; index < 4; ++index) {
                    float sourceX;
                    float sourceZ;
                    switch (rotation) {
                        case 1:
                            sourceX = v[index];
                            sourceZ = 1f - u[index];
                            break;
                        case 2:
                            sourceX = 1f - u[index];
                            sourceZ = 1f - v[index];
                            break;
                        case 3:
                            sourceX = 1f - v[index];
                            sourceZ = u[index];
                            break;
                        default:
                            sourceX = u[index];
                            sourceZ = v[index];
                            break;
                    }
                    if (mirrored) sourceX = 1f - sourceX;
                    u[index] = sourceX;
                    v[index] = sourceZ;
                }
                break;
            case "east":
            case "west":
                u[0] = part.localMinimumZ;
                v[0] = 1f - part.localMaximumY;
                u[1] = part.localMaximumZ;
                v[1] = 1f - part.localMaximumY;
                u[2] = part.localMaximumZ;
                v[2] = 1f - part.localMinimumY;
                u[3] = part.localMinimumZ;
                v[3] = 1f - part.localMinimumY;
                break;
            default:
                u[0] = part.localMinimumX;
                v[0] = 1f - part.localMaximumY;
                u[1] = part.localMaximumX;
                v[1] = 1f - part.localMaximumY;
                u[2] = part.localMaximumX;
                v[2] = 1f - part.localMinimumY;
                u[3] = part.localMinimumX;
                v[3] = 1f - part.localMinimumY;
                break;
        }
        for (int index = 0; index < 4; ++index) {
            sourceQuad[index * 2] = u[index] * width;
            sourceQuad[index * 2 + 1] = v[index] * height;
        }
    }

    private static String textureFace(String blockState, String face) {
        String value = blockState == null ? "" : blockState;
        int property = value.indexOf("pillar_axis=");
        if (property < 0) property = value.indexOf("axis=");
        if (property >= 0) {
            int start = value.indexOf('=', property) + 1;
            int end = start;
            while (end < value.length() && value.charAt(end) != ',' &&
                value.charAt(end) != ']') ++end;
            String axis = value.substring(start, end);
            if (("x".equals(axis) && ("east".equals(face) || "west".equals(face))) ||
                ("y".equals(axis) && ("top".equals(face) || "bottom".equals(face))) ||
                ("z".equals(axis) && ("north".equals(face) || "south".equals(face)))) {
                return "top";
            }
            return "side";
        }
        return face;
    }

    @Override
    protected void onDetachedFromWindow() {
        textureAtlas.close();
        collisionShapeCache.clear();
        super.onDetachedFromWindow();
    }

    private void requestFrame() {
        postInvalidateOnAnimation();
    }

    private static final class ProjectedPart {
        String blockState;
        double minimumX;
        double minimumY;
        double minimumZ;
        double maximumX;
        double maximumY;
        double maximumZ;
        float localMinimumX;
        float localMinimumY;
        float localMinimumZ;
        float localMaximumX;
        float localMaximumY;
        float localMaximumZ;
        double centerX;
        double centerY;
        double centerZ;
        double depth;
        int exposedFaces;

        void set(
            String blockState,
            double minimumX,
            double minimumY,
            double minimumZ,
            double maximumX,
            double maximumY,
            double maximumZ,
            float localMinimumX,
            float localMinimumY,
            float localMinimumZ,
            float localMaximumX,
            float localMaximumY,
            float localMaximumZ,
            double centerX,
            double centerY,
            double centerZ,
            double depth,
            int exposedFaces
        ) {
            this.blockState = blockState;
            this.minimumX = minimumX;
            this.minimumY = minimumY;
            this.minimumZ = minimumZ;
            this.maximumX = maximumX;
            this.maximumY = maximumY;
            this.maximumZ = maximumZ;
            this.localMinimumX = localMinimumX;
            this.localMinimumY = localMinimumY;
            this.localMinimumZ = localMinimumZ;
            this.localMaximumX = localMaximumX;
            this.localMaximumY = localMaximumY;
            this.localMaximumZ = localMaximumZ;
            this.centerX = centerX;
            this.centerY = centerY;
            this.centerZ = centerZ;
            this.depth = depth;
            this.exposedFaces = exposedFaces;
        }
    }
}
