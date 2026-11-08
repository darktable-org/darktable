/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.SharedPreferences
import android.os.Build
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

internal enum class AppTheme(val tag: String, val label: Int, val platformTheme: Int) {
    DESKTOP("desktop", R.string.app_theme_desktop, R.style.Theme_MobileRawEditor),
    LIGHT("light", R.string.app_theme_light, R.style.Theme_MobileRawEditor_Light),
    OLED("oled", R.string.app_theme_oled, R.style.Theme_MobileRawEditor_OLED);

    companion object {
        fun fromTag(tag: String?): AppTheme = entries.firstOrNull { it.tag == tag } ?: DESKTOP
    }
}

internal object AppThemes {
    private const val PREFERENCES = "app-theme"
    internal const val KEY = "theme"

    internal fun preferences(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)

    fun selected(context: Context): AppTheme = AppTheme.fromTag(preferences(context).getString(KEY, null))

    fun select(context: Context, theme: AppTheme) {
        preferences(context).edit().putString(KEY, theme.tag).apply()
    }
}

/** SharedPreferences also refresh an already-open editor without restarting its Activity. */
@Composable
internal fun rememberAppTheme(): AppTheme {
    val context = LocalContext.current.applicationContext
    val preferences = remember(context) { AppThemes.preferences(context) }
    var selected by remember(preferences) { mutableStateOf(AppThemes.selected(context)) }
    DisposableEffect(preferences) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if(key == AppThemes.KEY) selected = AppThemes.selected(context)
        }
        preferences.registerOnSharedPreferenceChangeListener(listener)
        selected = AppThemes.selected(context)
        onDispose { preferences.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    return selected
}

@Suppress("DEPRECATION")
@Composable
internal fun AppThemeSystemBars(theme: AppTheme, toolbar: Color) {
    val context = LocalContext.current
    val view = LocalView.current
    if(view.isInEditMode) return
    val activity = context.themeActivity() ?: return
    SideEffect {
        activity.setTheme(theme.platformTheme)
        val window = activity.window
        window.decorView.setBackgroundColor(toolbar.toArgb())
        window.statusBarColor = toolbar.toArgb()
        window.navigationBarColor = toolbar.toArgb()
        val controller = WindowCompat.getInsetsController(window, view)
        controller.isAppearanceLightStatusBars = theme == AppTheme.LIGHT
        controller.isAppearanceLightNavigationBars = theme == AppTheme.LIGHT
        if(Build.VERSION.SDK_INT >= 29) window.isNavigationBarContrastEnforced = theme != AppTheme.OLED
    }
}

private tailrec fun Context.themeActivity(): Activity? = when(this) {
    is Activity -> this
    is ContextWrapper -> baseContext.themeActivity()
    else -> null
}
