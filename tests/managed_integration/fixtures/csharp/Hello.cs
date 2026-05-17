// Tier 1: Basic C# hello-world. Tests namespace, class, Main entry point,
// Console.WriteLine, and string interpolation.
using System;

namespace RetDecTest
{
    class Hello
    {
        static void Main(string[] args)
        {
            string name = args.Length > 0 ? args[0] : "World";
            Console.WriteLine($"Hello, {name}!");
            Console.WriteLine("C# fixture for RetDec.");
        }
    }
}
