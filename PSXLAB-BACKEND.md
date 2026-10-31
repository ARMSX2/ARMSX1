# Original PSX Lab Android backend

Android uses the original PSX Lab CPU/GTE, CD/DMA, GPU and SPU implementation.
The current Compose UI is retained. The SwanStation submodule, Android host,
core-build script and packaging task have been removed. Desktop/platform source
outside the Android backend is unchanged.

`psxlab/backend` compiles the same engine sources as the standalone Android app
in `psxlab/app`. Native modules are `psxcore`, `psxgpu`, `psxdisc` and `psxtempo`;
the latter uses the separately built LGPL SoundTouch dependency. libchdr retains
its BSD notices. Third-party source and notices are included under the engine's
vendor directories. Existing frontend licensing and permissions are unchanged.

## Build

Use JDK 21 and the Android SDK. The host uses API 36. The shared engine library
uses API 35, NDK 28.2.13676358 and CMake 3.22.1. Only the SDL Java sources are
needed by historical frontend classes; SDL is not built or packaged as a core.

```sh
git submodule update --init --depth 1 third_party/SDL
cd android
./gradlew -PpsxlabRuntimeTest :app:assembleGithubDebug :app:testGithubDebugUnitTest
```

The runtime flag disables Android debug/CheckJNI overhead and enables R8 code
and resource shrinking for the installable runtime APK. Ordinary debug builds
without this flag remain unshrunk for development. Engine/JNI classes and the
versioned save-state field names remain protected by the backend consumer rules;
the frontend keeps its existing reflection rules. Native debug symbols remain
packaged for diagnosis. This does not change guest clocks or audio processing.
For a controlled comparison or fallback, add `-Parmsx.compactRuntime=false` to
produce an unshrunk runtime APK with the same Android debug/CheckJNI flags and
engine settings. Keep the package ID and signer unchanged when testing updates.

The compact package removes the unreferenced duplicate `library_theme.mp3`
(byte-identical to the retained `library_music.m4a`) and the retired library GIF.
The active library/pause music, procedural background, shaders, fonts, bundled
databases and emulator features remain included. Use `-Parmsx.applicationId=...` for an isolated
test app. Public release tasks remain gated by the existing frontend/asset audit;
the CI workflow builds and uploads an isolated development artifact, not a release.

Core JVM regressions can be run separately:

```sh
cd psxlab
./gradlew -Poptimized=true testReleaseUnitTest
```

No BIOS image, game, memory card, state, capture, signing key or local device
configuration is part of this source change. Select your own BIOS in the app.
PAL BIOS startup normally uses 50 Hz; NTSC startup uses approximately 60 Hz.

## Host integration and saves

`NativeApp` delegates launch, pause/resume, Surface changes, two-port input,
screenshots, states, disc replacement and audio/presentation suspension directly
to `ArmSxBackend`. Generation tokens use the adapter's monitor to serialize the
start/cancel transition, so stopping a prepared launch cannot start it afterwards
or stop a newer generation. Seekable CHD/data-image document URIs go directly to
the core; save identities do not depend on temporary SAF copy directories.

PSX Lab has two persistent shared raw 128 KiB cards under `psxlab/cards` and
versioned states under `psxlab/states`. The card manager points at those actual
files. Explicit imports validate/copy a raw card with a backup; old backend files
are preserved. SwanStation states cannot be loaded or converted by renaming them.
The adapter verifies a save backup before first initialization.

Some historical Java method names contain “Swan” for source compatibility with
the UI. They do not load another engine. Available diagnostics/state/card paths
delegate to PSX Lab. Unimplemented features return explicit empty/error results:
PGXP, achievements, shader chains, custom drivers, multitap, cheats, rewind,
runahead and embedded-PBP disc switching are not
implemented by this backend. Their presence in the existing UI is not a
compatibility claim. Native resolution and the existing shared GLES HD path are
supported; Vulkan is a presentation backend, not a separate polygon core.

## Validation recorded before publication

The shared engine's 327 JVM regressions passed, including large ordered VRAM
readback without a per-word GPU fence, intervening uploads and state restoration.
The deployed earlier-host integration passed 24 host JVM tests. Phone tests of
software, OpenGL and Vulkan BIOS startup recorded zero audio underruns after the
readback fix. A real 30-second Warped cold boot on Thor with the original engine
also recorded zero underruns and full guest speed. USA BIOS startup then measured
about 59.79 fields/s after the initial two seconds, with zero underruns; the user
reported stable FPS.

Those device measurements precede the newer Compose session/card adapter in this
branch. The updated Compose host has also passed 170 JVM checks. Build/JVM checks for that adapter are separate from device compatibility.
Full lifecycle/controller reconnection, completed levels/movies, CD-DA and real
multidisc handoffs still require broader testing. Existing source-publication
permission records do not imply release/asset clearance.

Texture dumping and raw VRAM-write replacements use the own-engine host service.
See `docs/psxlab-core-compatibility.md` for format, limits and rendering restrictions.
