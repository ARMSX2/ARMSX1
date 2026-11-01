# Save states across process restarts

The own-engine save manager now reads `psxlab/states/*.psxlabstate` instead of the retired backend's state directory. Slot discovery and PNG previews work without a running VM. A saved original source lets the library resolve the game and cold-launch its slot after process death.

The frontend supplies the original URI/path before starting the adapter. New state filenames and payload identities use that permanent source, not a per-session SAF cache directory. The version-2 outer header records game and disc source separately. An empty disc source means the freshly prepared boot image. Swapped discs retain their original URI and are prepared again before restoring. Prepared copies close after their CD readers and are released when no longer needed.

Version-1 states remain readable in place. Lookup accepts an old staged identity only when its complete generated document-URI digest matches the current boot image. Full machine validation still checks the recorded identity, architecture, BIOS checksum and device schema. A new save writes the permanent filename and leaves the old file intact. Unmatched old states remain listed for backup/deletion; reselecting their original library game permits recovery when its URI digest matches. Existing SwanStation-format states remain incompatible and are not converted or deleted.

The checked-load adapter previously returned a memory-card conflict unconditionally, preventing automatic loads after a fresh boot. PSX Lab's codec excludes memory-card contents, so checked loads now return the real load result and retain current card data. Export backups include own-engine states, cards and the autosave source index.

Host regressions cover all numbered slots/autosave, reopening the store, version-1 recovery, unrelated-game isolation, truncated headers, original-file preservation, library recovery from a URI digest and retaining current card contents on restore.

`StatePersistenceIntegrationTest` additionally runs `writeState` and `reopenState` in separate Android instrumentation processes, with the same unique `auditId`. It uses a private `performance/state-persistence-<auditId>` root and the existing synthetic `fixtures/game.chd`, with a fresh launch path in each process. It verifies actual RAM restoration from numbered/autosave slots after removing the first launch image. Player saves/cards/settings are never used as test destinations.

Validated on Thor with 0.3.24: both separate-process phases passed. A frontend test also saved FIFA 2000 to previously empty Slot 8, force-stopped the app, found its title/preview in Game Manager and cold-loaded it with `ANDROID_AUTOLOAD slot=7 result=0`. The slot was retained. Existing cards were backed up before the test; FIFA's test-only automatic card write was archived separately and the original card restored and hash-verified. Host validation passed 417 core and 193 app tests.
