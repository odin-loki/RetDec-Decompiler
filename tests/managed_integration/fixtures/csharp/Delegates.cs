// Tier 5: Delegates, events, and Func/Action. Tests multicast delegates,
// event add/remove, Func<> composition, and anonymous methods.
using System;
using System.Collections.Generic;

namespace RetDecTest
{
    delegate int BinaryOp(int a, int b);

    class EventSource
    {
        public event Action<string>? OnMessage;
        public void Emit(string msg) => OnMessage?.Invoke(msg);
    }

    class Delegates
    {
        static Func<int, int> Compose(Func<int, int> f, Func<int, int> g) => x => f(g(x));

        static void Main()
        {
            BinaryOp add = (a, b) => a + b;
            BinaryOp mul = (a, b) => a * b;
            BinaryOp combined = add + mul;  // multicast
            Console.WriteLine("add(3,4)=" + add(3, 4));
            Console.WriteLine("mul(3,4)=" + mul(3, 4));

            var src = new EventSource();
            var log = new List<string>();
            src.OnMessage += msg => log.Add("A:" + msg);
            src.OnMessage += msg => log.Add("B:" + msg);
            src.Emit("hello");
            src.Emit("world");
            log.ForEach(Console.WriteLine);

            Func<int, int> addOne = x => x + 1;
            Func<int, int> triple = x => x * 3;
            Func<int, int> composed = Compose(triple, addOne);  // triple(addOne(x))
            Console.WriteLine("composed(4)=" + composed(4));    // 15
        }
    }
}
