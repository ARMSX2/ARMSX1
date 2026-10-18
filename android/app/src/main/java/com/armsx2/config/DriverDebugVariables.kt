// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.config

/** Literal environment values, never shell commands. Android-free for validation tests. */
object DriverDebugVariables {
    const val MAX_BYTES = 16384
    const val MAX_ENTRIES = 64
    private val prefixes = listOf("FD_", "TU_", "IR3_", "MESA_")
    private val namePattern = Regex("[A-Z][A-Z0-9_]*")
    private const val UBWC_FEATURE = "enable_tp_ubwc_flag_hint"

    data class Validation(val entries: Map<String, String> = emptyMap(), val error: String? = null) {
        val valid: Boolean get() = error == null
        val canonical: String get() = entries.toSortedMap().entries.joinToString("\n") { "${it.key}=${it.value}" }
    }

    private fun parse(text: String): Validation {
        if (text.toByteArray(Charsets.UTF_8).size > MAX_BYTES) return Validation(error = "Driver variables exceed 16 KiB.")
        if ('\u0000' in text) return Validation(error = "NUL characters are not allowed.")
        val values = linkedMapOf<String, String>()
        text.replace("\r\n", "\n").split('\n').forEachIndexed { index, raw ->
            val line = raw.trim(' ', '\t')
            if (line.isEmpty() || line.startsWith('#')) return@forEachIndexed
            val at = line.indexOf('=')
            fun error(reason: String) = Validation(error = "Line ${index + 1}: $reason")
            if (at <= 0) return error("expected NAME=value.")
            val name = line.substring(0, at).trim(' ', '\t')
            val value = line.substring(at + 1).trim(' ', '\t')
            if (name.length > 128 || !namePattern.matches(name) || prefixes.none { name.startsWith(it) && name.length > it.length })
                return error("only FD_, TU_, IR3_ and MESA_ variable names are allowed.")
            if (value.toByteArray(Charsets.UTF_8).size > 2048 || value.any { it.code < 32 || it.code == 127 })
                return error("value is too long or contains a control character.")
            if (values.containsKey(name)) return error("$name is listed more than once.")
            values[name] = value
            if (values.size > MAX_ENTRIES) return error("at most $MAX_ENTRIES variables are allowed.")
        }
        return Validation(values)
    }

    fun validate(text: String, ubwcHint: Boolean = false): Validation {
        val parsed = parse(text)
        if (!parsed.valid || !ubwcHint) return parsed
        val values = parsed.entries.toMutableMap()
        val features = values["FD_DEV_FEATURES"].orEmpty().split(',').map(String::trim)
            .filter { it.isNotEmpty() && it.substringBefore('=').trim() != UBWC_FEATURE }
        values["FD_DEV_FEATURES"] = (features + "$UBWC_FEATURE=1").joinToString(",")
        return parse(Validation(values).canonical)
    }

    fun isTurnipDriver(library: String, name: String, vendor: String, description: String): Boolean =
        listOf(library, name, vendor, description).any {
            it.contains("turnip", ignoreCase = true) || it.contains("freedreno", ignoreCase = true)
        }
}
