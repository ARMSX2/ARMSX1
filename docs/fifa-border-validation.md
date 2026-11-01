# FIFA border presentation

The previous packed-RGB24 movie change did not address stale pixels in the
border of FIFA 2000's licensing screen. A normal-app capture on Thor shows
loading-screen pixels in a narrow strip at its lower edge. The existing
Overscan crop control called an unsupported bridge method, and the own engine
did not read `video.overscan_crop`.

The control now reaches the own engine both at boot and on a live change.
None keeps the full image. Small trims 1/32 from each edge; Full trims 1/16,
matching the existing setting's meaning. The crop changes the scanout source
rectangle before internal-resolution scaling. It applies to CPU rasterization,
OpenGL and Vulkan presentation, screenshots and shader input. Guest VRAM,
display registers, emulated timing and audio are unchanged. Loading a save state
keeps the host's crop choice.

RGB24 horizontal insets use an even number of pixels to preserve the packed
stream's red-byte alignment at a halfword-addressed display origin. Interlaced
vertical insets advance two VRAM rows per display line. Origins wrap at the VRAM
edges. No image heuristic or game-specific engine branch is used.

Seven core regression tests cover crop off/on, RGB24 alignment, wrapped origins,
interlaced offsets, tiny diagnostic images, software border removal with
unchanged guest VRAM, and save-state independence. Device regression coverage
adds a poisoned lower border with 15-bit and RGB24 pixels through software,
OpenGL and Vulkan at 1x and 4x, with filtering off/on and live crop changes.

Validation on Thor for 0.3.22: 412 core tests and 191 app tests passed; both
movie-presentation device tests passed (including 96 border/crop presentations).
The normal-app before/after recording confirms that Small crop removes the
loading-screen strip below the licensing image. Small was selected only for
FIFA 2000 on the test device; the default and other games were not changed.
The nine packaged native libraries are byte-identical to the installed 0.3.21
build. The APK passed signing-identity, update-version and 16 KiB alignment checks
and was installed over `com.nanodata.armsx` without uninstalling it.

Small crop is a border-removal option, not a correction to MDEC or movie
timing. The full-frame image with None can still expose game-generated border
data. A separate brief EA graphic observed before the movie must not be treated
as fixed by this change. Final visual confirmation of the reported movie-edge
flicker is separate from the deterministic poisoned-border tests.
