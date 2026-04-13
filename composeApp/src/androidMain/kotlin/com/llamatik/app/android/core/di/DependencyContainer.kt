package com.llamatik.app.android.core.di

import android.content.pm.ApplicationInfo
import android.content.Context
import androidx.annotation.VisibleForTesting
import com.llamatik.app.di.appModule
import org.koin.android.ext.koin.androidContext
import org.koin.android.ext.koin.androidLogger
import org.koin.core.context.startKoin
import org.koin.core.logger.Level
import org.koin.core.module.Module

object DependencyContainer {
    @VisibleForTesting
    var testModules: MutableList<Module> = mutableListOf()

    private val internalModules =
        sequence {
            yieldAll(appModule())
            yieldAll(testModules)
        }

    @JvmStatic
    fun initialize(context: Context) {
        val isDebuggable = (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0

        startKoin {
            if (isDebuggable) {
                androidLogger(Level.ERROR)
            }
            androidContext(context)
            modules(internalModules.toList())
        }
    }
}
