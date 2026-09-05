#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

/*
 * Streaming stereo WSOLA time stretcher. Ratio is input frames per output frame.
 * Candidate matching changes duration while keeping each segment at the original sample rate.
 * The expected analysis position remains the timing reference after correlation.
 */
class ArmsxAudioTimeStretcher {
  public:
    static constexpr std::size_t kChannels = 2;
    static constexpr std::size_t kSegmentFrames = 1024;   // 23.2 ms at 44.1 kHz
    static constexpr std::size_t kOverlapFrames = 256;    // 5.8 ms cross-fade
    static constexpr std::size_t kSynthesisHop = kSegmentFrames - kOverlapFrames;
    static constexpr std::size_t kSearchFrames = 128;     // +/- 2.9 ms
    static constexpr std::size_t kSearchStride = 4;

    void reset() {
        input_.clear();
        previous_.clear();
        analysis_position_ = 0.0;
        primed_ = false;
    }

    std::size_t bufferedFrames() const {
        return input_.size() / kChannels;
    }

    std::vector<std::uint8_t> process(
        const std::uint8_t* bytes,
        std::size_t input_frames,
        double ratio) {
        std::vector<std::int16_t> output;

        if (!bytes || input_frames == 0) {
            return {};
        }

        // Keep the analysis cursor finite and advancing.
        if (!std::isfinite(ratio)) {
            ratio = 1.0;
        }
        ratio = std::clamp(ratio, 0.5, 1.05);

        const auto* samples = reinterpret_cast<const std::int16_t*>(bytes);
        input_.insert(input_.end(), samples, samples + input_frames * kChannels);

        if (!primed_) {
            if (bufferedFrames() < kSegmentFrames) {
                return {};
            }

            previous_.assign(input_.begin(), input_.begin() + kSegmentFrames * kChannels);
            appendFrames(output, previous_.data(), kSynthesisHop);
            analysis_position_ = static_cast<double>(kSynthesisHop) * ratio;
            primed_ = true;
        }

        while (canSelectCompleteWindow()) {
            const std::size_t candidate = bestCandidate();
            appendOverlap(output, candidate);
            appendFrames(
                output,
                input_.data() + (candidate + kOverlapFrames) * kChannels,
                kSynthesisHop - kOverlapFrames);

            previous_.assign(
                input_.begin() + candidate * kChannels,
                input_.begin() + (candidate + kSegmentFrames) * kChannels);
            analysis_position_ += static_cast<double>(kSynthesisHop) * ratio;
        }

        compactInput();

        std::vector<std::uint8_t> result(output.size() * sizeof(std::int16_t));
        if (!result.empty()) {
            std::memcpy(result.data(), output.data(), result.size());
        }
        return result;
    }

  private:
    static void appendFrames(
        std::vector<std::int16_t>& output,
        const std::int16_t* frames,
        std::size_t frame_count) {
        output.insert(output.end(), frames, frames + frame_count * kChannels);
    }

    bool canSelectCompleteWindow() const {
        const std::size_t frames = bufferedFrames();
        const std::size_t expected = static_cast<std::size_t>(std::llround(analysis_position_));

        // Wait for both sides of the search interval.
        return expected <= std::numeric_limits<std::size_t>::max() -
                               (kSearchFrames + kSegmentFrames) &&
               frames >= expected + kSearchFrames + kSegmentFrames;
    }

    std::uint64_t differenceScore(std::size_t candidate, std::size_t stride) const {
        std::uint64_t score = 0;
        const std::int16_t* old_tail =
            previous_.data() + kSynthesisHop * kChannels;
        const std::int16_t* new_head = input_.data() + candidate * kChannels;

        for (std::size_t frame = 0; frame < kOverlapFrames; frame += stride) {
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                const std::int32_t delta =
                    static_cast<std::int32_t>(old_tail[frame * kChannels + channel]) -
                    static_cast<std::int32_t>(new_head[frame * kChannels + channel]);
                score += static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
            }
        }

        return score;
    }

    std::size_t bestCandidate() const {
        const std::size_t frames = bufferedFrames();
        const std::size_t expected = static_cast<std::size_t>(std::llround(analysis_position_));
        const std::size_t first = expected > kSearchFrames ? expected - kSearchFrames : 0;
        const std::size_t last = std::min(expected + kSearchFrames, frames - kSegmentFrames);

        std::size_t best = std::clamp(expected, first, last);
        std::uint64_t best_score = differenceScore(best, kSearchStride);

        // Prefer the closest candidate when scores match.
        for (std::size_t distance = 1; distance <= kSearchFrames && best_score != 0; ++distance) {
            if (expected >= distance) {
                const std::size_t candidate = expected - distance;
                if (candidate >= first && candidate <= last) {
                    const std::uint64_t score = differenceScore(candidate, kSearchStride);
                    if (score < best_score) {
                        best = candidate;
                        best_score = score;
                    }
                }
            }

            if (expected <= std::numeric_limits<std::size_t>::max() - distance) {
                const std::size_t candidate = expected + distance;
                if (candidate >= first && candidate <= last) {
                    const std::uint64_t score = differenceScore(candidate, kSearchStride);
                    if (score < best_score) {
                        best = candidate;
                        best_score = score;
                    }
                }
            }
        }

        // Refine the coarse match at full overlap resolution.
        const std::size_t refine_first = best > kSearchStride ? best - kSearchStride : first;
        const std::size_t refine_last = std::min(best + kSearchStride, last);
        best_score = differenceScore(best, 1);
        for (std::size_t candidate = std::max(refine_first, first);
             candidate <= refine_last;
             ++candidate) {
            const std::uint64_t score = differenceScore(candidate, 1);
            if (score < best_score ||
                (score == best_score && absoluteDistance(candidate, expected) <
                                            absoluteDistance(best, expected))) {
                best = candidate;
                best_score = score;
            }
        }

        return best;
    }

    static std::size_t absoluteDistance(std::size_t a, std::size_t b) {
        return a > b ? a - b : b - a;
    }

    void appendOverlap(std::vector<std::int16_t>& output, std::size_t candidate) const {
        const std::int16_t* old_tail =
            previous_.data() + kSynthesisHop * kChannels;
        const std::int16_t* new_head = input_.data() + candidate * kChannels;

        for (std::size_t frame = 0; frame < kOverlapFrames; ++frame) {
            const std::int64_t old_weight =
                static_cast<std::int64_t>(kOverlapFrames - frame);
            const std::int64_t new_weight = static_cast<std::int64_t>(frame);
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                const std::int64_t mixed =
                    static_cast<std::int64_t>(old_tail[frame * kChannels + channel]) * old_weight +
                    static_cast<std::int64_t>(new_head[frame * kChannels + channel]) * new_weight;
                output.push_back(static_cast<std::int16_t>(
                    mixed / static_cast<std::int64_t>(kOverlapFrames)));
            }
        }
    }

    void compactInput() {
        if (!primed_ || analysis_position_ <= static_cast<double>(kSearchFrames + 8)) {
            return;
        }

        const std::size_t discard =
            static_cast<std::size_t>(analysis_position_) - kSearchFrames - 8;
        const std::size_t bounded = std::min(discard, bufferedFrames());
        if (bounded == 0) {
            return;
        }

        input_.erase(input_.begin(), input_.begin() + bounded * kChannels);
        analysis_position_ -= static_cast<double>(bounded);
    }

    std::vector<std::int16_t> input_;
    std::vector<std::int16_t> previous_;
    double analysis_position_ = 0.0;
    bool primed_ = false;
};
