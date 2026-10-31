#pragma once

#include <cstddef>

/* Consume complete stereo frames and request a rebuffer after a short read. */
struct ArmsxAudioQueueReadDecision {
    std::size_t copy_bytes;
    bool request_rebuffer;
};

/* Bound writes to complete stereo frames without clearing the queue. */
struct ArmsxAudioQueueWriteDecision {
    std::size_t discard_queued_bytes;
    std::size_t copy_incoming_bytes;
};

constexpr ArmsxAudioQueueWriteDecision armsx_audio_queue_write_decision(
    std::size_t queued_bytes,
    std::size_t incoming_bytes,
    std::size_t maximum_bytes) {
    constexpr std::size_t kStereoFrameMask = ~std::size_t{3};
    const std::size_t queued = queued_bytes & kStereoFrameMask;
    const std::size_t maximum = maximum_bytes & kStereoFrameMask;
    const std::size_t incoming = incoming_bytes & kStereoFrameMask;
    const std::size_t copy = incoming < maximum ? incoming : maximum;
    const std::size_t room_for_queue = maximum - copy;
    const std::size_t discard = queued > room_for_queue ? queued - room_for_queue : 0;
    return {discard, copy};
}

constexpr ArmsxAudioQueueReadDecision armsx_audio_queue_read_decision(
    std::size_t available_bytes,
    std::size_t requested_bytes,
    bool rebuffer_pending) {
    if (requested_bytes == 0) {
        return {0, false};
    }

    if (rebuffer_pending) {
        return {0, true};
    }

    if (available_bytes < requested_bytes) {
        // AUDIO_S16SYS stereo uses four bytes per frame.
        return {available_bytes & ~std::size_t{3}, true};
    }

    return {requested_bytes, false};
}

/*
 * Dynamic rate control for normal-speed playback. Queue depth absorbs short timing jitter;
 * sustained deficits use a smoothed, slew-limited stretch ratio.
 */
struct ArmsxAudioDrcState {
    double ratio;         // input frames per output frame, 1.0 = neutral duration
    double speed_est;     // smoothed guest/host speed, 1.0 = realtime
    double deficit_base;  // integrator: the stretch the queue actually demands, 1.0 = none
    bool deficit;         // sustained-slowdown regime engaged
};

inline constexpr double kArmsxDrcMaxSkew = 0.005;
inline constexpr double kArmsxDrcSpeedEmaAlpha = 0.08;
inline constexpr double kArmsxDrcSpeedEmaAlphaLowWater = 0.5;
// Hysteresis for entering and leaving sustained-deficit mode.
inline constexpr double kArmsxDrcDeficitEngageSpeed = 0.995;
inline constexpr double kArmsxDrcDeficitExitSpeed = 0.998;
inline constexpr double kArmsxDrcSlewNormal = 0.002;
inline constexpr double kArmsxDrcSlewDeficit = 0.005;
inline constexpr double kArmsxDrcSlewHighWater = 0.02;
inline constexpr double kArmsxDrcSlewLowWater = 0.20;
inline constexpr double kArmsxDrcDeficitIntegral = 0.01;
// Prevent a non-neutral ratio from becoming a zero-error equilibrium.
inline constexpr double kArmsxDrcDeficitLeak = 0.001;
inline constexpr double kArmsxDrcDeficitStarvedBoost = 10.0;
// Preserve a small refill margin while limiting integrator windup.
inline constexpr double kArmsxDrcDeficitRefillMargin = 0.02;
inline constexpr double kArmsxDrcDeficitFloor = 0.5;
// Discard a stale deficit estimate when queued latency exceeds this multiple of the target.
inline constexpr std::size_t kArmsxDrcHighWaterTargets = 2;

/* Preserve the learned deficit across a rebuffer. */
constexpr ArmsxAudioDrcState armsx_audio_drc_after_rebuffer(ArmsxAudioDrcState state) {
    if (!state.deficit) {
        state.ratio = 1.0;
        return state;
    }

    // An underrun indicates that the learned stretch may be too shallow.
    const double measured = state.speed_est < kArmsxDrcDeficitFloor
        ? kArmsxDrcDeficitFloor
        : (state.speed_est > 1.0 ? 1.0 : state.speed_est);
    if (measured < state.deficit_base) {
        const double floor = state.deficit_base - kArmsxDrcSlewLowWater;
        state.deficit_base = measured < floor ? floor : measured;
    }
    const double ratio_floor = state.ratio - kArmsxDrcSlewLowWater;
    state.ratio = state.deficit_base < ratio_floor ? ratio_floor : state.deficit_base;
    return state;
}

/* Decay learned state toward realtime after a wall-clock discontinuity. */
constexpr ArmsxAudioDrcState armsx_audio_drc_after_discontinuity(ArmsxAudioDrcState state) {
    state.speed_est = (1.0 + state.speed_est) * 0.5;
    state.deficit_base = (1.0 + state.deficit_base) * 0.5;
    state.deficit = state.deficit && state.speed_est < kArmsxDrcDeficitExitSpeed;
    return armsx_audio_drc_after_rebuffer(state);
}

