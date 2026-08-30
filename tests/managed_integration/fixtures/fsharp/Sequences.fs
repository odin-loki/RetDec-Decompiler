// Tier 2: Sequences and pipelines. Tests F# seq computation expressions,
// pipeline operators, higher-order functions, and list comprehensions.
module Sequences

let primes limit =
    let sieve = Array.create (limit + 1) true
    sieve.[0] <- false
    sieve.[1] <- false
    for i in 2 .. int (sqrt (float limit)) do
        if sieve.[i] then
            let mutable j = i * i
            while j <= limit do
                sieve.[j] <- false
                j <- j + i
    seq { for i in 2..limit do if sieve.[i] then yield i }

let fibonacci () =
    Seq.unfold (fun (a, b) -> Some(a, (b, a + b))) (0, 1)

