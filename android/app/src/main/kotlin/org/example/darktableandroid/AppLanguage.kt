/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.app.Activity
import android.app.LocaleManager
import android.content.Context
import android.content.res.Configuration
import android.content.res.Resources
import android.os.Build
import android.os.LocaleList
import androidx.annotation.RequiresApi
import java.util.Locale

internal enum class AppLanguage(val tag: String, val label: Int) {
    SYSTEM("", R.string.app_language_system),
    ENGLISH("en", R.string.app_language_english),
    TRADITIONAL_CHINESE("zh-Hant", R.string.app_language_zh_hant),
    SIMPLIFIED_CHINESE("zh-Hans", R.string.app_language_zh_hans),
    LATIN_AMERICAN_SPANISH("es-419", R.string.app_language_es_419);

    companion object {
        fun fromTag(tag: String?): AppLanguage {
            if(tag.isNullOrBlank()) return SYSTEM
            val locale = Locale.forLanguageTag(tag.substringBefore(',').replace('_', '-'))
            return when(locale.language) {
                "en" -> ENGLISH
                "es" -> LATIN_AMERICAN_SPANISH
                "zh" -> if(locale.script.equals("Hant", true) ||
                    (!locale.script.equals("Hans", true) && locale.country in setOf("TW", "HK", "MO")))
                    TRADITIONAL_CHINESE else SIMPLIFIED_CHINESE
                else -> SYSTEM
            }
        }
    }
}

internal object AppLanguages {
    private const val PREFERENCES = "app-language"
    private const val LANGUAGE = "language-tag"

    fun selected(context: Context): AppLanguage = if(Build.VERSION.SDK_INT >= 33) {
        AppLanguage.fromTag(PlatformLocales.tags(context))
    } else saved(context)

    private fun saved(context: Context): AppLanguage = AppLanguage.fromTag(
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getString(LANGUAGE, ""))

    /** Legacy Android has no per-app LocaleManager; override only app resources. */
    fun wrap(context: Context): Context {
        if(Build.VERSION.SDK_INT >= 33) return context
        val configuration = configurationFor(context, saved(context))
        @Suppress("DEPRECATION")
        context.applicationContext?.resources?.updateConfiguration(configuration, context.resources.displayMetrics)
        return context.createConfigurationContext(configuration)
    }

    fun configurationFor(context: Context, language: AppLanguage): Configuration =
        Configuration(context.resources.configuration).apply {
            val locales = if(language == AppLanguage.SYSTEM) Resources.getSystem().configuration.locales
                else LocaleList.forLanguageTags(language.tag)
            setLocales(locales)
            if(!locales.isEmpty) setLayoutDirection(locales[0])
        }

    fun select(activity: Activity, language: AppLanguage) {
        if(selected(activity) == language) return
        activity.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putString(LANGUAGE, language.tag).apply()
        if(Build.VERSION.SDK_INT >= 33) PlatformLocales.select(activity, language.tag)
        else {
            // Activity recreation retains its ViewModel/native session and undo history.
            @Suppress("DEPRECATION")
            activity.applicationContext.resources.updateConfiguration(configurationFor(activity, language),
                activity.resources.displayMetrics)
            activity.recreate()
        }
    }

    @RequiresApi(33)
    private object PlatformLocales {
        fun tags(context: Context): String = context.getSystemService(LocaleManager::class.java)
            .applicationLocales.toLanguageTags()
        fun select(context: Context, tag: String) {
            context.getSystemService(LocaleManager::class.java).applicationLocales = LocaleList.forLanguageTags(tag)
        }
    }
}
