package com.m9chko.bedrockrelay;

/**
 * Packet-tick camera interpolation shared by world-anchored overlay modules.
 * It never reads Minecraft memory or the rendered screen.
 */
final class PacketCameraTracker {
    private static final long LINEAR_PREDICTION_NANOS = 32_000_000L;
    private static final long MAX_PREDICTION_NANOS = 48_000_000L;
    private static final long RENDER_LEAD_NANOS = 2_000_000L;
    private static final long MAXIMUM_FRAME_GAP_NANOS = 250_000_000L;

    private final MotionSmoother.Axis renderX = new MotionSmoother.Axis();
    private final MotionSmoother.Axis renderY = new MotionSmoother.Axis();
    private final MotionSmoother.Axis renderZ = new MotionSmoother.Axis();
    private final MotionSmoother.Axis renderPitch = new MotionSmoother.Axis();
    private final MotionSmoother.Axis renderYaw = new MotionSmoother.Axis();
    private final MotionSmoother.PacketVelocity velocityX =
        new MotionSmoother.PacketVelocity();
    private final MotionSmoother.PacketVelocity velocityY =
        new MotionSmoother.PacketVelocity();
    private final MotionSmoother.PacketVelocity velocityZ =
        new MotionSmoother.PacketVelocity();
    private final MotionSmoother.PacketVelocity velocityPitch =
        new MotionSmoother.PacketVelocity();
    private final MotionSmoother.PacketVelocity velocityYaw =
        new MotionSmoother.PacketVelocity();

    private boolean known;
    private boolean inputTickKnown;
    private long inputTick;
    private double sampleX;
    private double sampleY;
    private double sampleZ;
    private double samplePitch;
    private double sampleYaw;
    private double speedX;
    private double speedY;
    private double speedZ;
    private double speedPitch;
    private double speedYaw;
    private long updatedAtMs;
    private long receivedNanos;
    private long sampleAgeNanos;
    private long lastFrameNanos;
    private double renderedFov = Double.NaN;

    void update(EntityOutlineOverlayController.CameraSample sample) {
        update(sample, System.nanoTime());
    }

    void update(
        EntityOutlineOverlayController.CameraSample sample,
        long nowNanos
    ) {
        if (sample == null || !sample.known) {
            known = false;
            return;
        }
        if (!known) {
            known = true;
            sampleX = sample.x;
            sampleY = sample.y;
            sampleZ = sample.z;
            samplePitch = sample.pitch;
            sampleYaw = sample.yaw;
            resetVelocities();
            renderX.reset(sample.x, 0.0);
            renderY.reset(sample.y, 0.0);
            renderZ.reset(sample.z, 0.0);
            renderPitch.reset(sample.pitch, 0.0);
            renderYaw.reset(sample.yaw, 0.0);
            lastFrameNanos = nowNanos;
        } else {
            if (updatedAtMs > 0 && sample.updatedAtMs > 0 &&
                sample.updatedAtMs < updatedAtMs) return;
            double unwrappedYaw = MotionSmoother.unwrapAngle(
                sampleYaw,
                sample.yaw
            );
            boolean samePacket = inputTickKnown && sample.inputTickKnown
                ? inputTick == sample.inputTick
                : updatedAtMs == sample.updatedAtMs;
            if (samePacket && sample.x == sampleX && sample.y == sampleY &&
                sample.z == sampleZ && sample.pitch == samplePitch &&
                unwrappedYaw == sampleYaw) return;

            boolean tickDiscontinuity = inputTickKnown &&
                sample.inputTickKnown &&
                (sample.inputTick < inputTick || sample.inputTick - inputTick > 20);
            double interval = tickDiscontinuity
                ? Double.NaN
                : MotionSmoother.packetIntervalSeconds(
                    inputTickKnown,
                    inputTick,
                    sample.inputTickKnown,
                    sample.inputTick,
                    updatedAtMs,
                    sample.updatedAtMs
                );
            double dx = sample.x - sampleX;
            double dy = sample.y - sampleY;
            double dz = sample.z - sampleZ;
            boolean teleport = dx * dx + dy * dy + dz * dz > 64.0;
            if (Double.isFinite(interval) && interval >= 0.005 &&
                interval <= 0.5) {
                speedX = velocityX.update(dx, interval, 100.0, 0.00001);
                speedY = velocityY.update(dy, interval, 100.0, 0.00001);
                speedZ = velocityZ.update(dz, interval, 100.0, 0.00001);
                speedPitch = velocityPitch.update(
                    sample.pitch - samplePitch,
                    interval,
                    2160.0,
                    0.002,
                    0.025
                );
                speedYaw = velocityYaw.update(
                    unwrappedYaw - sampleYaw,
                    interval,
                    2160.0,
                    0.002,
                    0.025
                );
            } else if (tickDiscontinuity ||
                sample.updatedAtMs - updatedAtMs > 500) {
                resetVelocities();
            }
            if (teleport) {
                resetLinearVelocity();
                renderX.reset(sample.x, 0.0);
                renderY.reset(sample.y, 0.0);
                renderZ.reset(sample.z, 0.0);
            }
            sampleX = sample.x;
            sampleY = sample.y;
            sampleZ = sample.z;
            samplePitch = sample.pitch;
            sampleYaw = unwrappedYaw;
        }
        inputTickKnown = sample.inputTickKnown;
        inputTick = sample.inputTick;
        updatedAtMs = sample.updatedAtMs;
        receivedNanos = nowNanos;
        sampleAgeNanos = Math.min(sample.ageMs, 500) * 1_000_000L;
    }

