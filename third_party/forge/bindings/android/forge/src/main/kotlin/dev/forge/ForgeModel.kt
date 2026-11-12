package dev.forge

/** Kotlin binding over FORGE's C ABI (forge.h) via libforge_jni.so. Not thread-safe: use one model per thread. */
class ForgeModel private constructor(private var handle: Long) : AutoCloseable {
    fun interface TokenListener { /** Return true to stop generation. */ fun onToken(token: Int, text: String): Boolean }

    data class Sampling(
        val temperature: Float = 0f, val topK: Int = 0, val topP: Float = 1f, val minP: Float = 0f,
        val repetitionPenalty: Float = 1f, val seed: Long = 0,
    )

    val vocabSize: Long get() = nativeVocab(live())
    val position: Long get() = nativePosition(live())

    fun tokenize(text: String, addBos: Boolean = true): IntArray = nativeTokenize(live(), text, addBos)
    fun detokenize(ids: IntArray): String = nativeDetokenize(live(), ids)
    fun forward(ids: IntArray): FloatArray = nativeForward(live(), ids)
    fun reset() = nativeReset(live())
    fun rewind(position: Long) = nativeRewind(live(), position)
    fun generate(prompt: IntArray, maxNew: Int, sampling: Sampling = Sampling(), listener: TokenListener? = null): IntArray =
        nativeGenerate(live(), prompt, maxNew, sampling.temperature, sampling.topK, sampling.topP, sampling.minP,
            sampling.repetitionPenalty, sampling.seed, listener)
    fun generateText(prompt: String, maxNew: Int, sampling: Sampling = Sampling(), listener: TokenListener? = null): String =
        detokenize(generate(tokenize(prompt), maxNew, sampling, listener))

    override fun close() { if (handle != 0L) { nativeClose(handle); handle = 0L } }
    private fun live(): Long { check(handle != 0L) { "ForgeModel is closed" }; return handle }

    companion object {
        init { System.loadLibrary("forge_jni") }
        /** Opens a .fpk. Set FORGE_DEVICE=vulkan (Os.setenv) before opening to use the GPU. */
        @JvmStatic fun open(path: String, context: Long = 2048, verify: Boolean = false) = ForgeModel(nativeOpen(path, context, verify))
        @JvmStatic val version: String get() = nativeVersion()
        @JvmStatic val abi: Int get() = nativeAbi()

        @JvmStatic private external fun nativeVersion(): String
        @JvmStatic private external fun nativeAbi(): Int
        @JvmStatic private external fun nativeOpen(path: String, ctx: Long, verify: Boolean): Long
        @JvmStatic private external fun nativeClose(h: Long)
        @JvmStatic private external fun nativeReset(h: Long)
        @JvmStatic private external fun nativeVocab(h: Long): Long
        @JvmStatic private external fun nativePosition(h: Long): Long
        @JvmStatic private external fun nativeRewind(h: Long, pos: Long)
        @JvmStatic private external fun nativeTokenize(h: Long, text: String, bos: Boolean): IntArray
        @JvmStatic private external fun nativeDetokenize(h: Long, ids: IntArray): String
        @JvmStatic private external fun nativeForward(h: Long, ids: IntArray): FloatArray
        @JvmStatic private external fun nativeGenerate(h: Long, prompt: IntArray, maxNew: Int, temp: Float, topK: Int, topP: Float,
            minP: Float, rep: Float, seed: Long, cb: TokenListener?): IntArray
    }
}
