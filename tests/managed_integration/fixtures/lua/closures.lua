-- Tier 3: Closures and upvalues. Tests closed-over locals,
-- memoisation, and higher-order functions.
local function memoize(fn)
    local cache = {}
    return function(n)
        if cache[n] == nil then
            cache[n] = fn(n)
        end
        return cache[n]
    end
end

local fib
fib = memoize(function(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end)

local function compose(f, g)
    return function(x) return f(g(x)) end
end

local addOne = function(x) return x + 1 end
local triple  = function(x) return x * 3 end
local tripleAddOne = compose(triple, addOne)  -- triple(addOne(x))

print("fib(10)=", fib(10))
print("fib(20)=", fib(20))
print("compose(5)=", tripleAddOne(5))  -- 18

-- Partial application
local function partial(fn, ...)
    local args = { ... }
    return function(...)
        local all = {}
        for _, v in ipairs(args) do all[#all + 1] = v end
        for _, v in ipairs({ ... }) do all[#all + 1] = v end
        return fn(table.unpack(all))
    end
end

local function add(a, b) return a + b end
local add5 = partial(add, 5)
print("add5(3)=", add5(3))
print("add5(10)=", add5(10))
