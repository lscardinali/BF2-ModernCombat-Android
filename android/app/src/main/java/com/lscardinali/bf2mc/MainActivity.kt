package com.lscardinali.bf2mc

import org.libsdl.app.SDLActivity

// SDL3's activity: loads librexruntime.so (which holds SDL) and libmain.so (src/main/cpp), whose SDL_main runs the game
class MainActivity : SDLActivity() {
    override fun getLibraries(): Array<String> = arrayOf("rexruntime", "main")

    // onPause still counts as in the foreground, so the service may start here (KeepAliveService)
    override fun onPause() {
        super.onPause()
        if (!isFinishing) KeepAliveService.start(this)
    }

    override fun onResume() {
        super.onResume()
        KeepAliveService.stop(this)
    }

    override fun onDestroy() {
        KeepAliveService.stop(this)
        super.onDestroy()
    }
}
