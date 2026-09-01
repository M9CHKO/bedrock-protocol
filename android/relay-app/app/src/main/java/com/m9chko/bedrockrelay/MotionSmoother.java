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

    /**
     * Tracks a sampled value with continuous position, velocity and
     * acceleration. Packet corrections change the requested jerk rather
     * than the rendered position, so they cannot create a one-frame snap.
     */
    static final class JerkAxis {
        private boolean initialized;
        private double value;
        private double velocity;
        private double acceleration;
        private double lastSample;
        private double sampleVelocity;
        private double stationarySeconds;

        void reset(double newValue, double newVelocity) {
            initialized = true;
            value = newValue;
            velocity = newVelocity;
            acceleration = 0.0;
            lastSample = newValue;
            sampleVelocity = newVelocity;
            stationarySeconds = 0.0;
        }

        void step(
            double sample,
            double deltaSeconds,
            double maximumVelocity,
            double maximumAcceleration,
            double sampleVelocityResponseSeconds,
            double correctionSeconds,
            double accelerationResponseSeconds,
            double jerkResponseSeconds
        ) {
            if (!initialized || !Double.isFinite(value) ||
                !Double.isFinite(velocity) ||
                !Double.isFinite(acceleration)) {
                reset(sample, 0.0);
                return;
            }
            if (!Double.isFinite(sample) || deltaSeconds <= 0.0) return;

            double step = clamp(deltaSeconds, 0.0001, 0.05);
            double sampleDelta = sample - lastSample;
            stationarySeconds = Math.abs(sampleDelta) < 0.001
                ? Math.min(1.0, stationarySeconds + step)
                : 0.0;
            double measuredVelocity = clamp(
                sampleDelta / step,
                -maximumVelocity,
                maximumVelocity
            );
            lastSample = sample;
            sampleVelocity = filterVelocity(
                sampleVelocity,
                measuredVelocity,
                step,
                sampleVelocityResponseSeconds
            );

            double desiredVelocity = sampleVelocity +
                (sample - value) / Math.max(0.008, correctionSeconds);
            desiredVelocity = clamp(
                desiredVelocity,
                -maximumVelocity,
                maximumVelocity
            );
            double desiredAcceleration = clamp(
                (desiredVelocity - velocity) /
                    Math.max(0.006, accelerationResponseSeconds),
                -maximumAcceleration,
                maximumAcceleration
            );
            acceleration = filterVelocity(
                acceleration,
                desiredAcceleration,
                step,
                jerkResponseSeconds
            );
            velocity = clamp(
                velocity + acceleration * step,
                -maximumVelocity,
                maximumVelocity
            );
            double nextValue = value + velocity * step;

            // A stationary target may be crossed only because the remaining
            // correction is below a pixel. Clamp that final crossing without
            // affecting a real direction change of a moving camera.
            if ((Math.abs(sampleVelocity) < 0.5 ||
                 stationarySeconds >= 0.035) &&
                (sample - value) * (sample - nextValue) < 0.0) {
                value = sample;
                velocity = 0.0;
                acceleration = 0.0;
                sampleVelocity = 0.0;
            } else {
                value = nextValue;
            }
        }

        double value() {
            return value;
        }

        double velocity() {
            return velocity;
        }

        double acceleration() {
            return acceleration;
        }

        boolean isSettled(double target, double valueEpsilon) {
            return initialized && Math.abs(value - target) <= valueEpsilon &&
                Math.abs(velocity) <= 0.5 &&
                Math.abs(acceleration) <= 8.0;
        }
    }

    /** Allocation-free final stabilizer for projected screen rectangles. */
    static final class ScreenBox {
        private static final long MaximumFrameGapNanos = 250_000_000L;

        private final JerkAxis centerX = new JerkAxis();
        private final JerkAxis centerY = new JerkAxis();
        private final JerkAxis width = new JerkAxis();
        private final JerkAxis height = new JerkAxis();
        private boolean initialized;
        private double lastCenterX;
        private double lastCenterY;
        private long lastFrameNanos;

        void hide() {
            initialized = false;
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
                lastCenterX = targetCenterX;
                lastCenterY = targetCenterY;
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
            double rawSpeed = Math.sqrt(
                (targetCenterX - lastCenterX) *
                    (targetCenterX - lastCenterX) +
                (targetCenterY - lastCenterY) *
                    (targetCenterY - lastCenterY)
            ) / deltaSeconds;
            lastCenterX = targetCenterX;
            lastCenterY = targetCenterY;

            // A moving camera needs little latency, while a nearly static
            // projection benefits from stronger packet-noise rejection.
            // All parameters vary continuously; there is deliberately no
            // hard deadband or hysteresis boundary.
            double motion = clamp(
                rawSpeed / (viewportSpan * 0.8),
                0.0,
                1.0
            );
            double sampleVelocityResponse = 0.075 - motion * 0.035;
            double correctionSeconds = 0.065 - motion * 0.025;
            double accelerationResponse = 0.040 - motion * 0.015;
            double jerkResponse = 0.045 - motion * 0.018;
            double maximumPositionVelocity = viewportSpan * 20.0;
            double maximumPositionAcceleration = viewportSpan * 180.0;
            centerX.step(
                targetCenterX,
                deltaSeconds,
                maximumPositionVelocity,
                maximumPositionAcceleration,
                sampleVelocityResponse,
                correctionSeconds,
                accelerationResponse,
                jerkResponse
            );
            centerY.step(
                targetCenterY,
                deltaSeconds,
                maximumPositionVelocity,
                maximumPositionAcceleration,
                sampleVelocityResponse,
                correctionSeconds,
                accelerationResponse,
                jerkResponse
            );

            double maximumSizeVelocity = viewportSpan * 8.0;
            double maximumSizeAcceleration = viewportSpan * 60.0;
            width.step(
                targetWidth,
                deltaSeconds,
                maximumSizeVelocity,
                maximumSizeAcceleration,
                0.110 - motion * 0.035,
                0.100 - motion * 0.025,
                0.055 - motion * 0.015,
                0.065 - motion * 0.020
            );
            height.step(
                targetHeight,
                deltaSeconds,
                maximumSizeVelocity,
                maximumSizeAcceleration,
                0.110 - motion * 0.035,
                0.100 - motion * 0.025,
                0.055 - motion * 0.015,
                0.065 - motion * 0.020
            );
            if (width.value() <= 0.0 || height.value() <= 0.0) {
                width.reset(targetWidth, 0.0);
                height.reset(targetHeight, 0.0);
            }
            lastFrameNanos = frameNanos;
            return !centerX.isSettled(targetCenterX, 0.08) ||
                !centerY.isSettled(targetCenterY, 0.08) ||
                !width.isSettled(targetWidth, 0.08) ||
                !height.isSettled(targetHeight, 0.08);
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
