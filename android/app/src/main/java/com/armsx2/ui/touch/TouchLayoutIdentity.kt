package com.armsx2.ui.touch

/** One identity order for saving AND reloading. A menu's filename key is not a disc serial. */
internal object TouchLayoutIdentity {
    private val legacyCrcSuffix = Regex("\\s+\\([0-9a-fA-F]{8}\\)$")

    fun resolve(launchSerial: String?, coreSerial: String?, gameKey: String?, menuKey: String?, hint: String? = null): String? =
        sequenceOf(launchSerial, coreSerial, gameKey,
            menuKey?.trim()?.replace(legacyCrcSuffix, ""), hint?.trim()?.replace(legacyCrcSuffix, ""))
            .mapNotNull { it?.trim()?.takeIf(String::isNotEmpty) }
            .firstOrNull()
}
