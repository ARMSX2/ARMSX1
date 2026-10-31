package com.armsx2.ui

/** Pure presentation rules for live host readings, also used by the OSD tests. */
internal object OsdReadings {
    fun renderer(raw: String): String {
        val value = raw.trim()
        if (value.isEmpty() || value.equals("Stopped", true) || value.equals("Unknown", true)) return ""
        return value.removePrefix("SwanStation ")
    }

    fun resolution(width: Int, height: Int, scale: Int, videoWidth: Int, videoHeight: Int): String {
        if (width <= 0 || height <= 0) return "RES --"
        return buildString {
            append("RES ${width}x$height")
            if (scale > 0) append(" · ${scale}x internal")
            if (videoWidth > 0 && videoHeight > 0) append(" · video ${videoWidth}x$videoHeight")
        }
    }
}
