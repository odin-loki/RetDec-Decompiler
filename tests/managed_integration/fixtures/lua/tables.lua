-- Tier 2: Tables as arrays, dicts, and OOP. Tests metatables,
-- __index, __newindex, __tostring, and table methods.
local function newStack()
    local s = { _data = {}, _size = 0 }
    function s:push(v)
        self._size = self._size + 1
        self._data[self._size] = v
    end
    function s:pop()
        if self._size == 0 then error("stack underflow") end
        local v = self._data[self._size]
        self._data[self._size] = nil
        self._size = self._size - 1
        return v
    end
    function s:peek() return self._data[self._size] end
    function s:size() return self._size end
    return s
end

-- Class with metatable
local Animal = {}
Animal.__index = Animal

function Animal.new(name, sound)
    return setmetatable({ name = name, sound = sound }, Animal)
end

function Animal:speak()
    return self.name .. " says " .. self.sound
end

function Animal:__tostring()
    return "Animal(" .. self.name .. ")"
end

-- Table operations
local t = { 3, 1, 4, 1, 5, 9, 2, 6 }
table.sort(t)
print("sorted:", table.concat(t, ", "))

local stack = newStack()
stack:push(10); stack:push(20); stack:push(30)
print("peek:", stack:peek())
print("pop:", stack:pop(), "size:", stack:size())

local dog = Animal.new("Rex", "Woof")
print(dog:speak())
print(tostring(dog))
