(module
  (type $process_type (func (param i32 i32) (result i32)))
  (import "mylib" "process_buffer"
    (func $process_buffer (type $process_type)))

  (memory (export "memory") 1 2)
  (table 1 funcref)
  (elem (i32.const 0) $process_buffer)

  (export "reexport" (func $process_buffer))

  (func (export "run") (param $address i32) (param $length i32)
    (result i32)
    local.get $address
    local.get $length
    call $process_buffer)

  (func (export "run_indirect") (param $address i32) (param $length i32)
    (result i32)
    local.get $address
    local.get $length
    i32.const 0
    call_indirect (type $process_type)))
