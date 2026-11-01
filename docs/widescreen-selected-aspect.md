# Widescreen projection follows the selected aspect

The prior hack multiplied horizontal GTE projection by 3/4, so it only corrected geometry for a 16:9 output. The adapter now passes the selected output ratio to both GTE implementations and the native CPU interpreter/compiler. Custom ratios use the same path. Stretch uses the host's reported display dimensions and updates on display recreation.

Projection scales horizontal coordinates around the game's existing projection centre by `(4/3) / outputAspect`; vertical coordinates, depth and guest display timing are unchanged. The factor is computed when the setting changes. Vertex processing uses a bounded Q16 integer multiply with a constant divisor. At 16:9 this is arithmetically identical to the former 3/4 calculation; disabled projection skips that multiply.

Aspect is host configuration and remains excluded from machine snapshots. A load keeps the currently selected aspect and the existing state schema. Software, OpenGL and Vulkan use the same CPU/GTE projection code. This remains a geometry hack: a game's 2D menus, pre-rendered movies and its own off-screen object culling may not adapt to additional view width.

Host checks exercise 1:1, 4:3, 3:2, 16:10, 16:9, 18:9, 20:9, 64:27 and 4:1, toggling, invalid input and save-schema stability. Android `WidescreenProjectionTest` compares randomized native/reference geometry and changes ratios on an already-running native CPU in interpreter/compiler modes. `StatePersistenceIntegrationTest.reopenState` verifies actual adapter setting changes and Stretch while paused after restoring a state.

Thor validation on 0.3.25 passed both native geometry/CPU tests (including 27,600 randomized command/state comparisons) and the adapter aspect/Stretch test while loading numbered/autosave states created by 0.3.24 in a prior process. All 419 core and 193 app unit tests passed. The compact, non-debuggable APK passed signing identity/update and 16 KiB alignment verification before installation over the existing release package.
