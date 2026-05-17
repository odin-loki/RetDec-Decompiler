-- Tier 1: Basic Lua hello-world. Tests print, local variables,
-- string concatenation, and basic math operations.
local name = "World"
print("Hello, " .. name .. "!")
print("Lua fixture for RetDec.")

local function factorial(n)
    if n <= 1 then return 1 end
    return n * factorial(n - 1)
end

print("5! = " .. factorial(5))
print("10! = " .. factorial(10))
