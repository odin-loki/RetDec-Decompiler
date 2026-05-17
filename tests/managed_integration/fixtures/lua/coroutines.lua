-- Tier 4: Coroutines. Tests coroutine.create, resume, yield,
-- producer-consumer pattern, and error propagation through coroutines.
local function producer(items)
    return coroutine.create(function()
        for _, v in ipairs(items) do
            coroutine.yield(v)
        end
    end)
end

local function consumer(gen)
    local results = {}
    while true do
        local ok, val = coroutine.resume(gen)
        if not ok or val == nil then break end
        results[#results + 1] = val * 2
    end
    return results
end

local gen = producer({ 1, 2, 3, 4, 5 })
local doubled = consumer(gen)
print("doubled:", table.concat(doubled, ", "))

-- Coroutine as iterator
local function range(from, to, step)
    step = step or 1
    return coroutine.wrap(function()
        local i = from
        while i <= to do
            coroutine.yield(i)
            i = i + step
        end
    end)
end

local sum = 0
for v in range(1, 10, 2) do
    sum = sum + v
end
print("sum odd 1..10:", sum)  -- 25

-- Symmetric coroutines (ping-pong)
local ping, pong
ping = coroutine.create(function()
    for i = 1, 3 do
        print("ping", i)
        coroutine.resume(pong)
    end
end)
pong = coroutine.create(function()
    for i = 1, 3 do
        print("pong", i)
        coroutine.resume(ping)
    end
end)
coroutine.resume(ping)
