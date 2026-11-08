/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LocalTonalElevationEnabled
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/** Neutral roles from data/themes/darktable-elegant-grey.css and darktable.css. */
internal object DarktableColors {
    val Background = Color(0xFF6A6A6A)
    val Panel = Color(0xFF717171)
    val Preview = Color(0xFF777777)
    val Toolbar = Background
    val Text = Color(0xFFF1F1F1)
    val Label = Color(0xFFC6C6C6)
    val Border = Color(0xFF5E5E5E)
    val Button = Color(0xFF7D7D7D)
    val Active = Color(0xFFABABAB)
    val DarkText = Color(0xFF262626)
    val Selection = Color(0xFF525252)
    val White = Color(0xFFFFFFFF)
}

// Every surface/accent role is explicit: platform or Material defaults must not
// introduce colored casts around the photo. The error roles retain their meaning.
internal val DarktableColorScheme = darkColorScheme(
    primary = DarktableColors.White,
    onPrimary = DarktableColors.DarkText,
    primaryContainer = DarktableColors.Active,
    onPrimaryContainer = DarktableColors.DarkText,
    inversePrimary = DarktableColors.Border,
    secondary = DarktableColors.Text,
    onSecondary = DarktableColors.DarkText,
    secondaryContainer = DarktableColors.Selection,
    onSecondaryContainer = DarktableColors.White,
    tertiary = DarktableColors.Active,
    onTertiary = DarktableColors.DarkText,
    tertiaryContainer = DarktableColors.Selection,
    onTertiaryContainer = DarktableColors.White,
    background = DarktableColors.Background,
    onBackground = DarktableColors.Text,
    surface = DarktableColors.Panel,
    onSurface = DarktableColors.White,
    surfaceVariant = DarktableColors.Border,
    onSurfaceVariant = DarktableColors.White,
    surfaceTint = DarktableColors.Panel,
    inverseSurface = DarktableColors.Text,
    inverseOnSurface = DarktableColors.DarkText,
    error = Color(0xFFFFDAD6),
    onError = Color(0xFF410E0B),
    errorContainer = Color(0xFF73342D),
    onErrorContainer = Color(0xFFFFDAD6),
    outline = DarktableColors.Border,
    outlineVariant = Color(0xFF848484),
    scrim = Color.Black,
    surfaceBright = Color(0xFF848484),
    surfaceDim = DarktableColors.Background,
    surfaceContainer = DarktableColors.Panel,
    surfaceContainerHigh = DarktableColors.Button,
    surfaceContainerHighest = Color(0xFF848484),
    surfaceContainerLow = DarktableColors.Background,
    surfaceContainerLowest = DarktableColors.Selection,
)

private val LightEditorColorScheme = lightColorScheme(
    primary = Color(0xFF202020),
    onPrimary = Color.White,
    primaryContainer = Color(0xFF767676),
    onPrimaryContainer = Color.White,
    inversePrimary = Color(0xFFE0E0E0),
    secondary = Color(0xFF303030),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFE0E0E0),
    onSecondaryContainer = Color(0xFF202020),
    tertiary = Color(0xFF606060),
    onTertiary = Color.White,
    tertiaryContainer = Color(0xFFD8D8D8),
    onTertiaryContainer = Color(0xFF202020),
    background = Color(0xFFFAFAFA),
    onBackground = Color(0xFF202020),
    surface = Color.White,
    onSurface = Color(0xFF202020),
    surfaceVariant = Color(0xFFE6E6E6),
    onSurfaceVariant = Color(0xFF404040),
    surfaceTint = Color.White,
    inverseSurface = Color(0xFF303030),
    inverseOnSurface = Color(0xFFF1F1F1),
    error = Color(0xFFB3261E),
    onError = Color.White,
    errorContainer = Color(0xFFF9DEDC),
    onErrorContainer = Color(0xFF410E0B),
    outline = Color(0xFF767676),
    outlineVariant = Color(0xFFC0C0C0),
    scrim = Color.Black,
    surfaceBright = Color.White,
    surfaceDim = Color(0xFFE6E6E6),
    surfaceContainer = Color.White,
    surfaceContainerHigh = Color(0xFFF1F1F1),
    surfaceContainerHighest = Color(0xFFE6E6E6),
    surfaceContainerLow = Color(0xFFFAFAFA),
    surfaceContainerLowest = Color.White,
)

