# Attributed regional supplements

These data files are derivatives of the existing Libretro catalogue at commit
`12c21cf79f1e20eff6ac68b80875816c717d0a7a` and remain CC BY-SA 4.0, separately
from the app. See `android/app/src/main/assets/ps1-cheats-LICENSE.txt` for the
complete licence. Attribution: Libretro database contributors, Redump
contributors, and the original cheat authors named below.

## Crash Bandicoot - Warped (USA), SCUS-94244

Licensed input: [Libretro's existing Warped GameShark data](https://github.com/libretro/libretro-database/blob/12c21cf79f1e20eff6ac68b80875816c717d0a7a/cht/Sony%20-%20PlayStation/Crash%20Bandicoot%203%20-%20Warped%20%28Europe%29%20%28GameShark%29.cht).

That file's European label is not used as evidence of USA compatibility. Only
six groups whose exact addresses/values are independently listed for USA
`SCUS-94244` are included in the USA supplement:

- CodeBoy: 99 Lives, Enable All Powers, Have All Gems, Have All Sapphires and
  Have All Crystals. [Author's USA listings](https://gamehacking.org/game/88639?hacker=CodeBoy).
- SonicBlueTM: Have All Platinum Relics. [Author's USA listing](https://gamehacking.org/game/88639?hacker=SonicBlueTM).

Changes: selected those six groups, recorded their authors and regional
verification, and assigned a separate USA title/serial. Other groups in the
European input have not been copied into the USA list. No game, BIOS or save
data is included. This provenance check is not an in-game effect test, and no
claim is made for untested pressings, betas or revisions that reuse an ID.

`tools/update_ps1_cheats.py` merges these files with the pinned upstream data
deterministically. Do not relabel another regional list without independent
evidence, or silently replace an identically named upstream catalogue.