constexpr ArmsxAudioDrcState armsx_audio_drc_step(
    ArmsxAudioDrcState state,
    std::size_t window_input_samples,
    double window_elapsed_seconds,
    int mix_rate,
    std::size_t queued_samples,
    std::size_t target_queue_samples) {
    if (window_input_samples == 0 || !(window_elapsed_seconds > 0.0) || mix_rate <= 0 ||
        target_queue_samples == 0) {
        return state;
    }

    const double previous_deficit_base = state.deficit_base;
    double instant_speed = (static_cast<double>(window_input_samples) /
                            static_cast<double>(mix_rate)) /
                           window_elapsed_seconds;
    instant_speed = instant_speed < 0.25 ? 0.25 : (instant_speed > 4.0 ? 4.0 : instant_speed);
    const bool low_water = queued_samples < target_queue_samples / 2;
    const double alpha = low_water
        ? kArmsxDrcSpeedEmaAlphaLowWater
        : kArmsxDrcSpeedEmaAlpha;
    state.speed_est += alpha * (instant_speed - state.speed_est);

    const double fill_error =
        (static_cast<double>(queued_samples) - static_cast<double>(target_queue_samples)) /
        static_cast<double>(target_queue_samples);
    const double saturated_fill = fill_error < -1.0 ? -1.0 : (fill_error > 1.0 ? 1.0 : fill_error);
    const bool overfull = queued_samples / target_queue_samples >= kArmsxDrcHighWaterTargets;

    if (overfull && state.deficit) {
        state.deficit = false;
        state.deficit_base = 1.0;
    }

    bool entered_deficit = false;
    if (state.deficit) {
        if (state.speed_est > kArmsxDrcDeficitExitSpeed || state.deficit_base >= 1.0) {
            state.deficit = false;
        }
    } else if (state.speed_est < kArmsxDrcDeficitEngageSpeed && low_water) {
        state.deficit = true;
        entered_deficit = true;
        const double measured = state.speed_est < kArmsxDrcDeficitFloor
            ? kArmsxDrcDeficitFloor
            : (state.speed_est > 1.0 ? 1.0 : state.speed_est);
        const double floor = state.ratio - kArmsxDrcSlewLowWater;
        state.deficit_base = measured < floor ? floor : measured;
    }

    double base = 1.0;
    if (state.deficit) {
        const double measured_gap = state.deficit_base - state.speed_est;
        if (low_water && !entered_deficit && measured_gap > 0.0) {
            state.deficit_base -= measured_gap < kArmsxDrcSlewLowWater
                ? measured_gap
                : kArmsxDrcSlewLowWater;
        }
        const double integral = queued_samples < target_queue_samples / 8
            ? kArmsxDrcDeficitIntegral * kArmsxDrcDeficitStarvedBoost
            : kArmsxDrcDeficitIntegral;
        state.deficit_base += integral * saturated_fill +
                              kArmsxDrcDeficitLeak * (1.0 - state.deficit_base);
        // Do not stretch materially below measured speed.
        const double useful_floor = state.speed_est * (1.0 - kArmsxDrcDeficitRefillMargin);
        double lower = useful_floor > kArmsxDrcDeficitFloor ? useful_floor : kArmsxDrcDeficitFloor;
        if (lower > 1.0) lower = 1.0;
        if (state.deficit_base < lower) state.deficit_base = lower;
        const double slew_floor = previous_deficit_base - kArmsxDrcSlewLowWater;
        if (state.deficit_base < slew_floor) state.deficit_base = slew_floor;
        if (state.deficit_base > 1.0) state.deficit_base = 1.0;
        base = state.deficit_base;
    }

    double wanted = base * (1.0 + kArmsxDrcMaxSkew * saturated_fill);

    // Low water needs immediate downward correction; other changes remain slew-limited.
    const bool high_water_recovery = overfull && wanted > state.ratio;
    const double gap = wanted > state.ratio ? wanted - state.ratio : state.ratio - wanted;
    const double gap_half = gap * 0.5;
    const double slew = low_water && wanted < state.ratio
        ? (gap < kArmsxDrcSlewLowWater ? gap : kArmsxDrcSlewLowWater)
        : (high_water_recovery
            ? (gap_half > kArmsxDrcSlewHighWater ? gap_half : kArmsxDrcSlewHighWater)
            : (state.deficit ? kArmsxDrcSlewDeficit : kArmsxDrcSlewNormal));
    const double step = wanted - state.ratio;
    state.ratio += step < -slew ? -slew : (step > slew ? slew : step);

    const double upper = 1.0 + kArmsxDrcMaxSkew;
    state.ratio = state.ratio < 0.5 ? 0.5 : (state.ratio > upper ? upper : state.ratio);

    return state;
}
