package com.luoli.modifier

import android.app.Application
import com.luoli.modifier.core.CrashHandler
import com.topjohnwu.superuser.Shell

class LuoliApp : Application() {
    companion object {
        init {
            Shell.enableVerboseLogging = false
            Shell.setDefaultBuilder(
                Shell.Builder.create()
                    .setFlags(Shell.FLAG_MOUNT_MASTER)
                    .setTimeout(10)
            )
        }
    }

    override fun onCreate() {
        super.onCreate()
        CrashHandler.install(this)
    }
}
