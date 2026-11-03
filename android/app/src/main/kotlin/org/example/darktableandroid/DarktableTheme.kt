/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LocalTonalElevationEnabled
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
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
internal fun DarktableTheme(content: @Composable () -> Unit) {
    CompositionLocalProvider(LocalTonalElevationEnabled provides false) {
        MaterialTheme(colorScheme = DarktableColorScheme, shapes = DarktableShapes,
            typography = DarktableTypography, content = content)
    }
}
