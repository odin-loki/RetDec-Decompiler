;; Tier 4: Exception handling (WebAssembly exceptions proposal).
;; Tests throw, try/catch, and rethrow.
(module
  (tag $arithmetic_error (export "arithmetic_error") (param i32))
  (tag $range_error (export "range_error") (param i32))

  (func $divide (export "divide") (param $a i32) (param $b i32) (result i32)
    local.get $b
    i32.eqz
    if
      i32.const 0
      throw $arithmetic_error
    end
    local.get $a
    local.get $b
    i32.div_s
  )

  (func $safe_index (export "safe_index") (param $idx i32) (param $len i32) (result i32)
    local.get $idx
    i32.const 0
    i32.lt_s
    if
      local.get $idx
      throw $range_error
    end
    local.get $idx
    local.get $len
    i32.ge_s
    if
      local.get $idx
      throw $range_error
    end
    local.get $idx
  )

  (func $safe_divide (export "safe_divide") (param $a i32) (param $b i32) (result i32)
    try (result i32)
      local.get $a
      local.get $b
      call $divide
    catch $arithmetic_error
      drop
      i32.const -1   ;; sentinel: division by zero
    end
  )
)
