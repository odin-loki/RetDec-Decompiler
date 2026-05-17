# Tier 3: Comprehensions and built-ins. Tests list/dict/set comprehensions,
# nested comprehensions, walrus operator (3.8+), and built-in functions.
data = list(range(1, 21))

# List comprehension with condition
evens_squared = [x**2 for x in data if x % 2 == 0]
print("evens_squared:", evens_squared)

# Nested comprehension (matrix flatten)
matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
flat = [val for row in matrix for val in row]
print("flat:", flat)

# Dict comprehension
word_lengths = {w: len(w) for w in ["hello", "world", "python"]}
print("word_lengths:", word_lengths)

# Set comprehension
unique_mods = {x % 7 for x in range(50)}
print("unique_mods:", sorted(unique_mods))

# Walrus operator (Python 3.8+)
values = [y := x**2, y + 1, y + 2]
print("walrus:", values)

# Built-ins: map, filter, zip, enumerate
names = ["Alice", "Bob", "Carol"]
scores = [85, 92, 78]
for i, (name, score) in enumerate(zip(names, scores)):
    print(f"  [{i}] {name}: {score}")

total = sum(map(lambda x: x**2, filter(lambda x: x > 80, scores)))
print("sum_sq_above80:", total)
