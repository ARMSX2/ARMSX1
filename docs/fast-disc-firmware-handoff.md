# Fast disc boot keeps the firmware's LoadExe/Exec path

The Dr. Slump and Rhapsody reports from 0.3.21 show invalid accesses at `3C1A0000` / `3C1A0008`, with fast boot enabled. Those addresses resemble a MIPS instruction word, suggesting corrupted guest state; the reports lack fault-time registers, so the earlier cause cannot be proved from them alone.

The prior fast-disc route stopped after BIOS initialization and directly injected/jumped to the game executable. That skipped the firmware's disc-load/Exec work and its thread/kernel setup. Fast disc boot now replaces the resident shell entry in RAM with a short display-control/return routine. The normal CPU executes that routine and returns to the real BIOS bootstrap, which then reads and launches the disc itself. BIOS bytes on disk are not patched. An unexpected shell return address keeps the normal shell instead of guessing a destination.

This path applies to discs with a recognized console region. Standalone EXEs and serial-less/homebrew discs retain the direct loader, including the complete executable-header buffer fix. The latter preserves launch support for images that the retail BIOS cannot authenticate. Normal boot and state restores retain their existing paths.

A host regression verifies that fast disc startup returns to a synthetic firmware caller while preserving kernel/COP0 state. Before production wiring, a Thor prototype loaded the owned Crash 2 disc through the real SCPH1001 bootstrap. `BiosBootstrapIntegrationTest` accepts an explicit licensed `gameUri` to test that route; the tiny synthetic fixture disc is intentionally not accepted by the retail boot path. Dr. Slump/Rhapsody images are not present on Thor, so their exact failing scenes still need retesting.
