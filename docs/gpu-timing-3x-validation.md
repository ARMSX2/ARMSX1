# Reduce GPU profiling overhead at HD resolutions

The Moto G75 CTR capture at 3x identifies HD triangles with framebuffer fetch. Its GPU draw timing dominates the measured stages, while the emulation counter alone still reads about 59.8 FPS and many pictures are skipped. A 2x fallback was not the requested solution.

Two unnecessary profiling costs were present: GPU timing queries ran even with logging disabled, and HardwareGpu::flush opened a GPU draw query before discovering that the HD batch would only queue CPU primitive records. Deferred HD batches could therefore insert query boundaries even when they issued no raster command. The report also called completed timer segments "draw passes"; nested timing segments are not physical draw calls.

Queries now follow the existing logger/profiling switch, allocate lazily and are removed without reading unfinished results on disable. GPU draw timing starts after the deferred-only return, around actual GPU work. The existing shaders, primitive ordering, scale selection, audio pipeline and CPU execution are unchanged. All recent feedback/scissor/barrier and crash fixes are retained.

The timeline now calls timing segments "draw spans" and separately reports native and HD raster API calls per second. One API call can contain multiple primitives. Status distinguishes unsupported timers, active timing and logging disabled. The native snapshot accepts the previous 15-value array and the extended 17-value array.

Validation: the host GL-call shim executes the real timer state machine and passes disabled-query, nesting, nonblocking-read, bounded-pool, disjoint and toggle checks. All 511 core JVM tests pass, including the report compatibility and distinct draw-count tests. Pixel shaders are unchanged, and device FPS gains have not been measured. The next Moto 3x capture must establish whether the reduced query traffic improves presented frames and audio and show the true draw-call rate. No device install or gameplay test is authorized as part of these local checks.
