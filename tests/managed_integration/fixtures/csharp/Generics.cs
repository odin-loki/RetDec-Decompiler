// Tier 4: Generics, interfaces, and constraints. Tests generic classes,
// where constraints, IComparable, and covariance.
using System;
using System.Collections.Generic;

namespace RetDecTest
{
    interface IRepository<T> where T : class
    {
        void Add(T item);
        IEnumerable<T> GetAll();
    }

    class InMemoryRepo<T> : IRepository<T> where T : class
    {
        private readonly List<T> _store = new();
        public void Add(T item) => _store.Add(item);
        public IEnumerable<T> GetAll() => _store;
    }

    static class Extensions
    {
        public static T MaxBy<T, TKey>(this IEnumerable<T> source, Func<T, TKey> selector)
            where TKey : IComparable<TKey>
        {
            T best = default!;
            TKey bestKey = default!;
            bool first = true;
            foreach (var item in source)
            {
                var key = selector(item);
                if (first || key.CompareTo(bestKey) > 0) { best = item; bestKey = key; first = false; }
            }
            return best;
        }
    }

    class Generics
    {
        static void Main()
        {
            var repo = new InMemoryRepo<string>();
            repo.Add("hello"); repo.Add("world"); repo.Add("generics");
            foreach (var s in repo.GetAll()) Console.WriteLine(s);

            var longest = repo.GetAll().MaxBy(s => s.Length);
            Console.WriteLine("longest=" + longest);
        }
    }
}
