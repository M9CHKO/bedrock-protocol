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
        long maximumPredictionNanos,
        long decayNanos
    ) {
        double requested = Math.max(0.0, sampleAgeNanos) +
            Math.max(0.0, renderLeadNanos);
        double maximum = Math.max(1.0, maximumPredictionNanos);
        if (requested <= maximum) return requested / 1_000_000_000.0;
        double decay = Math.max(1.0, decayNanos);
        double phase = clamp((requested - maximum) / decay, 0.0, 1.0);
        if (phase >= 1.0) return 0.0;

        // Cubic Hermite continuation. Position and velocity are continuous at
        // the prediction limit; stale velocity then returns smoothly to the
        // last authoritative sample instead of leaving the box ahead forever.
        double phaseSquared = phase * phase;
        double phaseCubed = phaseSquared * phase;
        double basisPosition = 2.0 * phaseCubed -
            3.0 * phaseSquared + 1.0;
        double basisTangent = phaseCubed - 2.0 * phaseSquared + phase;
        double initialTangent = decay / maximum;
        double horizon = maximum *
            (basisPosition + basisTangent * initialTangent);
        return Math.max(0.0, horizon) / 1_000_000_000.0;
    }

    static double predictionVelocityScale(
        long sampleAgeNanos,
        long renderLeadNanos,
        long maximumPredictionNanos,
        long decayNanos
    ) {
        double requested = Math.max(0.0, sampleAgeNanos) +
            Math.max(0.0, renderLeadNanos);
        double maximum = Math.max(1.0, maximumPredictionNanos);
        if (requested <= maximum) return 1.0;
        double decay = Math.max(1.0, decayNanos);
        double phase = clamp((requested - maximum) / decay, 0.0, 1.0);
        if (phase >= 1.0) return 0.0;

        double phaseSquared = phase * phase;
        double initialTangent = decay / maximum;
        double derivativeByPhase = 6.0 * phaseSquared - 6.0 * phase +
            (3.0 * phaseSquared - 4.0 * phase + 1.0) * initialTangent;
        return maximum / decay * derivativeByPhase;
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
