;; Tier 3: Tables and indirect calls. Tests funcref table, call_indirect,
;; and dynamic dispatch through table indices.
(module
  (type $binary_op (func (param i32 i32) (result i32)))

  (func $add (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
  (func $sub (param i32 i32) (result i32) local.get 0 local.get 1 i32.sub)
  (func $mul (param i32 i32) (result i32) local.get 0 local.get 1 i32.mul)
  (func $max (param i32 i32) (result i32)
    local.get 0
    local.get 1
    local.get 0
    local.get 1
    i32.gt_s
    select
  )

  (table 4 funcref)
  (elem (i32.const 0) $add $sub $mul $max)

  (func $dispatch (export "dispatch") (param $op i32) (param $a i32) (param $b i32) (result i32)
    local.get $a
    local.get $b
    local.get $op
    call_indirect (type $binary_op)
  )

  (func $apply_all (export "apply_all") (param $a i32) (param $b i32) (result i32)
    (local $i i32)
    (local $sum i32)
    i32.const 0
    local.set $i
    i32.const 0
    local.set $sum
    loop $loop
      local.get $i
      i32.const 4
      i32.lt_s
      if
        local.get $sum
        local.get $a
        local.get $b
        local.get $i
        call_indirect (type $binary_op)
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
)
