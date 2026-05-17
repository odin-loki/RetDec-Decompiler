import kotlinx.coroutines.*

/**
 * Tier 3: Coroutines. Tests suspend functions, launch/async, structured
 * concurrency, and flow patterns.
 */
suspend fun fetchValue(id: Int): Int {
    delay(10)
    return id * id
}

fun main() = runBlocking {
    // Sequential
    val a = fetchValue(3)
    val b = fetchValue(4)
    println("seq: $a + $b = ${a + b}")

    // Concurrent with async
    val dA = async { fetchValue(5) }
    val dB = async { fetchValue(6) }
    println("async: ${dA.await()} + ${dB.await()}")

    // Launch multiple
    val jobs = (1..5).map { i ->
        launch {
            val v = fetchValue(i)
            println("job[$i]=$v")
        }
    }
    jobs.forEach { it.join() }
}
