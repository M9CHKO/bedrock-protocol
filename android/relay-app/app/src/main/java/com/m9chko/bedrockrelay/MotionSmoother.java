package com.m9chko.bedrockrelay;

/** Frame-rate-independent helpers for continuous overlay motion. */
final class MotionSmoother {
    private MotionSmoother() {}

    static final class Axis {
        private boolean initialized;
        private double value;
        private double velocity;

        void reset(double newValue, double newVelocity) {
            initialized = true;
            value = newValue;
            velocity = newVelocity;
        }

        double value() {
            return value;
        }

        double velocity() {
            return velocity;
        }

        void step(
            double targetValue,
            double targetVelocity,
            double smoothTimeSeconds,
            double deltaSeconds
        ) {
            if (!initialized || !Double.isFinite(value) ||
                !Double.isFinite(velocity)) {
                reset(targetValue, targetVelocity);
                return;
            }
            if (!Double.isFinite(targetValue) ||
                !Double.isFinite(targetVelocity) || deltaSeconds <= 0.0) {
                return;
            }

            double step = clamp(deltaSeconds, 0.0001, 0.05);
            double smoothTime = clamp(smoothTimeSeconds, 0.006, 0.5);
            double omega = 2.0 / smoothTime;
            double decay = Math.exp(-omega * step);
            // targetValue is the desired position at the end of this frame.
            // Reconstruct its start-of-frame position so a moving target
            // cannot pull the filtered value backwards for one frame.
            double targetAtFrameStart = targetValue - targetVelocity * step;
            double displacement = value - targetAtFrameStart;
            double relativeVelocity = velocity - targetVelocity;
            double temporary = (relativeVelocity +
                omega * displacement) * step;

            value = targetValue + (displacement + temporary) * decay;
            velocity = targetVelocity + (relativeVelocity -
                omega * temporary) * decay;
        }

        boolean isSettled(
            double targetValue,
            double targetVelocity,
            double valueEpsilon,
            double velocityEpsilon
        ) {
            return initialized &&
                Math.abs(value - targetValue) <= valueEpsilon &&
                Math.abs(velocity - targetVelocity) <= velocityEpsilon;
        }
    }

    static double filterVelocity(
        double current,
        double measured,
        double deltaSeconds,
        double responseSeconds
    ) {
        if (!Double.isFinite(measured)) return current;
        if (!Double.isFinite(current) || deltaSeconds <= 0.0) return measured;
        double response = Math.max(0.001, responseSeconds);
        double alpha = 1.0 - Math.exp(-deltaSeconds / response);
        return current + (measured - current) * clamp(alpha, 0.0, 1.0);
    }

    static double unwrapAngle(double previous, double wrapped) {
        return previous + angleDifference(previous, wrapped);
    }

    static double predictionSeconds(
        long sampleAgeNanos,
        long renderLeadNanos,
        long linearPredictionNanos,
        long maximumPredictionNanos
    ) {
        double requested = Math.max(0.0, sampleAgeNanos) +
            Math.max(0.0, renderLeadNanos);
        double maximum = Math.max(1.0, maximumPredictionNanos);
        double linear = clamp(linearPredictionNanos, 0.0, maximum);
        if (requested <= linear || linear >= maximum) {
            return Math.min(requested, maximum) / 1_000_000_000.0;
        }

        // Continue linearly for fresh samples, then approach the maximum
        // horizon asymptotically. The horizon never decreases, so a delayed
        // packet cannot make an outline travel forward and then reverse back
        // towards the same stale coordinate.
        double remaining = maximum - linear;
        double phase = (requested - linear) / remaining;
        double horizon = linear + remaining * (1.0 - Math.exp(-phase));
        return Math.min(horizon, maximum) / 1_000_000_000.0;
    }

    static double predictionVelocityScale(
        long sampleAgeNanos,
        long renderLeadNanos,
        long linearPredictionNanos,
        long maximumPredictionNanos
    ) {
        double requested = Math.max(0.0, sampleAgeNanos) +
            Math.max(0.0, renderLeadNanos);
        double maximum = Math.max(1.0, maximumPredictionNanos);
        double linear = clamp(linearPredictionNanos, 0.0, maximum);
        if (requested <= linear) return 1.0;
        if (linear >= maximum) return requested < maximum ? 1.0 : 0.0;
        double remaining = maximum - linear;
        return Math.exp(-(requested - linear) / remaining);
    }

