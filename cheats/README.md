# Cheat database updates without an APK release

The app's **Download cheats** button fetches the public data-only package at:

https://raw.githubusercontent.com/ARMSX2/ARMSX1/android-compose/cheats/ps1-cheats-update.zip

The button is in the global Cheats browser and each game's Cheats screen,
beside Browse cheat database and above Import & file tools. The first APK with
this feature must be installed once; later database changes need no new APK.
Downloads are explicit, not background network activity. No account is needed.

## Publish more codes

1. Add only redistribution-authorized source lists to `tools/cheat-supplements`
   in the existing RetroArch catalogue format. Record license, original authors,
   provenance and exact game/region/revision. Do not copy unlicensed collections
   or publish ROMs, BIOS, RAM dumps, private keys or users' settings.
2. Run `python tools/update_ps1_cheats.py <local-pinned-libretro-database>` to
   regenerate the catalogue and this download package. Serial matching remains
   conservative; do not guess region/revision compatibility. Experimental
   `tools/cheat-research` files are intentionally excluded.
3. If only the already-converted assets changed, run
   `python tools/build_cheat_update.py` instead.
4. Run the Python/JVM tests, including `Ps1CheatUpdatePackageTest`, then commit
   and push the generated package and relevant source/assets/notices to the
   **android-compose** branch. Merely pushing a `.cht` elsewhere will not update
   the download. Do not rename the endpoint without updating the app.
5. Users tap **Download cheats**. Existing codes and enabled selections are
   preserved. Newly matched codes are appended disabled when the game's Cheats
   screen reloads or is next opened. Ambiguous lists still require confirmation.

## Format and safety

Version 1 is a single ZIP containing the seven named catalogue/metadata/license
assets, `format-version.txt` and `hashes.tsv`. Deterministic packing gives a
stable revision hash. The app validates UTF-8, component SHA-256 checksums,
index rows/IDs, unique archive entries, advertised group counts and bounded
compressed/uncompressed sizes before atomically replacing its private database.
No paths from a ZIP are extracted to disk. No executable app/core code or
settings are downloaded. Checksums detect corruption, not a separate publisher
signature: authenticity relies on HTTPS and control of this GitHub branch.

Failure keeps the previous database. A missing/corrupt cached update falls back
to the built-in catalogue on startup. Successful updates survive app restarts
and work offline. The cache is separate from users' per-game `.cht` files and
never deletes or replaces them. Append-only merging intentionally does not
silently remove or modify an already imported code; users can edit their own
files to retire an old code. Back up memory cards and use only your exact region.

This package contains the same separately licensed CC BY-SA 4.0 data and
attribution as the bundled catalogue, not an APK or public-release clearance.
