;; Tier 1: Basic integer arithmetic. Tests i32 add/sub/mul, function
;; export, and calling convention.
(module
  (func $add (export "add") (param $a i32) (param $b i32) (result i32)
    local.get $a
    local.get $b
    i32.add
  )

  (func $sub (export "sub") (param $a i32) (param $b i32) (result i32)
    local.get $a
    local.get $b
    i32.sub
  )

  (func $mul (export "mul") (param $a i32) (param $b i32) (result i32)
    local.get $a
    local.get $b
    i32.mul
  )

  (func $abs_diff (export "abs_diff") (param $a i32) (param $b i32) (result i32)
    (local $diff i32)
    local.get $a
    local.get $b
    i32.sub
    local.tee $diff
    i32.const 0
    i32.lt_s
    if
      i32.const 0
      local.get $diff
      i32.sub
      local.set $diff
    end
    local.get $diff
  )
)
