(module
  (import "env" "memory" (memory 1 3))
  (import "host" "runtime_id" (func $runtime_id (result i32)))

  (data (i32.const 1024) "library")

  (func (export "write") (param i32 i32)
    local.get 0
    local.get 1
    i32.store)

  (func (export "read") (param i32) (result i32)
    local.get 0
    i32.load)

  (func (export "grow") (param i32) (result i32)
    local.get 0
    memory.grow)

  (func (export "size") (result i32)
    memory.size)

  (func (export "runtime_id") (result i32)
    call $runtime_id))
