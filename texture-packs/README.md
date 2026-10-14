# Automatic texture pack downloads

Open **Settings → Video → Texture packs**. Refresh downloads only this catalogue;
selecting a pack downloads, checks and installs its ZIP automatically. Downloads
are user-triggered, with progress, cancellation on leaving the screen, storage
checks and staged installation. Installed packs and cached catalogues work offline.
The initial catalogue is deliberately empty. No third-party packs or synthetic
test textures are shipped. A new APK is needed once to obtain this browser.

To apply a pack, open Video settings for a library/running game and select **This
game**. Compatibility uses the verified disc serial, not the game title. Wrong
regions cannot be selected. Global browsing permits download only; it never
applies a pack globally. A matching Download and use action enables replacements
and changes only that game's texture directory. Installed variants can be switched
without re-downloading. The existing Texture replacements toggle turns them off.
Software rendering does not display replacements; select a hardware renderer.

## Publish a pack

1. Obtain redistribution rights and preserve original author/license notices.
   Confirm the pack really works with this pinned SwanStation VRAM-write format
   and record every tested game/region/revision. Do not label a pack compatible
   merely because its title matches. No original game assets, ROMs or BIOS here.
2. Prepare a normalized ZIP containing only:

   ```text
   LICENSE.txt
   NOTICE.txt                       (optional attribution/additional notices)
   replacements/vram-write-<32 lowercase hex digits>.png
   ```

   PNG files need valid chunk checksums, dimensions up to 8192 per axis and no
   more than 16,777,216 pixels. Each file is bounded at 64 MiB. The ZIP is capped
   at 2 GiB, expanded contents at 8 GiB; oversized/unsupported packs need a
   deliberately reviewed format/limit update. Old psxe packs and newer differing
   hash formats are not interchangeable. The app never extracts arbitrary paths.
3. Upload the exact ZIP as a GitHub release asset (or retain an authorized
   upstream GitHub URL). Large ZIPs should not be committed as Git blobs. URLs
   must use HTTPS on GitHub/raw/release-asset hosts. This script does not upload.
4. Validate/register it, for example in PowerShell:

   ```powershell
   python tools/publish_texture_pack.py --pack "C:\packs\approved-pack.zip" --id example-pack --title "Example pack" --serials SLUS-00860 --version 1 --author "Original author" --license "Original license" --source-url "https://github.com/owner/source" --download-url "https://github.com/owner/repo/releases/download/packs/approved-pack.zip" --confirm-redistribution
   ```

   These are placeholders, not a supplied Tony Hawk pack. The helper computes
   compressed/expanded sizes and SHA-256 and updates both `catalogue.tsv` and the
   bundled `ps1-texture-packs.tsv` fallback. Identifiers/URLs must stay stable;
   new ZIP content changes its digest and installs alongside the older version.
5. Review the generated catalogue and verify the pack's rendering, then commit/push it to
   **android-compose**. Its endpoint is
   `https://raw.githubusercontent.com/ARMSX2/ARMSX1/android-compose/texture-packs/catalogue.tsv`.
   Users refresh and select a pack without another APK update.

## Storage and trust

Managed packs live in app-private `texture-packs/packs/<id>/<sha256>/` with a
`replacements/` child and a metadata receipt. User-managed texture folders,
dumps and earlier managed versions are never overwritten or deleted by downloads.
Failed downloads/extractions cannot publish a half-installed version. Only the
operation's temporary staging directory is removed on failure/cancellation.
There is no pack-removal or automatic pruning UI in this first version; app-data
deletion also deletes these managed packs. Existing user backups omit these
re-downloadable data files, as they already do for user-managed texture folders.

SHA-256 verifies downloads against the catalogue; PNG/ZIP checks catch corruption.
These hashes are not publisher signatures: catalogue authenticity relies on HTTPS
and control of the GitHub branch. License fields/notices record an author's terms;
validation does not grant permission. Downloading stores data, not executable
code. Real replacement loading/performance must be tested when real packs are added.
