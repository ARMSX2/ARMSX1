#include "frontend/audio_queue_policy.h"

#include <cstdio>

namespace {

int failures = 0;

void check(const char* name,
           ArmsxAudioQueueReadDecision actual,
           std::size_t expected_copy,
           bool expected_rebuffer) {
    if (actual.copy_bytes != expected_copy || actual.request_rebuffer != expected_rebuffer) {
        std::fprintf(stderr,
                     "AUDIO_QUEUE failed case=%s got=copy:%zu,rebuffer:%d expected=copy:%zu,rebuffer:%d\n",
                     name,
                     actual.copy_bytes,
                     actual.request_rebuffer ? 1 : 0,
                     expected_copy,
                     expected_rebuffer ? 1 : 0);
        failures++;
        return;
    }

    std::printf("AUDIO_QUEUE passed case=%s\n", name);
}

void check_ratio(const char* name, double actual, double expected, double tolerance = 0.0001) {
    const double delta = actual > expected ? actual - expected : expected - actual;
    if (delta > tolerance) {
        std::fprintf(stderr,
                     "AUDIO_QUEUE failed case=%s got=ratio:%.6f expected=%.6f\n",
                     name,
                     actual,
                     expected);
        failures++;
        return;
    }

    std::printf("AUDIO_QUEUE passed case=%s\n", name);
}

}  // namespace

int main() {
    check("full-callback", armsx_audio_queue_read_decision(4096, 3528, false), 3528, false);
    check("exact-callback", armsx_audio_queue_read_decision(3528, 3528, false), 3528, false);
    check("short-tail-is-preserved", armsx_audio_queue_read_decision(1200, 3528, false), 0, true);
    check("empty-queue-requests-rebuffer", armsx_audio_queue_read_decision(0, 3528, false), 0, true);
    check("pending-rebuffer-does-not-race", armsx_audio_queue_read_decision(7056, 3528, true), 0, true);
    check("zero-byte-callback-is-a-no-op", armsx_audio_queue_read_decision(0, 0, false), 0, false);

    constexpr std::size_t four_ntsc_frames = 2976;
    constexpr int mix_rate = 44100;
    constexpr std::size_t target_queue = 5292;
    check_ratio("realtime-ratio-at-speed",
                armsx_audio_realtime_ratio(four_ntsc_frames,
                                           static_cast<double>(four_ntsc_frames) / mix_rate,
                                           mix_rate, target_queue, target_queue),
                1.0);
    check_ratio("realtime-ratio-at-75-percent",
                armsx_audio_realtime_ratio(four_ntsc_frames, 0.09, mix_rate,
                                           target_queue, target_queue),
                static_cast<double>(four_ntsc_frames) / (0.09 * mix_rate));
    check_ratio("low-queue-refills-gradually",
                armsx_audio_realtime_ratio(four_ntsc_frames,
                                           static_cast<double>(four_ntsc_frames) / mix_rate,
                                           mix_rate, target_queue - 882, target_queue),
                static_cast<double>(four_ntsc_frames) /
                    (static_cast<double>(four_ntsc_frames) + 220.5));
    check_ratio("bad-clock-sample-is-neutral",
                armsx_audio_realtime_ratio(four_ntsc_frames, 0.0, mix_rate,
                                           target_queue, target_queue),
                1.0);
    check_ratio("slowdown-is-bounded",
                armsx_audio_realtime_ratio(four_ntsc_frames, 1.0, mix_rate,
                                           target_queue, target_queue),
                0.5);
    check_ratio("catchup-is-bounded",
                armsx_audio_realtime_ratio(four_ntsc_frames, 0.001, mix_rate,
                                           target_queue, target_queue),
                1.05);

    if (failures != 0) {
        std::fprintf(stderr, "AUDIO_QUEUE failures=%d\n", failures);
        return 1;
    }

    std::printf("AUDIO_QUEUE all cases passed\n");
    return 0;
}
