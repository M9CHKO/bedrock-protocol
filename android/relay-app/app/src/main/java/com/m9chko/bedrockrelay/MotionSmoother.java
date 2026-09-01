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

    /**
     * Chooses the time represented by two packet samples. PlayerAuthInput's
     * client tick is preferred because Android/native delivery jitter must
     * not be mistaken for a change in camera speed. Bedrock runs game ticks
     * at 20 Hz. Native monotonic timestamps remain a guarded fallback for
     * legacy movement packets and protocol versions without a decoded tick.
     */
    static double packetIntervalSeconds(
        boolean previousTickKnown,
        long previousTick,
        boolean currentTickKnown,
        long currentTick,
        long previousUpdatedAtMs,
        long currentUpdatedAtMs
    ) {
        if (previousTickKnown && currentTickKnown) {
            if (currentTick > previousTick) {
                long tickDelta = currentTick - previousTick;
                if (tickDelta <= 20) return tickDelta * 0.05;
            }
            // Do not silently replace an invalid decoded tick sequence with
            // delivery timing. That would reintroduce the exact jitter this
            // method is meant to remove.
            return Double.NaN;
        }

        long elapsedMs = currentUpdatedAtMs - previousUpdatedAtMs;
        if (previousUpdatedAtMs > 0 && currentUpdatedAtMs > 0 &&
            elapsedMs >= 5 && elapsedMs <= 500) {
            return elapsedMs / 1000.0;
        }
        return Double.NaN;
    }

    /**
     * Robust velocity estimate for packet-spaced motion.
     *
     * It reacts immediately to a true stop or direction change, while a
     * three-slope median clips a single noisy acceleration sample. The
     * estimate has no independent position state, so it cannot ring or keep
     * moving an outline after the newest packets say that motion stopped.
     */
    static final class PacketVelocity {
        private boolean initialized;
        private boolean stationary = true;
        private double velocity;
        private double newestSlope;
        private double olderSlope;
        private int slopeCount;
        private double confidence = 1.0;

        void reset() {
            initialized = false;
            stationary = true;
            velocity = 0.0;
            newestSlope = 0.0;
            olderSlope = 0.0;
            slopeCount = 0;
            confidence = 1.0;
        }

        double velocity() {
            return velocity;
        }

        double confidence() {
            return confidence;
        }

        double update(
            double delta,
            double intervalSeconds,
            double maximumVelocity,
            double zeroDelta
        ) {
            if (!Double.isFinite(delta) ||
                !Double.isFinite(intervalSeconds) ||
                intervalSeconds <= 0.0) {
                return velocity;
            }

            double maximum = Math.max(0.001, maximumVelocity);
            double epsilon = Math.max(0.0, zeroDelta);
            if (Math.abs(delta) <= epsilon) {
                // One complete client tick with no displacement is stronger
                // evidence than an old filtered derivative. Kill it now so
                // the projection cannot overshoot and then travel backwards.
                initialized = true;
                stationary = true;
                velocity = 0.0;
                newestSlope = 0.0;
                olderSlope = 0.0;
                slopeCount = 0;
                confidence = 1.0;
                return velocity;
            }

            double measured = clamp(
                delta / intervalSeconds,
                -maximum,
                maximum
            );
            if (!initialized || stationary) {
                initialized = true;
                stationary = false;
                velocity = measured;
                newestSlope = measured;
                olderSlope = measured;
                slopeCount = 1;
                confidence = 0.55;
                return velocity;
            }

            boolean reversed = measured * velocity < 0.0 ||
                (slopeCount > 0 && measured * newestSlope < 0.0);
            if (reversed) {
                // Do not average opposite directions: that is precisely what
                // creates a visible forward/backward wobble after a turn.
                velocity = measured;
                newestSlope = measured;
                olderSlope = measured;
                slopeCount = 1;
                confidence = 0.35;
                return velocity;
            }

            double previousSlope = newestSlope;
            double candidate = measured;
            if (slopeCount >= 2) {
                double median = median(measured, newestSlope, olderSlope);
                double allowedDeviation = Math.max(
                    maximum * 0.002,
                    Math.abs(median) * 0.65
                );
                candidate = median + clamp(
                    measured - median,
                    -allowedDeviation,
                    allowedDeviation
                );
            } else if (slopeCount == 1) {
                candidate = measured * 0.72 + newestSlope * 0.28;
            }

            double measuredMagnitude = Math.abs(measured);
            double currentMagnitude = Math.abs(velocity);
            double responseSeconds;
            if (measuredMagnitude < currentMagnitude * 0.78) {
                // Braking must beat smoothing or prediction flies past the
                // newest camera sample and has to visibly correct backwards.
                candidate = measured;
                responseSeconds = 0.010;
            } else if (measuredMagnitude > currentMagnitude * 1.35) {
                responseSeconds = 0.028;
            } else {
                responseSeconds = 0.055;
            }
            velocity = filterVelocity(
                velocity,
                candidate,
                intervalSeconds,
                responseSeconds
            );
            velocity = clamp(velocity, -maximum, maximum);

            double agreementScale = Math.max(
                maximum * 0.002,
                Math.max(Math.abs(measured), Math.abs(previousSlope))
            );
            double agreement = 1.0 - clamp(
                Math.abs(measured - previousSlope) / agreementScale,
                0.0,
                1.0
            );
            confidence = filterVelocity(
                confidence,
                agreement,
                intervalSeconds,
                0.12
            );

            olderSlope = newestSlope;
            newestSlope = measured;
            slopeCount = Math.min(3, slopeCount + 1);
            return velocity;
        }
    }

    static double unwrapAngle(double previous, double wrapped) {
        return previous + angleDifference(previous, wrapped);
    }

    private static double median(double first, double second, double third) {
        return first + second + third - Math.min(
            first,
            Math.min(second, third)
        ) - Math.max(first, Math.max(second, third));
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
     * Stable adaptive low-pass for a related pair of values. Both values use
     * one response coefficient, so a projected centre follows one coherent
     * 2D path and a rectangle's width/height cannot acquire separate inertia.
     *
     * The derivative only changes the cutoff. The rendered value is always a
     * convex interpolation towards the current sample; it therefore cannot
     * continue along an old trajectory or overshoot a stationary target.
     */
    static final class AdaptivePair {
        private boolean initialized;
        private double first;
        private double second;
        private double lastSampleFirst;
        private double lastSampleSecond;
        private double derivativeFirst;
        private double derivativeSecond;

        void reset(double newFirst, double newSecond) {
            initialized = true;
            first = newFirst;
            second = newSecond;
            lastSampleFirst = newFirst;
            lastSampleSecond = newSecond;
            derivativeFirst = 0.0;
            derivativeSecond = 0.0;
        }

        void step(
            double sampleFirst,
            double sampleSecond,
            double deltaSeconds,
            double minimumCutoffHz,
            double speedCutoffSlope,
            double derivativeCutoffHz,
            double maximumCutoffHz
        ) {
            if (!initialized || !Double.isFinite(first) ||
                !Double.isFinite(second) ||
                !Double.isFinite(derivativeFirst) ||
                !Double.isFinite(derivativeSecond)) {
                reset(sampleFirst, sampleSecond);
                return;
            }
            if (!Double.isFinite(sampleFirst) ||
                !Double.isFinite(sampleSecond) || deltaSeconds <= 0.0) {
                return;
            }

            double step = clamp(deltaSeconds, 0.0001, 0.05);
            double rawDerivativeFirst =
                (sampleFirst - lastSampleFirst) / step;
            double rawDerivativeSecond =
                (sampleSecond - lastSampleSecond) / step;
            lastSampleFirst = sampleFirst;
            lastSampleSecond = sampleSecond;

            double derivativeAlpha = lowPassAlpha(
                derivativeCutoffHz,
                step
            );
            derivativeFirst += derivativeAlpha *
                (rawDerivativeFirst - derivativeFirst);
            derivativeSecond += derivativeAlpha *
                (rawDerivativeSecond - derivativeSecond);
            double speed = Math.hypot(derivativeFirst, derivativeSecond);
            double cutoff = clamp(
                minimumCutoffHz + speedCutoffSlope * speed,
                minimumCutoffHz,
                maximumCutoffHz
            );
            double alpha = lowPassAlpha(cutoff, step);
            first += alpha * (sampleFirst - first);
            second += alpha * (sampleSecond - second);
        }

        double first() {
            return first;
        }

        double second() {
            return second;
        }

        boolean isSettled(
            double targetFirst,
            double targetSecond,
            double valueEpsilon
        ) {
            return initialized &&
                Math.abs(first - targetFirst) <= valueEpsilon &&
                Math.abs(second - targetSecond) <= valueEpsilon;
        }
    }

    /** Allocation-free final stabilizer for projected screen rectangles. */
    static final class ScreenBox {
        private static final long MaximumFrameGapNanos = 250_000_000L;

        private static final double CenterMinimumCutoffHz = 1.2;
        private static final double CenterDerivativeCutoffHz = 0.5;
        private static final double CenterMaximumCutoffHz = 12.0;
        private static final double CenterRelativeSpeedGain = 14.0;
        private static final double SizeMinimumCutoffHz = 0.9;
        private static final double SizeDerivativeCutoffHz = 0.5;
        private static final double SizeMaximumCutoffHz = 8.0;
        private static final double SizeRelativeSpeedGain = 9.0;

        private final AdaptivePair center = new AdaptivePair();
        private final AdaptivePair size = new AdaptivePair();
        private boolean initialized;
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
            return step(
                left,
                top,
                right,
                bottom,
                frameNanos,
                viewportWidth,
                viewportHeight,
                false
            );
        }

        boolean step(
            double left,
            double top,
            double right,
            double bottom,
            long frameNanos,
            double viewportWidth,
            double viewportHeight,
            boolean cameraProjectionActive
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
                cameraProjectionActive ||
                frameGap > MaximumFrameGapNanos) {
                initialized = true;
                // Camera motion is already predicted and smoothed once for
                // the whole overlay. Applying a second per-entity screen
                // filter here makes every box trail the Minecraft image.
                // Follow the shared projection directly while that global
                // camera transform is active. World-space entity smoothing
                // still keeps independently moving mobs continuous.
                center.reset(targetCenterX, targetCenterY);
                size.reset(targetWidth, targetHeight);
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
            center.step(
                targetCenterX,
                targetCenterY,
                deltaSeconds,
                CenterMinimumCutoffHz,
                CenterRelativeSpeedGain / viewportSpan,
                CenterDerivativeCutoffHz,
                CenterMaximumCutoffHz
            );
            size.step(
                targetWidth,
                targetHeight,
                deltaSeconds,
                SizeMinimumCutoffHz,
                SizeRelativeSpeedGain / viewportSpan,
                SizeDerivativeCutoffHz,
                SizeMaximumCutoffHz
            );
            if (size.first() <= 0.0 || size.second() <= 0.0) {
                size.reset(targetWidth, targetHeight);
            }
            lastFrameNanos = frameNanos;
            return !center.isSettled(targetCenterX, targetCenterY, 0.08) ||
                !size.isSettled(targetWidth, targetHeight, 0.08);
        }

        double left() {
            return center.first() - size.first() * 0.5;
        }

        double top() {
            return center.second() - size.second() * 0.5;
        }

        double right() {
            return center.first() + size.first() * 0.5;
        }

        double bottom() {
            return center.second() + size.second() * 0.5;
        }

        double centerX() {
            return center.first();
        }

        double centerY() {
            return center.second();
        }
    }

    private static double lowPassAlpha(
        double cutoffHz,
        double deltaSeconds
    ) {
        double cutoff = Math.max(0.001, cutoffHz);
        double step = Math.max(0.0, deltaSeconds);
        return clamp(1.0 - Math.exp(-2.0 * Math.PI * cutoff * step), 0.0, 1.0);
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