    /** Allocation-free final stabilizer for projected screen rectangles. */
    static final class ScreenBox {
        private static final long MaximumFrameGapNanos = 250_000_000L;
        private static final double PositionVelocityResponseSeconds = 0.050;
        private static final double SizeVelocityResponseSeconds = 0.085;

        private final Axis centerX = new Axis();
        private final Axis centerY = new Axis();
        private final Axis width = new Axis();
        private final Axis height = new Axis();
        private boolean initialized;
        private double rawCenterX;
        private double rawCenterY;
        private double rawWidth;
        private double rawHeight;
        private double velocityCenterX;
        private double velocityCenterY;
        private double velocityWidth;
        private double velocityHeight;
        private long lastFrameNanos;

        void hide() {
            initialized = false;
            velocityCenterX = velocityCenterY = 0.0;
            velocityWidth = velocityHeight = 0.0;
            lastFrameNanos = 0;
        }

        boolean step(
            double left,
            double top,
            double right,
            double bottom,
            long frameNanos,
            double viewportWidth,
            double viewportHeight
        ) {
            double targetCenterX = (left + right) * 0.5;
            double targetCenterY = (top + bottom) * 0.5;
            double targetWidth = Math.max(1.0, right - left);
            double targetHeight = Math.max(1.0, bottom - top);
            if (!Double.isFinite(targetCenterX) ||
                !Double.isFinite(targetCenterY) ||
                !Double.isFinite(targetWidth) ||
                !Double.isFinite(targetHeight)) {
                hide();
                return false;
            }

            long frameGap = Math.max(0, frameNanos - lastFrameNanos);
            if (!initialized || lastFrameNanos == 0 ||
                frameGap > MaximumFrameGapNanos) {
                initialized = true;
                rawCenterX = targetCenterX;
                rawCenterY = targetCenterY;
                rawWidth = targetWidth;
                rawHeight = targetHeight;
                velocityCenterX = velocityCenterY = 0.0;
                velocityWidth = velocityHeight = 0.0;
                centerX.reset(targetCenterX, 0.0);
                centerY.reset(targetCenterY, 0.0);
                width.reset(targetWidth, 0.0);
                height.reset(targetHeight, 0.0);
                lastFrameNanos = frameNanos;
                return false;
            }

            double deltaSeconds = clamp(
                frameGap / 1_000_000_000.0,
                0.0001,
                0.05
            );
            double viewportSpan = Math.max(
                1.0,
                Math.max(viewportWidth, viewportHeight)
            );
            double positionDeadband = clamp(
                viewportSpan * 0.0035,
                1.5,
                7.0
            );
            double sizeDeadband = clamp(
                viewportSpan * 0.0012,
                0.75,
                2.5
            );
            targetCenterX = followOutsideDeadband(
                rawCenterX,
                targetCenterX,
                positionDeadband,
                deltaSeconds
            );
            targetCenterY = followOutsideDeadband(
                rawCenterY,
                targetCenterY,
                positionDeadband,
                deltaSeconds
            );
            targetWidth = followOutsideDeadband(
                rawWidth,
                targetWidth,
                sizeDeadband,
                deltaSeconds
            );
            targetHeight = followOutsideDeadband(
                rawHeight,
                targetHeight,
                sizeDeadband,
                deltaSeconds
            );
            double maximumPositionVelocity = viewportSpan * 12.0;
            double maximumSizeVelocity = viewportSpan * 5.0;
            velocityCenterX = filterVelocity(
                velocityCenterX,
                clamp(
                    (targetCenterX - rawCenterX) / deltaSeconds,
                    -maximumPositionVelocity,
                    maximumPositionVelocity
                ),
                deltaSeconds,
                PositionVelocityResponseSeconds
            );
            velocityCenterY = filterVelocity(
                velocityCenterY,
                clamp(
                    (targetCenterY - rawCenterY) / deltaSeconds,
                    -maximumPositionVelocity,
                    maximumPositionVelocity
                ),
                deltaSeconds,
                PositionVelocityResponseSeconds
            );
            velocityWidth = filterVelocity(
                velocityWidth,
                clamp(
                    (targetWidth - rawWidth) / deltaSeconds,
                    -maximumSizeVelocity,
                    maximumSizeVelocity
                ),
                deltaSeconds,
                SizeVelocityResponseSeconds
            );
            velocityHeight = filterVelocity(
                velocityHeight,
                clamp(
                    (targetHeight - rawHeight) / deltaSeconds,
                    -maximumSizeVelocity,
                    maximumSizeVelocity
                ),
                deltaSeconds,
                SizeVelocityResponseSeconds
            );
            rawCenterX = targetCenterX;
            rawCenterY = targetCenterY;
            rawWidth = targetWidth;
            rawHeight = targetHeight;

            double targetSpeed = Math.sqrt(
                velocityCenterX * velocityCenterX +
                    velocityCenterY * velocityCenterY
            );
            double targetError = Math.sqrt(
                (targetCenterX - centerX.value()) *
                    (targetCenterX - centerX.value()) +
                (targetCenterY - centerY.value()) *
                    (targetCenterY - centerY.value())
            );
            double motion = clamp(
                Math.max(
                    targetSpeed / (viewportSpan * 4.0),
                    targetError / (viewportSpan * 0.14)
                ),
                0.0,
                1.0
            );
            double positionSmoothTime = 0.052 - motion * 0.034;
            double sizeSmoothTime = 0.090 - motion * 0.040;
            double guidedCenterVelocityX = velocityTowardsTarget(
                centerX,
                targetCenterX,
                velocityCenterX
            );
            double guidedCenterVelocityY = velocityTowardsTarget(
                centerY,
                targetCenterY,
                velocityCenterY
            );
            double guidedWidthVelocity = velocityTowardsTarget(
                width,
                targetWidth,
                velocityWidth
            );
            double guidedHeightVelocity = velocityTowardsTarget(
                height,
                targetHeight,
                velocityHeight
            );
            stepWithoutOvershoot(
                centerX,
                targetCenterX,
                guidedCenterVelocityX,
                positionSmoothTime,
                deltaSeconds
            );
            stepWithoutOvershoot(
                centerY,
                targetCenterY,
                guidedCenterVelocityY,
                positionSmoothTime,
                deltaSeconds
            );
            stepWithoutOvershoot(
                width,
                targetWidth,
                guidedWidthVelocity,
                sizeSmoothTime,
                deltaSeconds
            );
            stepWithoutOvershoot(
                height,
                targetHeight,
                guidedHeightVelocity,
                sizeSmoothTime,
                deltaSeconds
            );
            if (width.value() <= 0.0 || height.value() <= 0.0) {
                width.reset(targetWidth, velocityWidth);
                height.reset(targetHeight, velocityHeight);
            }
            lastFrameNanos = frameNanos;
            return !centerX.isSettled(
                    targetCenterX,
                    velocityCenterX,
                    0.08,
                    0.5
                ) ||
                !centerY.isSettled(
                    targetCenterY,
                    velocityCenterY,
                    0.08,
                    0.5
                ) ||
                !width.isSettled(targetWidth, velocityWidth, 0.08, 0.5) ||
                !height.isSettled(targetHeight, velocityHeight, 0.08, 0.5);
        }

