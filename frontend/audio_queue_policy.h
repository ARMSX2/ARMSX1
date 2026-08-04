#pragma once

#include <cstddef>

/*
 * Decide what an SDL audio callback may consume.
 *
 * A partial callback is worse than a clean re-prime: it plays the tail of a frame followed by
 * silence, advances the queue into the next sample, and then repeats that torn boundary every
 * callback while the producer and device remain phase-locked. Preserve the short tail instead,
 * output one already-zeroed callback, and ask the emulation thread to pause the device until the
 * normal startup prebuffer has been rebuilt. SDL_PauseAudioDevice() is deliberately not called
 * from the callback itself.
 */
struct ArmsxAudioQueueReadDecision {
    std::size_t copy_bytes;
    bool request_rebuffer;
};

constexpr ArmsxAudioQueueReadDecision armsx_audio_queue_read_decision(
    std::size_t available_bytes,
    std::size_t requested_bytes,
    bool rebuffer_pending) {
    if (requested_bytes == 0) {
        return {0, false};
    }

    if (rebuffer_pending || available_bytes < requested_bytes) {
        return {0, true};
    }

    return {requested_bytes, false};
}

/*
 * Ratio for the normal-speed host elasticity path.
 *
 * The emulated SPU clock remains authoritative: input_samples is exactly what the PS1 produced.
 * elapsed_seconds is how much real output time those samples have to cover.  A device which is
 * briefly slower than realtime therefore gets a longer host stream instead of a torn callback
 * followed by silence.  The small queue correction restores the jitter cushion over four
 * measurement windows without making one scheduler hiccup become an audible pitch jump.
 *
 * This is deliberately a pure policy function so every platform uses identical arithmetic and
 * the edge cases are covered without an audio device.
 */
constexpr double armsx_audio_realtime_ratio(
    std::size_t input_samples,
    double elapsed_seconds,
    int mix_rate,
    std::size_t queued_samples,
    std::size_t target_queue_samples) {
    if (input_samples == 0 || !(elapsed_seconds > 0.0) || mix_rate <= 0) {
        return 1.0;
    }

    double desired_output_samples = elapsed_seconds * static_cast<double>(mix_rate);
    const double queue_error = static_cast<double>(target_queue_samples) -
                               static_cast<double>(queued_samples);
    desired_output_samples += queue_error * 0.25;

    // Normal play may stretch as far as 2x when a thermally-limited host reaches 50%, but it
    // may speed up by at most 5% while draining excess cushion. A catch-up frame must not turn
    // into a conspicuous high-pitch burst.
    const double minimum_output = static_cast<double>(input_samples) / 1.05;
    const double maximum_output = static_cast<double>(input_samples) * 2.0;
    if (desired_output_samples < minimum_output) {
        desired_output_samples = minimum_output;
    } else if (desired_output_samples > maximum_output) {
        desired_output_samples = maximum_output;
    }

    const double ratio = static_cast<double>(input_samples) / desired_output_samples;
    return ratio < 0.5 ? 0.5 : (ratio > 1.05 ? 1.05 : ratio);
}
