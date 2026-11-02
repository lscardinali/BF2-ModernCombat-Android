package com.lscardinali.bf2mc

import org.libsdl.app.SDLActivity

// SDL3's activity: loads librexruntime.so (which holds SDL) and libmain.so (src/main/cpp), whose SDL_main runs the game
class MainActivity : SDLActivity() {
    override fun getLibraries(): Array<String> = arrayOf("rexruntime", "main")
}
