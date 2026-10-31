package com.armsx2.data.library

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray

/** Provider and consent activity share these preferences in the app process. */
internal object RecentGamesAccess {

    const val PREFS_NAME = "ARMSX2"

    /** Package names granted read access, as a JSON array of strings. */
    const val KEY_GRANTED_PACKAGES = "library.shareRecentGames.packages"

    const val KEY_DECLINED_PACKAGES = "library.shareRecentGames.declined"

    fun prefs(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun grantedPackages(prefs: SharedPreferences): Set<String> = decode(prefs, KEY_GRANTED_PACKAGES)

    fun isSharing(prefs: SharedPreferences): Boolean =
        prefs.getBoolean(RecentGamesContentProvider.KEY_SHARE_ENABLED, false) ||
            grantedPackages(prefs).isNotEmpty()

    fun setSharingEnabled(prefs: SharedPreferences, enabled: Boolean) {
        prefs.edit()
            .putBoolean(RecentGamesContentProvider.KEY_SHARE_ENABLED, enabled)
            .remove(KEY_DECLINED_PACKAGES)
            .apply {
                if (!enabled) remove(KEY_GRANTED_PACKAGES)
            }
            .apply()
    }

    private fun decode(prefs: SharedPreferences, key: String): Set<String> {
        val raw = prefs.getString(key, null) ?: return emptySet()
        return runCatching {
            val array = JSONArray(raw)
            buildSet {
                repeat(array.length()) { index ->
                    array.optString(index).takeIf { it.isNotBlank() }?.let(::add)
                }
            }
        }.getOrDefault(emptySet())
    }

    fun isGranted(prefs: SharedPreferences, packageName: String?): Boolean =
        packageName != null && packageName in grantedPackages(prefs)

    fun grant(prefs: SharedPreferences, packageName: String) {
        prefs.edit()
            .putString(KEY_GRANTED_PACKAGES, encode(grantedPackages(prefs) + packageName))
            .putString(KEY_DECLINED_PACKAGES, encode(declinedPackages(prefs) - packageName))
            .apply()
    }

    fun declinedPackages(prefs: SharedPreferences): Set<String> = decode(prefs, KEY_DECLINED_PACKAGES)

    fun isDeclined(prefs: SharedPreferences, packageName: String?): Boolean =
        packageName != null && packageName in declinedPackages(prefs)

    fun decline(prefs: SharedPreferences, packageName: String) {
        write(prefs, KEY_DECLINED_PACKAGES, declinedPackages(prefs) + packageName)
    }

    private fun write(prefs: SharedPreferences, key: String, packages: Set<String>) {
        prefs.edit().putString(key, encode(packages)).apply()
    }

    private fun encode(packages: Set<String>): String =
        JSONArray().apply { packages.sorted().forEach(::put) }.toString()
}
