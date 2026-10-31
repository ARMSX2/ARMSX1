package com.armsx2.core

import com.armsx2.ShaderParams
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import kr.co.iefriends.pcsx2.NativeApp

/** Shader settings live in the host's global/sparse-game store, outside settings.toml. */
object Ps1Shaders {
    fun push(settings: Settings) {
        NativeApp.setShaderChain(settings.shaderChainEnabled, settings.shaderChainPreset)
        ShaderParams.push(settings.shaderChainPreset, settings.shaderChainParams[settings.shaderChainPreset].orEmpty())
    }

    fun pushForGame(gameKey: String?) = push(ConfigStore.resolveForGame(gameKey))
}
