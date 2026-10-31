package com.armsx2.textures

import com.armsx2.config.Ps1Settings

/** Activation is game-local, even while a global browser can install packs for later. */
object TexturePackSelection {
    fun updated(settings: Ps1Settings, gameKey: String?, verifiedSerial: String?, pack: TexturePack,
                installedDirectory: String): Ps1Settings {
        require(!gameKey.isNullOrBlank() && pack.matches(verifiedSerial)) { "Verified per-game scope required" }
        require(installedDirectory.isNotBlank())
        return settings.copy(textureReplacements = true, textureDir = installedDirectory)
    }
}
