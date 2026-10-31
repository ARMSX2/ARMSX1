package com.armsx2

import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticExportGateTest {
    @Test
    fun concurrentExportReturnsBusyWithoutEnteringWriter() {
        val gate = DiagnosticExportGate()
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val first = FutureTask {
            gate.run {
                entered.countDown()
                release.await(5, TimeUnit.SECONDS)
            }
        }
        Thread(first).start()
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            assertEquals(DiagnosticExportGate.Result.BUSY, gate.run { error("Second writer entered") })
        } finally {
            release.countDown()
        }
        assertEquals(DiagnosticExportGate.Result.SUCCESS, first.get(2, TimeUnit.SECONDS))
        assertEquals(DiagnosticExportGate.Result.SUCCESS, gate.run { true })
    }

    @Test
    fun failedAndThrowingExportsReleaseOwnership() {
        val gate = DiagnosticExportGate()
        assertEquals(DiagnosticExportGate.Result.FAILED, gate.run { false })
        assertEquals(DiagnosticExportGate.Result.FAILED, gate.run { throw IOException("Provider unavailable") })
        assertEquals(DiagnosticExportGate.Result.SUCCESS, gate.run { true })
    }

    @Test
    fun recursiveExportCannotReenterWriter() {
        val gate = DiagnosticExportGate()
        val result = gate.run {
            assertEquals(DiagnosticExportGate.Result.BUSY, gate.run { error("Nested writer entered") })
            true
        }
        assertEquals(DiagnosticExportGate.Result.SUCCESS, result)
    }
}