        private static void stepWithoutOvershoot(
            Axis axis,
            double targetValue,
            double targetVelocity,
            double smoothTimeSeconds,
            double deltaSeconds
        ) {
            double before = axis.value();
            axis.step(
                targetValue,
                targetVelocity,
                smoothTimeSeconds,
                deltaSeconds
            );
            double beforeError = targetValue - before;
            double afterError = targetValue - axis.value();
            if (beforeError != 0.0 && beforeError * afterError < 0.0) {
                axis.reset(targetValue, 0.0);
            }
        }

        private static double velocityTowardsTarget(
            Axis axis,
            double targetValue,
            double estimatedVelocity
        ) {
            double error = targetValue - axis.value();
            if (Math.abs(error) < 0.001 || error * estimatedVelocity <= 0.0) {
                return 0.0;
            }
            return estimatedVelocity;
        }

        private static double followOutsideDeadband(
            double current,
            double sample,
            double radius,
            double deltaSeconds
        ) {
            if (sample > current + radius) return sample - radius;
            if (sample < current - radius) return sample + radius;
            double settleAlpha = 1.0 - Math.exp(
                -Math.max(0.0, deltaSeconds) / 0.35
            );
            return current + (sample - current) * settleAlpha;
        }

        double left() {
            return centerX.value() - width.value() * 0.5;
        }

        double top() {
            return centerY.value() - height.value() * 0.5;
        }

        double right() {
            return centerX.value() + width.value() * 0.5;
        }

        double bottom() {
            return centerY.value() + height.value() * 0.5;
        }

        double centerX() {
            return centerX.value();
        }

        double centerY() {
            return centerY.value();
        }
    }

    static double angleDifference(double from, double to) {
        double difference = (to - from) % 360.0;
        if (difference > 180.0) difference -= 360.0;
        if (difference < -180.0) difference += 360.0;
        return difference;
    }

    static double clamp(double value, double minimum, double maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
