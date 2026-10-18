// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2

import android.content.Context
import com.armsx2.config.DriverDebugVariables

/** App-wide: Mesa flags may be cached until process exit, not just the next game. */
object DriverDebugPreferences {
    data class Options(val variables: String = "", val ubwcHint: Boolean = false)
    private const val VARIABLES = "gpu.driver.debugVariables"
    private const val UBWC = "gpu.driver.ubwcFlagHint"
    private var startup: Options? = null
    private fun prefs(context: Context) = context.applicationContext.getSharedPreferences("ARMSX2", Context.MODE_PRIVATE)
    fun read(context: Context): Options = prefs(context).let {
        Options(it.getString(VARIABLES, "").orEmpty(), it.getBoolean(UBWC, false))
    }
    @Synchronized fun initialize(context: Context) { if (startup == null) startup = read(context) }
    @Synchronized fun forProcess(context: Context): Options { initialize(context); return startup!! }
    /** Call on IO; synchronous persistence is required before a cold restart. */
    fun save(context: Context, options: Options): Boolean {
        if (!DriverDebugVariables.validate(options.variables, options.ubwcHint).valid) return false
        return prefs(context).edit().putString(VARIABLES, options.variables).putBoolean(UBWC, options.ubwcHint).commit()
    }
    fun nativeValues(context: Context, driver: CustomDriver.InstalledDriver?): String {
        if (driver == null || !DriverDebugVariables.isTurnipDriver(driver.libraryName, driver.name, driver.vendor, driver.description)) return ""
        val options = forProcess(context)
        val validation = DriverDebugVariables.validate(options.variables, options.ubwcHint)
        return if (validation.valid) validation.canonical else ""
    }
}
