/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

internal data class RuntimePaths(val data: String, val modules: String, val temporary: String)

/** One native configuration per process, retained when an Activity is recreated.
 * Native sessions outlive a ViewModel briefly while its asynchronous close runs.
 * Reusing an already successful configuration avoids changing it during that close.
 */
internal class RuntimeConfiguration {
    private var configured: RuntimePaths? = null

    @Synchronized
    fun configure(paths: RuntimePaths, apply: (RuntimePaths) -> Unit) {
        if(configured == paths) return
        apply(paths)
        configured = paths
    }
}
