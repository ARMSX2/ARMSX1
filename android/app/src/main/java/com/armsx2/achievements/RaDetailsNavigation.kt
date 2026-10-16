package com.armsx2.achievements

/** Hardware B/Back is dispatched by the frontend before Compose's BackHandler. */
internal object RaDetailsNavigation {
    private var owner: Any? = null
    private var dismiss: (() -> Unit)? = null

    fun open(onBack: () -> Unit): () -> Unit {
        val token = Any()
        owner = token
        dismiss = onBack
        return {
            if (owner === token) {
                owner = null
                dismiss = null
            }
        }
    }

    fun back(): Boolean {
        val action = dismiss ?: return false
        action()
        return true
    }
}
