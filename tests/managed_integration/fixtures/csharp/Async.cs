// Tier 3: Async/await. Tests Task<T>, async state machines, exception
// propagation through async, and Task.WhenAll.
using System;
using System.Threading.Tasks;

namespace RetDecTest
{
    class Async
    {
        static async Task<int> FetchAsync(int id)
        {
            await Task.Delay(10);
            if (id < 0) throw new ArgumentException($"Bad id: {id}");
            return id * id;
        }

        static async Task<string> SafeFetch(int id)
        {
            try
            {
                int result = await FetchAsync(id);
                return $"ok:{result}";
            }
            catch (ArgumentException e)
            {
                return $"err:{e.Message}";
            }
        }

        static async Task Main()
        {
            Console.WriteLine(await SafeFetch(5));
            Console.WriteLine(await SafeFetch(-1));

            var tasks = new[] { FetchAsync(1), FetchAsync(2), FetchAsync(3) };
            int[] results = await Task.WhenAll(tasks);
            Console.WriteLine("all: " + string.Join(", ", results));
        }
    }
}