private val OledEditorColorScheme = darkColorScheme(
    primary = Color(0xFFF1F1F1),
    onPrimary = Color.Black,
    primaryContainer = Color(0xFFB5B5B5),
    onPrimaryContainer = Color(0xFF202020),
    inversePrimary = Color(0xFF303030),
    secondary = Color(0xFFD0D0D0),
    onSecondary = Color.Black,
    secondaryContainer = Color(0xFF303030),
    onSecondaryContainer = Color(0xFFF1F1F1),
    tertiary = Color(0xFFA0A0A0),
    onTertiary = Color.Black,
    tertiaryContainer = Color(0xFF303030),
    onTertiaryContainer = Color(0xFFF1F1F1),
    background = Color.Black,
    onBackground = Color(0xFFF1F1F1),
    surface = Color.Black,
    onSurface = Color(0xFFF1F1F1),
    surfaceVariant = Color(0xFF161616),
    onSurfaceVariant = Color(0xFFD0D0D0),
    surfaceTint = Color.Black,
    inverseSurface = Color(0xFFF1F1F1),
    inverseOnSurface = Color(0xFF202020),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF410002),
    onErrorContainer = Color(0xFFFFDAD6),
    outline = Color(0xFF666666),
    outlineVariant = Color(0xFF303030),
    scrim = Color.Black,
    surfaceBright = Color(0xFF242424),
    surfaceDim = Color.Black,
    surfaceContainer = Color.Black,
    surfaceContainerHigh = Color(0xFF181818),
    surfaceContainerHighest = Color(0xFF242424),
    surfaceContainerLow = Color.Black,
    surfaceContainerLowest = Color.Black,
)

/** Paint-only workspace roles; choosing a palette never changes control geometry. */
internal data class EditorChromeColors(
    val panel: Color,
    val toolbar: Color,
    val preview: Color,
    val selection: Color,
)

internal fun appThemeScheme(theme: AppTheme): ColorScheme = when(theme) {
    AppTheme.DESKTOP -> DarktableColorScheme
    AppTheme.LIGHT -> LightEditorColorScheme
    AppTheme.OLED -> OledEditorColorScheme
}

internal fun appThemeChrome(theme: AppTheme): EditorChromeColors = when(theme) {
    AppTheme.DESKTOP -> EditorChromeColors(DarktableColors.Panel, DarktableColors.Toolbar,
        DarktableColors.Preview, DarktableColors.Selection)
    AppTheme.LIGHT -> EditorChromeColors(Color.White, Color(0xFFFAFAFA),
        Color(0xFFE0E0E0), Color(0xFFE0E0E0))
    AppTheme.OLED -> EditorChromeColors(Color.Black, Color.Black, Color.Black, Color(0xFF303030))
}

internal val LocalEditorChromeColors = staticCompositionLocalOf { appThemeChrome(AppTheme.DESKTOP) }

private val DarktableShapes = Shapes(
    extraSmall = RoundedCornerShape(3.dp),
    small = RoundedCornerShape(3.dp),
    medium = RoundedCornerShape(3.dp),
    large = RoundedCornerShape(3.dp),
    extraLarge = RoundedCornerShape(3.dp),
)

// Keep Material's existing Android font metrics, sizes, weights and line heights.
private val DarktableTypography = Typography()

@Composable
internal fun DarktableTheme(theme: AppTheme? = null, content: @Composable () -> Unit) {
    val selected = theme ?: rememberAppTheme()
    val chrome = appThemeChrome(selected)
    AppThemeSystemBars(selected, chrome.toolbar)
    CompositionLocalProvider(LocalTonalElevationEnabled provides false, LocalEditorChromeColors provides chrome) {
        MaterialTheme(colorScheme = appThemeScheme(selected), shapes = DarktableShapes,
            typography = DarktableTypography, content = content)
    }
}
