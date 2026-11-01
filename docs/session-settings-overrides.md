# Release live setting overrides when a game ends

The adapter retained runtime aspect, crop, widescreen, volume/mute, presentation, fast-forward and texture overrides after stopping a game. A later session could therefore read the correct regenerated `settings.toml` yet continue using the prior game's live values. This is a concrete settings-lifetime defect; the broader report about old installations does not identify which stored keys were incompatible.

Shutdown now clears those transient fields after saving the stopped-session report. The next launch resolves its current global/per-game file and can apply its own live overrides. Global settings files, sparse per-game choices, controller mappings, cards and save states are not reset or deleted. Existing sparse overrides still intentionally take precedence over global settings; clearing such an override remains a separate user choice.

`SettingsSessionIntegrationTest` runs two isolated EXE sessions on one adapter, with a custom widescreen/crop override for the first. It verifies the live values clear at shutdown and the next session uses its 16:9 baseline with widescreen disabled. This test targets session carryover; it does not establish the cause of every upgrade mismatch or justify erasing old settings.

The test reproduced the second session incorrectly keeping widescreen enabled on 0.3.26, then passed on the installed 0.3.27 build. Existing saved preferences/cards/states were preserved during the signed in-place update.
