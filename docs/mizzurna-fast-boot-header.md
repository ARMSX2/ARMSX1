# Mizzurna Falls report and fast-boot header compatibility

The supplied 0.3.21 report identifies Mizzurna Falls on a Samsung SM-A528B, with fast boot enabled. It stops after New Game/the opening movie on `UnsupportedOperationException: Read at FFFFFF00 requires unimplemented hardware`. This is a guest memory-access failure escaping the VM, not an Android native driver crash. The report does not retain the exact fault instruction/registers, so the earlier cause of that address is not established by the report alone.

The own-engine fast loader initialized the BIOS kernel and copied the executable payload/BSS, but omitted the complete 2048-byte EXE header from the BIOS `0xA000B070` buffer. The English Mizzurna Falls translation is known to execute helpers stored in otherwise unused header bytes. The original translation/loader investigation documents failure immediately after the first movie when the header is absent: <https://github.com/socram8888/tonyhax/issues/95>. Whether the reporting image is that translation is currently unconfirmed.

The shared PS-X EXE loader now copies the entire validated header to that buffer before payload/BSS, matching BIOS load ordering. Payload ranges/lengths are validated before any mutation. The initialized kernel, COP0 and devices remain intact. No game-name override, dropped memory reads or substituted return values are used. The normal disc-through-BIOS path is unchanged.

An original regression fixture calls a helper in the header after entering its main code. It fails against the previous loader and passes after the change, checking every header byte and preserved kernel/COP0 values. Invalid/truncated payloads must leave the buffer untouched. `ExecutableHeaderIntegrationTest` repeats the handoff with a real user-supplied BIOS and portable/native interpreter/ARM64 compiler modes, without accessing player cards or saves. Host validation passes all 421 core tests.

This fixes a demonstrated fast-loader compatibility defect and is a candidate for this report's earlier failure. The exact Mizzurna Falls image and New Game route still require a retest on the reporting device.
