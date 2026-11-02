# CUE launch failure in 0.3.33

The supplied Moto G75 report identifies source commit 2ec81c7 and a NullPointerException at ArmSxBackend.discLayout, called by run before the first emulated frame. It is a diagnostic-formatting regression, not a GPU-driver failure.

Media(CueDisc) intentionally sets its ByteSource field to null and owns the CueDisc instead. The new disc-layout formatter dereferenced that absent ByteSource before entering its metadata exception guard. CUE launches therefore aborted while composing the startup timeline incident. The same helper is used after disc changes.

The formatter now selects CUE/BIN explicitly for CUE media, retains the byte-source format name for other containers and keeps format/track description work inside its exception guard. CUE ownership and CD/media emulation are unchanged. The CPU, GPU batching, readback and audio changes remain in place.

The new regression fixture mounts real synthetic ISO and CUE media and invokes the actual startup/disc-change formatter. Both single-track CUE and data-plus-audio CUE cases reproduced the same null dereference before the fix. With the fix, all 501 core JVM tests pass, including CUE track/audio descriptions, unchanged source ownership, ordinary ISO, executable and BIOS-only cases. This is local verification; no phone or Thor install is performed as part of preparing this hotfix.
