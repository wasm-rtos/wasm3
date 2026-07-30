(module
  (import "env" "memory" (memory 1 2))

  (func (export "process_buffer")
    (param $address i32)
    (param $length i32)
    (result i32)
    (local $index i32)
    (local $sum i32)
    (local $value i32)

    block $done
      loop $next
        local.get $index
        local.get $length
        i32.ge_u
        br_if $done

        local.get $address
        local.get $index
        i32.add
        i32.load8_u
        i32.const 1
        i32.add
        local.set $value

        local.get $address
        local.get $index
        i32.add
        local.get $value
        i32.store8

        local.get $sum
        local.get $value
        i32.add
        local.set $sum

        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $next
      end
    end

    local.get $sum)

  (func (export "wrong_signature") (param i32) (result i32)
    local.get 0))
