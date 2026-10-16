package com.armsx2.update

/** GitHub feed order and nightly dates are not semantic version order. No Android dependencies. */
internal object UpdatePolicy {
    const val VERSION_CODE_EPOCH = 1_700_000_000L
    data class Release(val tag: String, val prerelease: Boolean, val published: Long,
        val url: String, val notes: String = "", val bytes: Long = 0, val digest: String = "")

    private val stable = Regex("^[vV]?(\\d+(?:\\.\\d+){1,3})$")
    private val nightly = Regex("^nightly-(\\d{8})\\.(\\d+)$", RegexOption.IGNORE_CASE)
    private fun stableParts(tag: String): List<Long>? = stable.matchEntire(tag.trim())
        ?.groupValues?.get(1)?.split('.')?.map { it.toLongOrNull() ?: return null }
    private fun nightlyParts(tag: String): List<Long>? = nightly.matchEntire(tag.trim())
        ?.groupValues?.drop(1)?.map { it.toLongOrNull() ?: return null }
    private fun compare(a: List<Long>, b: List<Long>): Int {
        for (i in 0 until maxOf(a.size, b.size)) {
            val result = a.getOrElse(i) { 0 }.compareTo(b.getOrElse(i) { 0 })
            if (result != 0) return result
        }
        return 0
    }

    fun newer(remote: Release, installed: String, installedCode: Long): Boolean {
        if (remote.tag.equals(installed.trim(), true)) return false
        val rs = stableParts(remote.tag); val localStable = stableParts(installed)
        val rn = nightlyParts(remote.tag); val localNightly = nightlyParts(installed)
        if (rs == null && rn == null) return false // Diagnostics/private/arbitrary tags are not updates.
        if (rs != null && localStable != null) return compare(rs, localStable) > 0
        if (rn != null && localNightly != null) return compare(rn, localNightly) > 0
        // CI/stable codes use Unix seconds minus this epoch. Across channels, never compare
        // "20261016" with "0.2.6". Publication time is a discovery hint; the downloaded APK's
        // actual package, certificate and monotonically increasing code are checked before install.
        if (installedCode in 50_000_000L..1_000_000_000L)
            return remote.published > VERSION_CODE_EPOCH + installedCode
        // Legacy small-code stable builds predate the timestamp-code nightly channel.
        return localStable != null && rn != null && remote.published > 0
    }

    fun select(releases: List<Release>, installed: String, installedCode: Long, includeNightly: Boolean): Release? {
        val eligible = releases.filter { (!it.prerelease || includeNightly) &&
            (includeNightly || nightlyParts(it.tag) == null) && newer(it, installed, installedCode) }
        // Pick within each channel first. Mixing semver and date comparators in one sort can
        // be non-transitive when an older stable release is republished after a newer one.
        val latestStable = eligible.filter { stableParts(it.tag) != null }.maxWithOrNull { a, b ->
            compare(stableParts(a.tag)!!, stableParts(b.tag)!!)
        }
        val latestNightly = eligible.filter { nightlyParts(it.tag) != null }.maxWithOrNull { a, b ->
            compare(nightlyParts(a.tag)!!, nightlyParts(b.tag)!!)
        }
        return listOfNotNull(latestStable, latestNightly).maxByOrNull { it.published }
    }

    data class Asset(val name: String, val url: String)
    data class Archive(val packageName: String, val code: Long, val certificates: Set<String>, val compatibleEngine: Boolean = true)
    fun archiveError(current: Archive, incoming: Archive): String? = when {
        incoming.packageName != current.packageName -> "Update is for a different app"
        incoming.code <= current.code -> "This release cannot update your installed build; its Android version code is not newer"
        incoming.certificates.isEmpty() || incoming.certificates != current.certificates -> "Update signing certificate does not match the installed app"
        !incoming.compatibleEngine -> "This release uses a different emulator engine and cannot safely update this build"
        else -> null
    }

    fun pickAsset(assets: List<Asset>): Asset? = assets.filter {
        val name = it.name.lowercase(java.util.Locale.ROOT)
        name.endsWith(".apk") && !listOf("armeabi", "armv7", "arm32", "x86", "mips").any(name::contains) &&
            !Regex("(?:^|[-_. ])(?:debug|diagnostic|private|test)(?:[-_. ]|$)").containsMatchIn(name) &&
            it.url.startsWith("https://github.com/ARMSX2/ARMSX1/releases/download/")
    }.let { candidates ->
        candidates.firstOrNull {
            val name = it.name.lowercase(java.util.Locale.ROOT)
            name.contains("arm64") || name.contains("aarch64")
        } ?: candidates.firstOrNull()
    }
}
