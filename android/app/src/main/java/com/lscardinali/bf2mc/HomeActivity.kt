package com.lscardinali.bf2mc

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import java.io.File
import kotlin.concurrent.thread

// The launcher activity. With the game files in place (<files>/game/default.xex, the same check app_main.cpp makes)
// it offers Play and Settings. Otherwise it asks for the disc image with the system file picker (no storage permission)
// and extracts it into <files>/game (GameImport). Touch or gamepad: D-pad moves focus, A presses, B is back.
class HomeActivity : ComponentActivity() {
    private lateinit var message: TextView
    private lateinit var detail: TextView
    private lateinit var progress: ProgressBar
    private lateinit var primary: Button
    private lateinit var settings: Button
    private lateinit var quit: Button

    @Volatile private var cancel = false
    private var worker: Thread? = null

    private val pick = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) startImport(uri) else primary.requestFocus()
    }

    // back does nothing while importing (Cancel stops it)
    private val busyBack = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() {}
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        GameImport.deleteTree(File(filesDir, GameImport.TEMP))  // an import that was interrupted
        setContentView(R.layout.activity_home)
        findViewById<View>(R.id.home_scroll).isFocusable = false  // gamepad focus starts on the first button
        message = findViewById(R.id.home_message)
        detail = findViewById(R.id.home_detail)
        progress = findViewById(R.id.home_progress)
        primary = findViewById(R.id.home_primary)
        settings = findViewById(R.id.home_settings)
        quit = findViewById(R.id.home_quit)
        primary.setOnClickListener {
            if (worker == null) {
                if (installed()) startActivity(Intent(this, MainActivity::class.java)) else openPicker()
            }
        }
        settings.setOnClickListener { startActivity(Intent(this, SettingsActivity::class.java)) }
        quit.setOnClickListener { if (worker != null) cancel = true else finish() }
        onBackPressedDispatcher.addCallback(this, busyBack)
        showState()
    }

    override fun onDestroy() {
        cancel = true
        super.onDestroy()
    }

    // gamepad A presses the focused button (B already falls back to BACK)
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_A) {
            // in touch mode (the app starts in it) nothing shows focus: A presses the default button
            if (event.action == KeyEvent.ACTION_UP && event.repeatCount == 0) {
                val focused = currentFocus
                if (focused is Button && focused.isEnabled && !window.decorView.isInTouchMode) focused.performClick()
                else defaultButton().performClick()
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun defaultButton(): Button = if (primary.isEnabled) primary else quit

    private fun installed() = GameImport.installed(filesDir) || GameImport.installed(getExternalFilesDir(null))

    private fun showState() {
        val ready = installed()
        message.visibility = if (ready) View.GONE else View.VISIBLE
        settings.visibility = if (ready) View.VISIBLE else View.GONE
        primary.setText(if (ready) R.string.home_play else R.string.import_choose)
        primary.requestFocus()
    }

    private fun openPicker() {
        try {
            pick.launch(arrayOf("*/*"))  // .iso has no reliable MIME type across providers
        } catch (e: ActivityNotFoundException) {
            message.setText(R.string.import_no_picker)
        }
    }

    private fun setBusy(busy: Boolean) {
        busyBack.isEnabled = busy
        primary.isEnabled = !busy
        quit.setText(if (busy) R.string.import_cancel else R.string.home_quit)
        progress.visibility = if (busy) View.VISIBLE else View.GONE
        detail.visibility = if (busy) View.VISIBLE else View.GONE
        if (busy) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        defaultButton().requestFocus()
    }

    private fun startImport(uri: Uri) {
        cancel = false
        message.setText(R.string.import_verifying)
        detail.text = ""
        progress.progress = 0
        setBusy(true)
        val dir = filesDir
        var lastPost = 0L
        var lastIndex = 0
        worker = thread(name = "GameImport") {
            var ok = false
            val error: String? = try {
                val pfd = contentResolver.openFileDescriptor(uri, "r") ?: throw java.io.IOException("can't open $uri")
                ParcelFileDescriptor.AutoCloseInputStream(pfd).use { stream ->
                    ok = GameImport.run(stream.channel, dir) { file, index, count, done, total ->
                        val now = SystemClock.uptimeMillis()
                        if (index != lastIndex || now - lastPost >= 100) {
                            lastPost = now
                            lastIndex = index
                            runOnUiThread { showProgress(file, index, count, done, total) }
                        }
                        !cancel
                    }
                }
                if (ok) null else getString(R.string.import_cancelled)
            } catch (e: GameImport.WrongDiscException) {
                getString(R.string.import_wrong_disc, e.sha256 ?: "no default.xex")
            } catch (e: Xdvdfs.NotXboxDiscException) {
                getString(R.string.import_not_xbox)
            } catch (e: GameImport.NoSpaceException) {
                getString(R.string.import_no_space, e.needed / 1e9, e.free / 1e9)
            } catch (e: Exception) {
                getString(R.string.import_failed, e.message ?: e.toString())
            }
            runOnUiThread {
                worker = null
                if (isDestroyed) return@runOnUiThread
                setBusy(false)
                if (ok) {
                    showState()
                } else {
                    message.text = error
                    primary.setText(R.string.import_choose_again)
                    primary.requestFocus()
                }
            }
        }
    }

    private fun showProgress(file: String, index: Int, count: Int, done: Long, total: Long) {
        if (worker == null || isDestroyed) return
        message.setText(if (index <= 1) R.string.import_verifying else R.string.import_running)
        progress.progress = if (total > 0) (done * progress.max / total).toInt() else 0
        detail.text = getString(R.string.import_detail, file, index, count, done / 1e9, total / 1e9)
    }
}
