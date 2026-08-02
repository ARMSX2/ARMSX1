// SPDX-FileCopyrightText: 2026 ARMSX contributors
// SPDX-License-Identifier: MIT
//
// Licensed MIT, deliberately, matching the rest of the Discord helper. This file belongs to the
// process that links Discord's proprietary Social SDK; keeping this side permissive is what lets
// the emulator side consume the shared IPC definitions without the two becoming one program.
//
// Copyright (c) 2026 ARMSX contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.

package com.armsx2.discord

import android.app.Activity
import android.os.Bundle
import android.util.Log

/**
 * Invisible Activity that exists purely to give the SDK an Activity in ITS OWN process.
 *
 * The SDK launches the browser by calling startActivity on an Activity handed to
 * DiscordSocialSdkInit. Since the SDK lives in :discord, that Activity has to live there too —
 * ARMSX's Main is in another process and the reference would be meaningless here.
 *
 * It binds itself, kicks off authorization and finishes immediately. The browser hand-back lands on
 * the SDK's own AuthenticationActivity (also declared in :discord, with the discord-<app id>
 * scheme), so nothing further is needed here; the app process learns the outcome from the next
 * state poll like any other change.
 */
class DiscordAuthActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (!DiscordNative.load()) {
            finish()
            return
        }

        // Touching any com.discord class runs DiscordSocialSdkInit's static initializer, which
        // System.loadLibrary's the SDK and runs its JNI_OnLoad — a path that ABORTS the process on
        // failure rather than throwing, so runCatching cannot save us. Doing it here means the
        // blast radius is this helper process, never the emulator.
        runCatching { com.discord.socialsdk.DiscordSocialSdkInit.setEngineActivity(this) }
            .onFailure { Log.w(TAG, "setEngineActivity failed: ${it.message}") }

        runCatching { DiscordNative.authorize() }
            .onFailure { Log.w(TAG, "authorize failed: ${it.message}") }

        finish()
        overridePendingTransition(0, 0)
    }

    private companion object {
        const val TAG = "ARMSXDiscordSvc"
    }
}
