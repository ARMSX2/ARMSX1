package com.armsx2.ui.emulation

/** Stable keys keep hidden/dynamic options and translated labels from changing saved layouts. */
internal object MenuOrderPolicy {
    /** Place a newly introduced option beside its anchor without resetting a user's custom order. */
    fun insertAfterIfNew(stored: List<String>, item: String, anchor: String): List<String> {
        if (item in stored || anchor !in stored) return stored
        return stored.toMutableList().apply { add(indexOf(anchor) + 1, item) }
    }

    fun visible(stored: List<String>, defaults: List<String>): List<String> =
        stored.filter { it in defaults }.distinct() + defaults.filterNot { it in stored }.distinct()

    fun move(stored: List<String>, defaults: List<String>, item: String, target: String): List<String> {
        val all = stored.distinct() + defaults.filterNot { it in stored }.distinct()
        val from = all.indexOf(item)
        val to = all.indexOf(target)
        if (item !in defaults || target !in defaults || from < 0 || to < 0 || from == to) return all
        return all.toMutableList().apply {
            remove(item)
            add(indexOf(target) + if (from < to) 1 else 0, item)
        }
    }
}

/** A held or interrupted press can never activate the item on release. */
internal class MenuConfirmPress {
    private var key: Int? = null
    private var consumed = false
    fun down(code: Int): Boolean {
        if (key != null) return false
        key = code
        consumed = false
        return true
    }
    fun held() { if (key != null) consumed = true }
    fun matches(code: Int): Boolean = key == code
    fun cancel() { held() }
    fun up(code: Int, cancelled: Boolean): Boolean {
        if (key != code) return false
        val activate = !consumed && !cancelled
        key = null
        consumed = false
        return activate
    }
    fun reset() { key = null; consumed = false }
}
