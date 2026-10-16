package com.armsx2.ui.touch

/** Explicit profile selection wins over an older game-only edit without deleting that edit. */
internal object TouchProfileSelection {
    data class Selected(val name: String, val layout: TouchLayout, val editProfile: Boolean)

    fun resolve(
        profiles: List<TouchProfile>,
        binding: String?,
        custom: TouchLayout?,
        preferProfile: Boolean,
        portrait: Boolean,
    ): Selected {
        val bound = profiles.firstOrNull { it.name == binding }
        if (bound != null && (preferProfile || custom == null))
            return Selected(bound.name, bound.layoutFor(portrait).copy(), true)
        val fallback = profiles.firstOrNull { it.name == "Default" } ?: profiles.firstOrNull()
        if (custom != null) return Selected(bound?.name ?: fallback?.name ?: "Default", custom, false)
        return Selected(fallback?.name ?: "Default", fallback?.layoutFor(portrait)?.copy()
            ?: TouchLayout.defaultFor(portrait), false)
    }

    fun updated(profiles: List<TouchProfile>, name: String, portrait: Boolean, layout: TouchLayout,
        seed: TouchProfile? = null): TouchProfile =
        (profiles.firstOrNull { it.name == name } ?: seed?.copy(name = name)
            ?: TouchProfile(name, TouchLayout.default()))
            .withLayoutFor(portrait, layout.copy())
}
