# Renderer submission efficiency

## Software clears

GP0 fill commands previously assigned VRAM one pixel at a time, recalculating wrapped coordinates for each assignment. Each row now uses one contiguous `Arrays.fill`, or two spans when it wraps across VRAM's right edge. The 16-pixel horizontal alignment, rounded width, vertical wrapping and 15-bit colour conversion are preserved. Zero-width/height fills return without incrementing the software content revision, allowing static scanout caches to remain valid.

This applies to software rasterization with OpenGL or Vulkan presentation and the Java display fallback. It does not change guest execution clocks. A unit test checks both wrapped edges, width rounding, full-width wrapping, empty fills and threaded ordering.

## OpenGL window binding

`beginWindow` already establishes this rasterizer's context before flushing commands. It now calls `eglMakeCurrent` for the window only when the current draw or read surface differs. Repeated frames on the same window avoid a redundant driver call. Switching rasterizers, recreated windows, shader toggles and pbuffer-to-window transitions still bind the required surfaces.

This serves native software display, OpenGL drawing and HD display through OpenGL. GPU-side drawing, scanout and buffer swaps retain their ordering.

## Vulkan fence wait

Each presentation path waited for the previous submission before updating resources/acquiring its next image, then waited on that same fence again in `begin`. Recording is now named `beginAfterFence` and trusts that completed wait. All callers, including CPU uploads, shared HD frames, BFI/shader subframes and verification readbacks, retain their first wait before touching resources. Fence reset, submission, acquire/present semaphores and imported GL producer fences remain in place.

The device timing regression demonstrated two fence-wait calls for one shared frame before the change. It now requires one. This removes a redundant API call, not the GPU synchronization required for safe reuse.

## Scope of the slowdown investigation

The original SM-A528B report used a PAL guest at 4× OpenGL. It retained intermittent waits but no game title; it cannot prove that these changes resolve that exact scene. Use the new command/window/swap scopes in a fresh report on the affected device to locate any remaining waits.

A command-buffer orphaning experiment was rejected: pixel checks passed, but its short Thor benchmark showed no benefit and slightly higher 4× elapsed time. The shipped command upload path retains the original buffer allocation policy.
