/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.app.LocaleManager
import android.content.Context
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.content.res.Resources
import android.os.Build
import android.os.LocaleList
import androidx.activity.ComponentActivity
import androidx.test.filters.SdkSuppress
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.util.ReflectionHelpers
import org.xmlpull.v1.XmlPullParser
import java.util.Locale

/** App resources and platform locale selection; no native image engine is started. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "en-rUS-w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@SdkSuppress(minSdkVersion = 33)
class AppLanguageTest {
    private val context: Context get() = RuntimeEnvironment.getApplication()

    private fun localized(language: AppLanguage): Context =
        context.createConfigurationContext(AppLanguages.configurationFor(context, language))

    @Test fun supportedConfigurationsResolveActualTranslatedResources() {
        val expected = mapOf(
            AppLanguage.ENGLISH to "Settings",
            AppLanguage.TRADITIONAL_CHINESE to "設定",
            AppLanguage.SIMPLIFIED_CHINESE to "设置",
            AppLanguage.LATIN_AMERICAN_SPANISH to "Configuración",
        )
        expected.forEach { (language, label) ->
            val translated = localized(language)
            assertEquals(label, translated.getString(R.string.workspace_tab_settings))
            assertEquals(language.tag, translated.resources.configuration.locales[0].toLanguageTag())
            assertEquals(Configuration.SCREENLAYOUT_LAYOUTDIR_LTR,
                translated.resources.configuration.screenLayout and Configuration.SCREENLAYOUT_LAYOUTDIR_MASK)
        }
    }

    @Test fun regionalLocalesResolveChineseScriptAndLatinAmericanFallbacks() {
        val expected = mapOf("zh-TW" to "設定", "zh-HK" to "設定", "zh-CN" to "设置",
            "es-MX" to "Configuración", "es-AR" to "Configuración")
        expected.forEach { (tag, label) ->
            val configuration = Configuration(context.resources.configuration).apply {
                setLocales(LocaleList.forLanguageTags(tag))
            }
            assertEquals("Incorrect resource fallback for $tag", label,
                context.createConfigurationContext(configuration).getString(R.string.workspace_tab_settings))
        }
    }

    @Test fun platformAndStoredRegionalTagsNormalizeToSupportedMenuChoices() {
        mapOf("en-US" to AppLanguage.ENGLISH, "en-GB" to AppLanguage.ENGLISH,
            "zh-TW" to AppLanguage.TRADITIONAL_CHINESE, "zh-HK" to AppLanguage.TRADITIONAL_CHINESE,
            "zh-Hant" to AppLanguage.TRADITIONAL_CHINESE, "zh-Hans-TW" to AppLanguage.SIMPLIFIED_CHINESE,
            "zh-CN" to AppLanguage.SIMPLIFIED_CHINESE, "es-MX" to AppLanguage.LATIN_AMERICAN_SPANISH,
            "es-AR,en-US" to AppLanguage.LATIN_AMERICAN_SPANISH).forEach { (tag, language) ->
            assertEquals(tag, language, AppLanguage.fromTag(tag))
        }
        listOf(null, "", "fr-FR", "invalid-language").forEach { tag ->
            assertEquals(AppLanguage.SYSTEM, AppLanguage.fromTag(tag))
        }
    }

    @Test fun selectingAnAppLocaleUsesPlatformStorageAndCanReturnToSystem() {
        shadowOf(context.packageManager).addOrUpdateActivity(ActivityInfo().apply {
            name = ComponentActivity::class.java.name
            packageName = context.packageName
            applicationInfo = context.applicationInfo
            theme = android.R.style.Theme_Material_Light_NoActionBar
            enabled = true
            exported = true
        })
        val controller = Robolectric.buildActivity(ComponentActivity::class.java).setup()
        try {
            AppLanguages.select(controller.get(), AppLanguage.LATIN_AMERICAN_SPANISH)
            val manager = context.getSystemService(LocaleManager::class.java)
            assertEquals("es-419", manager.applicationLocales.toLanguageTags())
            assertEquals(AppLanguage.LATIN_AMERICAN_SPANISH, AppLanguages.selected(context))
            // Simulate an external Android App Languages choice. It must take
            // precedence over an earlier selection stored by our own menu.
            manager.applicationLocales = LocaleList.forLanguageTags("zh-Hant")
            assertEquals(AppLanguage.TRADITIONAL_CHINESE, AppLanguages.selected(context))
            AppLanguages.select(controller.get(), AppLanguage.SYSTEM)
            assertTrue(manager.applicationLocales.isEmpty)
            assertEquals(AppLanguage.SYSTEM, AppLanguages.selected(context))
        } finally {
            controller.pause().stop().destroy()
        }
    }

    @Suppress("DEPRECATION")
    @Test fun legacyWrappingRestoresSavedLanguageAndDoesNotKeepItAfterSystemReset() {
        val app = context
        val originalSdk = Build.VERSION.SDK_INT
        val originalConfiguration = Configuration(app.resources.configuration)
        val originalSystemConfiguration = Configuration(Resources.getSystem().configuration)
        val originalJavaLocale = Locale.getDefault()
        val systemLabel = app.createConfigurationContext(Configuration(originalConfiguration).apply {
            setLocales(originalSystemConfiguration.locales)
        }).getString(R.string.workspace_tab_settings)
        val preferences = app.getSharedPreferences("app-language", Context.MODE_PRIVATE)
        val hadLanguage = preferences.contains("language-tag")
        val originalLanguage = preferences.getString("language-tag", null)
        try {
            // The runtime remains SDK 35; change only the branch guard so this
            // test exercises the pre-LocaleManager preference/context route.
            ReflectionHelpers.setStaticField(Build.VERSION::class.java, "SDK_INT", 32)
            preferences.edit().putString("language-tag", "zh-Hant").commit()
            val translated = AppLanguages.wrap(app)
            assertEquals(AppLanguage.TRADITIONAL_CHINESE, AppLanguages.selected(translated))
            assertEquals("zh-Hant", translated.resources.configuration.locales[0].toLanguageTag())
            assertEquals("設定", translated.getString(R.string.workspace_tab_settings))
            assertEquals("設定", app.getString(R.string.workspace_tab_settings))
            assertEquals(originalJavaLocale, Locale.getDefault())

            preferences.edit().putString("language-tag", "").commit()
            val restored = AppLanguages.wrap(app)
            assertEquals(AppLanguage.SYSTEM, AppLanguages.selected(restored))
            assertEquals(originalSystemConfiguration.locales.toLanguageTags(),
                restored.resources.configuration.locales.toLanguageTags())
            assertEquals(systemLabel, restored.getString(R.string.workspace_tab_settings))
            assertEquals(originalSystemConfiguration.locales.toLanguageTags(),
                Resources.getSystem().configuration.locales.toLanguageTags())
            assertEquals(originalJavaLocale, Locale.getDefault())
        } finally {
            ReflectionHelpers.setStaticField(Build.VERSION::class.java, "SDK_INT", originalSdk)
            preferences.edit().apply {
                if(hadLanguage) putString("language-tag", originalLanguage) else remove("language-tag")
            }.commit()
            app.resources.updateConfiguration(originalConfiguration, app.resources.displayMetrics)
            if(Resources.getSystem().configuration != originalSystemConfiguration)
                Resources.getSystem().updateConfiguration(originalSystemConfiguration, Resources.getSystem().displayMetrics)
            if(Locale.getDefault() != originalJavaLocale) Locale.setDefault(originalJavaLocale)
        }
    }

    @Test fun localeConfigurationAdvertisesExactlyTheMenuLanguages() {
        val advertised = mutableSetOf<String>()
        context.resources.getXml(R.xml.locales_config).use { xml ->
            while(xml.eventType != XmlPullParser.END_DOCUMENT) {
                if(xml.eventType == XmlPullParser.START_TAG && xml.name == "locale") {
                    advertised += requireNotNull(xml.getAttributeValue("http://schemas.android.com/apk/res/android", "name"))
                }
                xml.next()
            }
        }
        assertEquals(AppLanguage.entries.filter { it != AppLanguage.SYSTEM }.map { it.tag }.toSet(), advertised)
    }

    @Test fun translatedFormatStringsPreserveCameraNamesAndExposureValues() {
        AppLanguage.entries.filter { it != AppLanguage.SYSTEM }.forEach { language ->
            val translated = localized(language)
            val camera = "GoPro HERO12 Black / Caméra 7"
            val value = -.25
            val cameraLabel = translated.getString(R.string.lens_image_camera, camera)
            val toneLabel = translated.getString(R.string.tone_touch_sample, value)
            val expectedNumber = String.format(translated.resources.configuration.locales[0], "%.2f", value)
            assertTrue(cameraLabel.contains(camera))
            assertFalse(cameraLabel.contains("%1"))
            assertTrue("Incorrect localized EV for ${language.tag}", toneLabel.contains(expectedNumber))
            assertTrue(toneLabel.contains("EV"))
            assertFalse(toneLabel.contains("%1"))
        }
    }
}
