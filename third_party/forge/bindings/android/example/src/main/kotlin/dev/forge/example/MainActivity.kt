package dev.forge.example

import android.app.Activity
import android.os.Bundle
import android.system.Os
import android.widget.TextView
import dev.forge.ForgeModel
import java.io.File
import kotlin.concurrent.thread

/** Copies assets/model.fpk to app storage, opens it on Vulkan (CPU fallback) and streams a completion. */
class MainActivity : Activity() {
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val view = TextView(this).apply { textSize = 16f; text = "FORGE ${ForgeModel.version}\n" }
        setContentView(view)
        thread {
            val f = File(filesDir, "model.fpk")
            if (!f.exists()) runCatching { assets.open("model.fpk").use { i -> f.outputStream().use { i.copyTo(it) } } }
            if (!f.exists()) { runOnUiThread { view.append("put a model at assets/model.fpk\n") }; return@thread }
            Os.setenv("FORGE_DEVICE", "vulkan", true)
            ForgeModel.open(f.path).use { m ->
                m.generateText("Once upon a time", 64, ForgeModel.Sampling(temperature = 0.7f, topP = 0.9f)) { _, t ->
                    runOnUiThread { view.append(t) }; false
                }
            }
        }
    }
}
