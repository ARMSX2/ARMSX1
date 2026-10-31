package com.armsx2.core

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class Ps1MemoryCardsSessionTest {
    @Test
    fun cancelledLaunchDoesNotWaitForCardImport() {
        val cancelled = AtomicBoolean(false)
        val ran = AtomicBoolean(false)
        val finished = CountDownLatch(1)
        Ps1MemoryCards.sessionLock.lock()
        val worker = thread {
            try {
                Ps1MemoryCards.runSession({ cancelled.get() }) {
                    ran.set(true)
                    true
                }
            } finally {
                finished.countDown()
            }
        }
        try {
            cancelled.set(true)
            assertTrue(finished.await(2, TimeUnit.SECONDS))
            assertFalse(ran.get())
        } finally {
            Ps1MemoryCards.sessionLock.unlock()
            worker.join(2000)
        }
    }

    @Test
    fun sessionExcludesCardMutationAndReleasesLock() {
        val acquired = AtomicBoolean(true)
        assertTrue(Ps1MemoryCards.runSession({ false }) {
            val worker = thread {
                acquired.set(Ps1MemoryCards.sessionLock.tryLock())
                if (acquired.get()) Ps1MemoryCards.sessionLock.unlock()
            }
            worker.join(2000)
            assertFalse(worker.isAlive)
            !acquired.get()
        })
        assertFalse(Ps1MemoryCards.sessionLock.isLocked)
    }

    @Test
    fun cancelledOrFailedSessionReleasesLock() {
        assertFalse(Ps1MemoryCards.runSession({ true }) { error("must not run") })
        val failed = runCatching {
            Ps1MemoryCards.runSession({ false }) { error("native startup failed") }
        }
        assertTrue(failed.isFailure)
        assertFalse(Ps1MemoryCards.sessionLock.isLocked)
    }
}
