// Tier 1: Basic F# hello-world. Tests let bindings, printfn,
// discriminated unions, and pattern matching.
module Hello

type Greeting =
    | Formal of string
    | Casual of string
    | None

let greet greeting =
    match greeting with
    | Formal name -> sprintf "Good day, %s." name
    | Casual name -> sprintf "Hey, %s!" name
    | None        -> "Hello, stranger!"

[<EntryPoint>]
let main _ =
    printfn "%s" (greet (Formal "World"))
    printfn "%s" (greet (Casual "friend"))
    printfn "%s" (greet None)
    0
