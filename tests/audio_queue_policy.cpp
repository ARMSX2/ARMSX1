#include "frontend/audio_queue_policy.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    if (condition) {
        std::printf("AUDIO_QUEUE passed case=%s\n", name);
        return;
    }

    std::fprintf(stderr, "AUDIO_QUEUE failed case=%s\n", name);
    failures++;
}

void check_read(const char* name,
                ArmsxAudioQueueReadDecision actual,
                std::size_t expected_copy,
                bool expected_rebuffer) {
    check(name, actual.copy_bytes == expected_copy &&
                    actual.request_rebuffer == expected_rebuffer);
}

}  // namespace

int main() {
    check_read("full-callback",
               armsx_audio_queue_read_decision(4096, 3528, false), 3528, false);
    check_read("short-tail",
               armsx_audio_queue_read_decision(1203, 3528, false), 1200, true);
    check_read("pending-rebuffer",
               armsx_audio_queue_read_decision(7056, 3528, true), 0, true);
    check_read("zero-byte-callback",
               armsx_audio_queue_read_decision(0, 0, false), 0, false);

    {
        constexpr auto write = armsx_audio_queue_write_decision(87500, 3072, 88200);
        check("overflow-discards-exact-excess",
              write.discard_queued_bytes == 2372 &&
                  write.copy_incoming_bytes == 3072 &&
                  87500 - write.discard_queued_bytes + write.copy_incoming_bytes == 88200);
    }
    {
        constexpr auto write = armsx_audio_queue_write_decision(12003, 100003, 88203);
        check("oversized-write-is-aligned-and-bounded",
              write.discard_queued_bytes == 12000 &&
                  write.copy_incoming_bytes == 88200);
    }

    constexpr std::size_t input_samples = 2976;
    constexpr int mix_rate = 44100;
    constexpr std::size_t target_queue = 5292;
    constexpr double window = static_cast<double>(input_samples) / mix_rate;
    constexpr ArmsxAudioDrcState neutral{1.0, 1.0, 1.0, false};

    {
        const auto state = armsx_audio_drc_step(
            neutral, input_samples, window * 1.20, mix_rate,
            target_queue - 600, target_queue);
        check("transient-stays-in-normal-band",
              !state.deficit &&
                  state.ratio >= 1.0 - kArmsxDrcMaxSkew &&
                  state.ratio <= 1.0 + kArmsxDrcMaxSkew);
    }
    {
        ArmsxAudioDrcState state{0.645, 0.70, 0.66, true};
        state = armsx_audio_drc_step(
            state, input_samples, window, mix_rate,
            target_queue * kArmsxDrcHighWaterTargets + 1, target_queue);
        check("high-water-clears-stale-deficit",
              !state.deficit && state.deficit_base == 1.0 &&
                  state.ratio > 0.82 && state.ratio < 0.84);

        for (int i = 0; i < 5; ++i) {
            state = armsx_audio_drc_step(
                state, input_samples, window, mix_rate,
                target_queue * kArmsxDrcHighWaterTargets + 1, target_queue);
        }
        check("high-water-recovers-before-overflow", state.ratio > 0.99);
    }
    {
        constexpr double host_speed = 0.55;
        ArmsxAudioDrcState state = neutral;
        double queue = target_queue;
        int underruns = 0;

        for (int i = 0; i < 100; ++i) {
            queue += static_cast<double>(input_samples) / state.ratio -
                     (window / host_speed) * mix_rate;
            if (queue <= 0.0) {
                underruns++;
                queue = target_queue;
                state = armsx_audio_drc_after_rebuffer(state);
            }
            state = armsx_audio_drc_step(
                state, input_samples, window / host_speed, mix_rate,
                static_cast<std::size_t>(queue), target_queue);
        }

        check("sustained-deficit-converges",
              state.deficit && state.ratio > 0.50 && state.ratio < 0.62);
        check("sustained-deficit-bounds-underruns", underruns <= 4);
    }
    {
        const auto state = armsx_audio_drc_after_rebuffer({0.6, 0.6, 0.6, true});
        check("rebuffer-preserves-learned-deficit",
              state.deficit && std::fabs(state.ratio - 0.6) < 0.0001);
    }

    if (failures != 0) {
        std::fprintf(stderr, "AUDIO_QUEUE failures=%d\n", failures);
        return 1;
    }

    std::printf("AUDIO_QUEUE all cases passed\n");
    return 0;
}
