/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

import java.io.IOException
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Test

class RuntimeConfigurationTest {
    @Test fun recreatedEditorReusesConfigurationWhilePreviousSessionCloses() {
        val owner = RuntimeConfiguration()
        val paths = RuntimePaths("/private/data/v1", "/installed/lib", "/private/cache")
        var nativeCalls = 0
        val configure: (RuntimePaths) -> Unit = {
            nativeCalls++
            if(nativeCalls > 1) throw IOException("Previous image session is still active")
        }
        owner.configure(paths, configure)
        // A new ViewModel obtains fresh strings for the same installation. A
        // second native configure would fail until the previous owner closes.
        owner.configure(paths.copy(), configure)
        assertEquals(1, nativeCalls)
    }

    @Test fun failedConfigurationCanRetryAndDoesNotReplaceSuccessfulInstallation() {
        val owner = RuntimeConfiguration()
        val initial = RuntimePaths("/private/data/v1", "/installed/lib", "/private/cache")
        val updated = initial.copy(data = "/private/data/v2")
        var attempts = 0
        owner.configure(initial) { attempts++ }
        try {
            owner.configure(updated) { attempts++; throw IOException("Session is active") }
            fail("A changed runtime must propagate the native rejection")
        } catch(_: IOException) { }
        owner.configure(initial) { fail("The successful installation must remain configured") }
        owner.configure(updated) { attempts++ }
        assertEquals(3, attempts)
    }
}
