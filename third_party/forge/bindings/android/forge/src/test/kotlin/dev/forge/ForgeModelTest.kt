package dev.forge

import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/** Runs on a device/emulator (connectedAndroidTest) or a host JVM with libforge_jni on java.library.path.
 *  Skips when FORGE_TEST_FPK (a model path) is not set. */
class ForgeModelTest {
    @Test fun abi() = assertTrue(ForgeModel.abi >= 3)

    @Test fun roundTrip() {
        val path = System.getenv("FORGE_TEST_FPK"); assumeTrue(path != null && File(path).exists())
        ForgeModel.open(path!!).use { m ->
            val ids = m.tokenize("hello", addBos = false)
            assertEquals("hello", m.detokenize(ids))
            val out = m.generate(m.tokenize("hi"), 4)
            assertTrue(out.size in 1..4)
            assertEquals(m.vocabSize.toInt(), m.forward(intArrayOf(out[0])).size)
        }
    }
}
