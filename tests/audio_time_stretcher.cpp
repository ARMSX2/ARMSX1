#include "frontend/audio_time_stretcher.h"

#include <cstdio>
#include <cstdlib>

static int failures;

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "AUDIO STRETCH failed: %s\n", message);
        ++failures;
    }
}

static void tone(double frequency, double ratio, std::size_t chunk) {
    ArmsxAudioTimeStretcher stretch;
    constexpr std::size_t frames = 132300;
    std::vector<std::int16_t> output;
    std::vector<std::int16_t> original;
    for (std::size_t start = 0; start < frames; start += chunk) {
        const std::size_t count = std::min(chunk, frames - start);
        std::vector<std::int16_t> input(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            const auto sample = static_cast<std::int16_t>(12000 * std::sin(
                6.283185307179586 * frequency * (start + i) / 44100));
            input[i * 2] = sample;
            input[i * 2 + 1] = -sample;
            original.push_back(sample);
        }
        const auto bytes = stretch.process(reinterpret_cast<const std::uint8_t*>(input.data()), count, ratio);
        for (std::size_t i = 0; i < bytes.size(); i += 4) {
            std::int16_t left, right;
            std::memcpy(&left, bytes.data() + i, 2);
            std::memcpy(&right, bytes.data() + i + 2, 2);
            check(left == -right, "stereo phase relationship is preserved");
            output.push_back(left);
        }
        check(stretch.bufferedFrames() <= 3 * ArmsxAudioTimeStretcher::kSegmentFrames,
              "streaming input storage stays bounded");
    }
    const double expected = frames / ratio;
    check(std::abs(double(output.size()) - expected) < 4096, "output duration follows requested ratio");
    unsigned crossings = 0;
    for (std::size_t i = 1; i < output.size(); ++i)
        crossings += output[i - 1] <= 0 && output[i] > 0;
    const double measured = crossings * 44100.0 / output.size();
    if (std::abs(measured - frequency) > frequency * 0.01) {
        std::fprintf(stderr, "AUDIO STRETCH pitch: input=%.1f ratio=%.3f output=%.3f\n",
                     frequency, ratio, measured);
        ++failures;
    }
    if (ratio == 1.0)
        check(std::equal(output.begin(), output.end(), original.begin()), "unity ratio is sample-exact");
    stretch.reset();
    check(stretch.bufferedFrames() == 0, "reset discards all buffered samples");
}

static void invalid_ratio(std::uint64_t bits) {
    double ratio;
    std::memcpy(&ratio, &bits, sizeof(ratio));
    ArmsxAudioTimeStretcher invalid, reference;
    std::vector<std::int16_t> input(4096, 1234);
    for (unsigned i = 0; i < 3; ++i) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.data());
        check(invalid.process(bytes, 2048, ratio) == reference.process(bytes, 2048, 1.0),
              "nonfinite ratio falls back to unity");
        check(invalid.bufferedFrames() == reference.bufferedFrames(),
              "nonfinite ratio keeps a valid streaming position");
    }
}

int main() {
    for (double ratio : {0.5, 0.63, 0.8, 1.0, 1.005})
        for (double frequency : {60., 110., 440., 4000.})
            tone(frequency, ratio, 744);
    tone(60, 0.63, 17);
    tone(110, 1.0, 17);
    invalid_ratio(UINT64_C(0x7ff8000000000001));
    invalid_ratio(UINT64_C(0x7ff0000000000000));
    invalid_ratio(UINT64_C(0xfff0000000000000));
    if (failures) return 1;
    std::puts("AUDIO STRETCH PASS 22 tones, pitch, duration, stereo, unity, bounded storage, reset and invalid ratios");
    return 0;
}
