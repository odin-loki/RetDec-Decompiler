// Tier 2: LINQ queries. Tests deferred execution, query syntax vs method syntax,
// GroupBy, OrderBy, and anonymous types.
using System;
using System.Collections.Generic;
using System.Linq;

namespace RetDecTest
{
    record Person(string Name, int Age, string City);

    class Linq
    {
        static void Main()
        {
            var people = new List<Person>
            {
                new("Alice", 30, "London"),
                new("Bob",   25, "Paris"),
                new("Carol", 35, "London"),
                new("Dave",  28, "Paris"),
                new("Eve",   32, "Berlin"),
            };

            // Method syntax
            var londoners = people
                .Where(p => p.City == "London")
                .OrderBy(p => p.Age)
                .Select(p => p.Name);
            Console.WriteLine("London: " + string.Join(", ", londoners));

            // Query syntax
            var grouped =
                from p in people
                group p by p.City into g
                select new { City = g.Key, Count = g.Count(), AvgAge = g.Average(x => x.Age) };

            foreach (var g in grouped.OrderBy(g => g.City))
                Console.WriteLine($"{g.City}: count={g.Count}, avgAge={g.AvgAge:F1}");
        }
    }
}