    State frame(long nowNanos, int configuredFov) {
        if (!known) return null;
        long receiptAge = Math.max(0, nowNanos - receivedNanos);
        long totalAge = Math.min(
            10_000_000_000L,
            sampleAgeNanos + receiptAge
        );
        double linearConfidence = Math.min(
            velocityX.confidence(),
            Math.min(velocityY.confidence(), velocityZ.confidence())
        );
        double angleConfidence = Math.min(
            velocityPitch.confidence(),
            velocityYaw.confidence()
        );
        double linearSpeed = Math.sqrt(
            speedX * speedX + speedY * speedY + speedZ * speedZ
        );
        double angleSpeed = Math.max(Math.abs(speedPitch), Math.abs(speedYaw));
        double motion = ProjectionMath.cameraMotionStrength(
            linearSpeed,
            angleSpeed
        );
        double medium = ProjectionMath.mediumMotionSoftness(motion);
        double predictionDamping = 1.0 - medium * 0.22;
        long linearLead = Math.round(
            RENDER_LEAD_NANOS * MotionSmoother.clamp(
                linearConfidence,
                0.0,
                1.0
            ) * predictionDamping
        );
        long angleLead = Math.round(
            RENDER_LEAD_NANOS * MotionSmoother.clamp(
                angleConfidence,
                0.0,
                1.0
            ) * predictionDamping
        );
        double linearPrediction = MotionSmoother.predictionSeconds(
            totalAge,
            linearLead,
            LINEAR_PREDICTION_NANOS,
            MAX_PREDICTION_NANOS
        ) * predictionDamping;
        double anglePrediction = MotionSmoother.predictionSeconds(
            totalAge,
            angleLead,
            LINEAR_PREDICTION_NANOS,
            MAX_PREDICTION_NANOS
        ) * predictionDamping;
        double linearVelocityScale = MotionSmoother.predictionVelocityScale(
            totalAge,
            linearLead,
            LINEAR_PREDICTION_NANOS,
            MAX_PREDICTION_NANOS
        );
        double angleVelocityScale = MotionSmoother.predictionVelocityScale(
            totalAge,
            angleLead,
            LINEAR_PREDICTION_NANOS,
            MAX_PREDICTION_NANOS
        );

        linearPrediction = boundedPredictionSeconds(
            linearPrediction,
            linearSpeed,
            0.85
        );
        anglePrediction = boundedPredictionSeconds(
            anglePrediction,
            angleSpeed,
            3.25
        );
        double targetX = sampleX + speedX * linearPrediction;
        double targetY = sampleY + speedY * linearPrediction;
        double targetZ = sampleZ + speedZ * linearPrediction;
        double targetPitch = MotionSmoother.clamp(
            samplePitch + speedPitch * anglePrediction,
            -90.0,
            90.0
        );
        double targetYaw = sampleYaw + speedYaw * anglePrediction;
        double targetVelocityX = speedX * linearVelocityScale;
        double targetVelocityY = speedY * linearVelocityScale;
        double targetVelocityZ = speedZ * linearVelocityScale;
        double targetVelocityPitch = speedPitch * angleVelocityScale;
        double targetVelocityYaw = speedYaw * angleVelocityScale;
        double targetFov = ProjectionMath.dynamicVerticalFov(
            configuredFov,
            linearSpeed
        );

        long frameGap = Math.max(0, nowNanos - lastFrameNanos);
        if (lastFrameNanos == 0 || frameGap > MAXIMUM_FRAME_GAP_NANOS) {
            renderX.reset(targetX, targetVelocityX);
            renderY.reset(targetY, targetVelocityY);
            renderZ.reset(targetZ, targetVelocityZ);
            renderPitch.reset(targetPitch, targetVelocityPitch);
            renderYaw.reset(targetYaw, targetVelocityYaw);
            renderedFov = targetFov;
        } else {
            double delta = frameGap / 1_000_000_000.0;
            double positionSmooth = 0.032 -
                MotionSmoother.clamp(linearSpeed / 30.0, 0.0, 1.0) * 0.014 +
                medium * 0.010 -
                ProjectionMath.smoothStep(0.72, 1.0, motion) * 0.004 +
                (1.0 - linearConfidence) * 0.012;
            double angleSmooth = 0.016 -
                MotionSmoother.clamp(angleSpeed / 720.0, 0.0, 1.0) * 0.008 +
                medium * 0.010 -
                ProjectionMath.smoothStep(0.72, 1.0, motion) * 0.004 +
                (1.0 - angleConfidence) * 0.010;
            renderX.step(targetX, targetVelocityX, positionSmooth, delta);
            renderY.step(targetY, targetVelocityY, positionSmooth, delta);
            renderZ.step(targetZ, targetVelocityZ, positionSmooth, delta);
            renderPitch.step(
                targetPitch,
                targetVelocityPitch,
                angleSmooth,
                delta
            );
            renderYaw.step(targetYaw, targetVelocityYaw, angleSmooth, delta);
            if (!Double.isFinite(renderedFov)) {
                renderedFov = targetFov;
            } else {
                double response = targetFov > renderedFov ? 0.12 : 0.24;
                renderedFov += (1.0 - Math.exp(-delta / response)) *
                    (targetFov - renderedFov);
            }
        }
        lastFrameNanos = nowNanos;
        boolean animating = linearVelocityScale > 0.0001 ||
            angleVelocityScale > 0.0001 ||
            !renderX.isSettled(targetX, targetVelocityX, 0.0001, 0.002) ||
            !renderY.isSettled(targetY, targetVelocityY, 0.0001, 0.002) ||
            !renderZ.isSettled(targetZ, targetVelocityZ, 0.0001, 0.002) ||
            !renderPitch.isSettled(
                targetPitch,
                targetVelocityPitch,
                0.001,
                0.01
            ) ||
            !renderYaw.isSettled(targetYaw, targetVelocityYaw, 0.001, 0.01);
        return new State(
            renderX.value(),
            renderY.value(),
            renderZ.value(),
            renderPitch.value(),
            renderYaw.value(),
            renderedFov,
            linearSpeed,
            angleSpeed,
            animating
        );
    }

    private void resetVelocities() {
        resetLinearVelocity();
        speedPitch = speedYaw = 0.0;
        velocityPitch.reset();
        velocityYaw.reset();
    }

    private void resetLinearVelocity() {
        speedX = speedY = speedZ = 0.0;
        velocityX.reset();
        velocityY.reset();
        velocityZ.reset();
    }

    private static double boundedPredictionSeconds(
        double seconds,
        double speed,
        double maximumDisplacement
    ) {
        if (speed <= 0.000001) return 0.0;
        return Math.min(seconds, maximumDisplacement / speed);
    }

    static final class State {
        final double x;
        final double y;
        final double z;
        final double pitch;
        final double yaw;
        final double verticalFov;
        final double positionSpeed;
        final double angleSpeed;
        final boolean animating;

        State(
            double x,
            double y,
            double z,
            double pitch,
            double yaw,
            double verticalFov,
            double positionSpeed,
            double angleSpeed,
            boolean animating
        ) {
            this.x = x;
            this.y = y;
            this.z = z;
            this.pitch = pitch;
            this.yaw = yaw;
            this.verticalFov = verticalFov;
            this.positionSpeed = positionSpeed;
            this.angleSpeed = angleSpeed;
            this.animating = animating;
        }
    }
}
