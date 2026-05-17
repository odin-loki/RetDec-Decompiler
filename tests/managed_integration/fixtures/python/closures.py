# Tier 5: Closures and decorators. Tests free variables, nonlocal,
# functools.wraps, parameterised decorators, and __closure__.
import functools
import time

def make_counter(start: int = 0):
    count = start
    def increment(by: int = 1) -> int:
        nonlocal count
        count += by
        return count
    def reset():
        nonlocal count
        count = start
    increment.reset = reset
    return increment

def retry(times: int, delay: float = 0.0):
    def decorator(fn):
        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            last_exc = None
            for attempt in range(times):
                try:
                    return fn(*args, **kwargs)
                except Exception as e:
                    last_exc = e
                    if delay > 0:
                        time.sleep(delay)
            raise last_exc
        return wrapper
    return decorator

_call_count = 0

@retry(times=3)
def flaky_fn(threshold: int) -> str:
    global _call_count
    _call_count += 1
    if _call_count < threshold:
        raise ValueError(f"not ready yet ({_call_count})")
    return "ok"

if __name__ == "__main__":
    counter = make_counter(10)
    print(counter())    # 11
    print(counter(5))   # 16
    counter.reset()
    print(counter())    # 11

    _call_count = 0
    result = flaky_fn(3)
    print("flaky result:", result, "after", _call_count, "calls")
    print("closure vars:", [c.cell_contents for c in flaky_fn.__wrapped__.__code__.co_freevars and flaky_fn.__closure__ or []])
