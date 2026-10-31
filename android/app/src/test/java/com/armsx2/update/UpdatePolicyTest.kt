package com.armsx2.update

import org.junit.Assert.*
import org.junit.Test

class UpdatePolicyTest {
    @Test fun stableReleaseSelectionUsesVersionsAndPreservesChannelChoice() {
        val stable = UpdatePolicy.Release("v0.3.20", false, 1793480000, "stable")
        val nightly = UpdatePolicy.Release("nightly-20261102.1", true, 1793570000, "nightly")
        assertEquals(stable, UpdatePolicy.select(listOf(nightly, stable), "0.3.19", 92183375, false))
        assertEquals(nightly, UpdatePolicy.select(listOf(stable, nightly), "0.3.19", 92183375, true))
        assertNull(UpdatePolicy.select(listOf(stable), "0.3.20", 93486539, false))
    }
    @Test fun anUpdateMustKeepReleaseIdentityAndOwnEngine() {
        val installed = UpdatePolicy.Archive("com.nanodata.armsx", 92183375, setOf("release-key"))
        val update = installed.copy(code = 93486539)
        assertNull(UpdatePolicy.archiveError(installed, update))
        assertNotNull(UpdatePolicy.archiveError(installed, update.copy(packageName = "test.app")))
        assertNotNull(UpdatePolicy.archiveError(installed, update.copy(certificates = setOf("other-key"))))
        assertNotNull(UpdatePolicy.archiveError(installed, update.copy(code = installed.code)))
        assertNotNull(UpdatePolicy.archiveError(installed, update.copy(compatibleEngine = false)))
    }
    @Test fun releaseAssetSelectionDoesNotInstallPrivateOrWrongArchitectureApks() {
        fun asset(name: String) = UpdatePolicy.Asset(name, "https://github.com/ARMSX2/ARMSX1/releases/download/v0.3.20/$name")
        val release = asset("ArmSX1-arm64.apk")
        assertEquals(release, UpdatePolicy.pickAsset(listOf(asset("ArmSX1-test-arm64.apk"), asset("ArmSX1-x86.apk"), release)))
        assertNull(UpdatePolicy.pickAsset(listOf(asset("ArmSX1-test-arm64.apk"))))
    }
}
