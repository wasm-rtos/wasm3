# Direct WebAssembly interpreter

This wasm3 fork executes the original module bytes. A function is validated
before its first reachable execution, but it is never translated into another
instruction format.

## Runtime state

Each runtime owns:

- a 64-bit value stack;
- explicit function-call frames;
- explicit structured-control frames;
- the current WebAssembly byte offset;
- linear memory, globals, fuel, and suspension state.

The value stack uses one eight-byte slot for every Core WebAssembly value.
`i32` and `f32` values occupy the low 32 bits of a slot. This representation is
independent of host endianness; conversion happens only at the C raw-import
boundary.

Function frames hold the active function, its current bytecode PC, local-value
base, operand-stack base, and control-frame base. Control frames describe
blocks, loops, and conditionals, including their entry and exit byte offsets.
No interpreter state depends on the native C call stack.

## Instruction cycle

`m3_Step()` performs one cycle:

1. Read one opcode from the active function's `.wasm` bytes.
2. Decode that instruction's immediates.
3. Execute its semantics against the explicit runtime state.
4. Return to the host.

An extended `0xFC` opcode and all of its immediates still form one Core
WebAssembly instruction. Bulk-memory operations execute atomically.

`m3_Execute(runtime, fuel, &consumed)` repeats this cycle up to `fuel` times.
Consequently one instruction always consumes one fuel. `m3_Run()` repeats it
without a fuel limit.

## Calls and imports

A WebAssembly call pushes an explicit function frame. A return moves the
declared results to the caller's operand-stack position and removes the frame.
Tail calls replace the current frame.

Raw host imports are invoked directly through `M3RawCall`. Arguments and result
slots are marshalled to the public raw-call ABI before entering C and converted
back afterward. This keeps the internal representation consistent on both
little-endian and big-endian machines.

## Memory

WebAssembly linear memory is byte-addressed and little-endian. Loads assemble
their value from bytes and stores split their value into bytes explicitly, so
the implementation does not depend on host alignment or endianness.

The module byte buffer must remain available for the module lifetime. It may
reside in memory-mapped flash or ROM. Storage that is not directly addressable,
such as a block-mode SD card, requires a cache or paging layer supplied by the
host.

## Suspension and snapshots

Because all execution state is explicit, the interpreter can return after any
instruction. Fuel exhaustion marks the runtime suspended without losing its
program counter, operand values, calls, or structured-control state.

Runtime snapshots store module-relative byte offsets rather than process code
addresses. They can therefore restore a suspended direct-execution state
without compiling or translating functions.
