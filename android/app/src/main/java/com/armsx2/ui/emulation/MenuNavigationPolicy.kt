package com.armsx2.ui.emulation

/** Navigation follows the visible tab rail, not the display's physical rotation. */
internal object MenuNavigationPolicy {
    enum class Move {
        PreviousTab, NextTab, EnterContent, ExitContent,
        Up, Down, Left, Right, None,
    }

    fun move(horizontalTabs: Boolean, inContent: Boolean, dx: Int, dy: Int): Move {
        if (inContent) return when {
            dy < 0 -> Move.Up
            dy > 0 -> Move.Down
            dx < 0 -> Move.Left
            dx > 0 -> if (horizontalTabs) Move.Right else Move.ExitContent
            else -> Move.None
        }
        return if (horizontalTabs) when {
            dy > 0 -> Move.EnterContent
            dy < 0 -> Move.None
            dx < 0 -> Move.PreviousTab
            dx > 0 -> Move.NextTab
            else -> Move.None
        } else when {
            dy < 0 -> Move.PreviousTab
            dy > 0 -> Move.NextTab
            dx < 0 -> Move.EnterContent
            else -> Move.None
        }
    }
}
