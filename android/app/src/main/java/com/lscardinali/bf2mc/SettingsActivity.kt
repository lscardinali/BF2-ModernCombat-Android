package com.lscardinali.bf2mc

import android.os.Bundle
import android.view.KeyEvent
import android.view.View
import android.widget.Button
import android.widget.Switch
import androidx.activity.ComponentActivity

// The settings, opened from the home screen. They apply the next time the game starts. Touch or gamepad.
class SettingsActivity : ComponentActivity() {
    private lateinit var settings: GameSettings
    private lateinit var fill: Switch

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        findViewById<View>(R.id.settings_scroll).isFocusable = false  // gamepad focus starts on the switch
        settings = GameSettings(this)
        fill = findViewById(R.id.settings_fill)
        fill.isChecked = settings.fillScreen
        fill.setOnCheckedChangeListener { _, checked ->
            settings.fillScreen = checked
            settings.save()
        }
        val split = findViewById<Switch>(R.id.settings_split)
        split.isChecked = settings.splitScreen
        split.setOnCheckedChangeListener { _, checked ->
            settings.splitScreen = checked
            settings.save()
        }
        findViewById<Button>(R.id.settings_back).setOnClickListener { finish() }
        fill.requestFocus()
    }

    // gamepad A presses the focused control (B already falls back to BACK)
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_A) {
            // in touch mode (a new screen starts in it) nothing has focus: A presses the first control
            if (event.action == KeyEvent.ACTION_UP && event.repeatCount == 0) {
                val focused = currentFocus
                if (focused is Button && !window.decorView.isInTouchMode) focused.performClick() else fill.performClick()
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }
}
