package com.armsx2.ui.saves

import kr.co.iefriends.pcsx2.NativeApp

/** Automatic resume must never silently roll back an imported/newer memory card. */
internal object AutoStateLoadPolicy {
    fun cardChanged(result: Int): Boolean =
        result == NativeApp.STATE_ERR_CARD_NEWER || result == NativeApp.STATE_ERR_CARD_DIVERGED
}
