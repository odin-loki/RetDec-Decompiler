# Tier 4: Generators and itertools. Tests yield, yield from, send(),
# generator expressions, and itertools combinatorics.
import itertools
from typing import Generator, Iterator

def countdown(n: int) -> Generator[int, None, None]:
    while n > 0:
        yield n
        n -= 1

def fibonacci() -> Iterator[int]:
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b

def flatten(nested) -> Generator:
    for item in nested:
        if isinstance(item, (list, tuple)):
            yield from flatten(item)
        else:
            yield item

def running_average() -> Generator[float, float, None]:
    total = count = 0
    while True:
        value = yield total / count if count else 0.0
        if value is not None:
            total += value
            count += 1

if __name__ == "__main__":
    print("countdown:", list(countdown(5)))

    fibs = list(itertools.islice(fibonacci(), 10))
    print("fibs:", fibs)

    nested = [1, [2, [3, 4]], 5, [6, 7]]
    print("flat:", list(flatten(nested)))

    gen = running_average()
    next(gen)
    for v in [10, 20, 30, 40]:
        avg = gen.send(v)
        print(f"  running avg after {v}: {avg:.2f}")
