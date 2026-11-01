# CPU context retained at VM failure

An earlier Mizzurna Falls report kept the last half-second/frame PC (`80000080`) but not the CPU state at the instruction that read `FFFFFF00`. NativeCpu already exports `faultPc` and `faultInstruction` on a JNI memory callback failure. The adapter discarded its CPU object before the outer error/report handler ran.

The emulation-loop exception path now records `Cpu.diagnostic()` before device teardown. Reports and persisted `last-session.json` expose it as `failureCpuContext`; structured logs include `runtime.cpu_context` with the exception and guest clock. It contains the fault PC/instruction, continuation PC, SR/CAUSE/EPC and all general registers. It resets at the next launch. This is failure-time evidence, not a claim that every VM error originates in the CPU. Normal frames do not collect extra register dumps.

`CrashContextIntegrationTest` intentionally performs a byte read from `FFFFFF00` in an isolated original executable. It checks that the exact `80010008` / `81020000` fault and `r8=FFFFFF00` survive VM teardown in the API report, persisted session and structured log. The fixture uses separate cards/settings; it does not crash the player's running game.
