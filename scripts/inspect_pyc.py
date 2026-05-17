import marshal, sys
with open(sys.argv[1], 'rb') as f:
    f.read(16)  # skip header
    code = marshal.load(f)
print("co_names:", list(code.co_names))
print("co_varnames:", list(code.co_varnames))
print("co_consts count:", len(code.co_consts))
for i, c in enumerate(code.co_consts):
    print(f"  co_consts[{i}]: {type(c).__name__} = {repr(c)[:60]}")
