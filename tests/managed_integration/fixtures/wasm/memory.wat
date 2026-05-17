;; Tier 2: Linear memory. Tests memory.grow, i32.store/load,
;; i64 operations, and memory-based data structures (array sum).
(module
  (memory (export "mem") 1)

  (func $store_array (export "store_array") (param $base i32) (param $len i32)
    (local $i i32)
    (local $ptr i32)
    i32.const 0
    local.set $i
    loop $loop
      local.get $i
      local.get $len
      i32.lt_s
      if
        local.get $base
        local.get $i
        i32.const 4
        i32.mul
        i32.add
        local.tee $ptr

        local.get $i     ;; value = index
        i32.store

        local.get $i
        i32.const 1
        i32.add
        local.set $i
        br $loop
      end
    end
  )

  (func $sum_array (export "sum_array") (param $base i32) (param $len i32) (result i32)
    (local $i i32)
    (local $sum i32)
    i32.const 0
    local.set $i
    i32.const 0
    local.set $sum
    loop $loop
      local.get $i
      local.get $len
      i32.lt_s
      if
        local.get $sum
        local.get $base
        local.get $i
        i32.const 4
        i32.mul
        i32.add
        i32.load
        i32.add
        local.set $sum

        local.get $i
        i32.const 1
        i32.add
        local.set $i
        br $loop
      end
    end
    local.get $sum
  )

  (func $grow_and_check (export "grow_and_check") (result i32)
    i32.const 1
    memory.grow  ;; returns old page count or -1
  )
)
