package com.lscardinali.bf2mc

import android.content.Context
import java.io.File

// <base>/user/settings.toml, where <base> is the folder that holds the game, chosen as app_main.cpp chooses it.
// Renderer::Init reads it when the game starts.
class GameSettings(context: Context) {
    private val file: File = File(
        File(
            listOf(context.filesDir, context.getExternalFilesDir(null)).firstOrNull { GameImport.installed(it) }
                ?: context.filesDir,
            "user"
        ),
        "settings.toml"
    )

    var fillScreen: Boolean = false
    var splitScreen: Boolean = false

    init {
        if (file.isFile) {
            for (line in file.readLines()) {
                val parts = line.split('=', limit = 2)
                if (parts.size == 2 && parts[0].trim() == "split_screen") splitScreen = parts[1].trim() == "true"
                if (parts.size == 2 && parts[0].trim() == "fill_screen") fillScreen = parts[1].trim() == "true"
            }
        }
    }

    fun save() {
        file.parentFile?.mkdirs()
        val temp = File(file.path + ".new")
        temp.writeText("fill_screen = $fillScreen\nsplit_screen = $splitScreen\n")
        temp.renameTo(file)
    }
}
