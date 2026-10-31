"""Guard the native hot-loop boundary; APK disassembly verifies actual inlining."""
from pathlib import Path
import unittest


SOURCE = (Path(__file__).resolve().parents[1] / "frontend/main.cpp").read_text(encoding="utf-8")


def method(name):
    signature = f"std::uint32_t {name}()"
    start = SOURCE.index(signature)
    brace = SOURCE.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class FrameLoopIsolationTest(unittest.TestCase):
    def test_dispatch_is_outside_instruction_loops(self):
        dispatch = method("stepOneFrame")
        self.assertIn("probe720Active() && g_psx_work_diag_enabled", dispatch)
        self.assertIn("? stepOneFrameProfiled() : stepOneFrameNormal()", dispatch)
        self.assertNotIn("while (", dispatch)
        self.assertNotIn("psx_update(", dispatch)
        self.assertIn("instruction_loop_ticks_ +=", dispatch)

    def test_normal_loop_cannot_absorb_optional_probe(self):
        self.assertIn("__attribute__((noinline)) std::uint32_t stepOneFrameNormal()", SOURCE)
        normal = method("stepOneFrameNormal")
        self.assertIn("while (vblank_counter_ == start_vblank)", normal)
        self.assertIn("psx_update(psx_);", normal)
        self.assertIn("steps >= kMaxFrameSteps", normal)
        for probe in ("probe720", "psx_update_profiled", "g_psx_work_diag", "SDL_Get"):
            self.assertNotIn(probe, normal)

    def test_profiled_loop_retains_sampling_phase_and_frame_boundary(self):
        self.assertIn("__attribute__((noinline)) std::uint32_t stepOneFrameProfiled()", SOURCE)
        sampled = method("stepOneFrameProfiled")
        self.assertIn("while (vblank_counter_ == start_vblank)", sampled)
        self.assertIn("16381 - probe720_instruction_phase_", sampled)
        self.assertIn("if (--countdown == 0)", sampled)
        self.assertIn("psx_update_profiled(psx_);", sampled)
        self.assertIn("psx_update(psx_);", sampled)
        self.assertIn("probe720_instruction_phase_ = 16381 - countdown", sampled)
        self.assertIn("steps >= kMaxFrameSteps", sampled)


if __name__ == "__main__":
    unittest.main()
