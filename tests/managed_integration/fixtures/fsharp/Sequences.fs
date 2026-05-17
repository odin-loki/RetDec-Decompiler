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

[<EntryPoint>]
let main _ =
    let ps = primes 50 |> Seq.toList
    printfn "primes up to 50: %A" ps

    let fibs = fibonacci () |> Seq.take 10 |> Seq.toList
    printfn "first 10 fibs: %A" fibs

    let result =
        [1..20]
        |> List.filter (fun n -> n % 2 = 0)
        |> List.map (fun n -> n * n)
        |> List.sum
    printfn "sum of squares of evens 1..20 = %d" result
    0
