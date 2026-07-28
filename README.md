# wasm3 direct-execution fork

This [`wasm3`](https://github.com/wasm3/wasm3) fork is the WebAssembly engine used by `wasm-rtos`.

The default backend executes the original WebAssembly bytecode directly. It does not translate functions to wasm3 metacode and does not allocate metacode pages. The program counter always points into the module's persistent `.wasm` bytes.

Floating-point instructions, raw host imports, simple WASI, fuel-based suspension, resume, and runtime snapshots remain supported.

## Execution API

```c
M3Result m3_Start(
    IM3Function function,
    uint32_t argc,
    const void *argptrs[]
);

M3Result m3_Step(IM3Runtime runtime);

M3Result m3_Execute(
    IM3Runtime runtime,
    uint64_t fuel,
    uint64_t *consumed
);

M3Result m3_Run(IM3Runtime runtime);
```

- `m3_Start()` prepares a call and does not execute its first instruction.
- `m3_Step()` executes exactly one Core WebAssembly instruction. It returns `m3Err_fuelExhausted` if the call is still active, or the call result when that instruction finishes or traps.
- `m3_Execute()` executes at most `fuel` instructions and optionally writes the exact number executed to `consumed`.
- `m3_Run()` removes the fuel limit and runs until completion or a trap. An infinite WebAssembly program intentionally makes this call run forever.

`m3_Call()`, `m3_CallV()`, and `m3_CallArgv()` are retained for source compatibility and use the direct executor. The existing `m3_SetFuel()`, `m3_AddFuel()`, `m3_DisableFuel()`, and `m3_Resume()` APIs are also retained.

## Fuel semantics

One decoded Core WebAssembly instruction costs one fuel.

- Immediates are part of their instruction and cost no additional fuel.
- A `0xFC`-prefixed instruction is one instruction.
- `call` costs one fuel, including a call to a raw host import.
- `memory.copy` and `memory.fill` each cost one fuel regardless of byte count.

Fuel is therefore a deterministic WebAssembly instruction budget, not a wall-clock deadline. A host import or bulk-memory instruction can perform variable work atomically. A platform that needs a hard real-time limit must also bound its host functions and use a clock or hardware watchdog.

Typical scheduler use:

```c
M3Result result;

if (!m3_IsSuspended(runtime))
{
    result = m3_Start(function, argc, args);
    if (result)
        return result;
}

result = m3_Execute(runtime, instructions_per_slice, NULL);
```

`m3_Execute(runtime, 0, ...)` executes no instructions and leaves an active call suspended.

## Direct interpreter state

The interpreter stores resumable state explicitly:

- a 64-bit value stack in the caller-sized wasm3 runtime stack;
- call frames containing bytecode PCs;
- structured-control frames for blocks, loops, conditionals, and branches;
- runtime-local fuel and suspension state.

Call and control frame arrays grow only as needed. The large legacy compiler state is not part of `M3Runtime` in the default configuration.

The byte buffer passed to `m3_ParseModule()` must remain valid for the module lifetime. On a target with memory-mapped flash or ROM, that buffer can reside there and be executed directly. A block device such as an SD card is not directly addressable, so using it as executable backing storage still requires a host-side cache or paging layer.

## Raw imports and WASI

`m3_LinkRawFunction()` and `m3_LinkRawFunctionEx()` bind imports without generating wrapper metacode. Imported functions are checked lazily when a reachable function is entered, matching wasm3's existing lazy-link behavior.

Simple WASI and floating-point execution are enabled through the same build options as before.

## Runtime snapshots

Snapshots can be saved when a runtime is suspended:

```c
M3Result m3_GetRuntimeSnapshotSize(
    IM3Runtime runtime,
    uint32_t *out_size
);

M3Result m3_SaveRuntimeSnapshot(
    IM3Runtime runtime,
    uint8_t *buffer,
    uint32_t buffer_size,
    uint32_t *out_size
);

M3Result m3_LoadRuntimeSnapshot(
    IM3Runtime runtime,
    const uint8_t *buffer,
    uint32_t buffer_size
);
```

Snapshot v2 stores bytecode offsets instead of metacode addresses. Restoring therefore does not compile any functions. It includes:

- value-stack contents;
- call and control frames;
- linear memory and globals;
- fuel and suspension state;
- module/function identities and bytecode offsets.

The format is process-local and intended for controlled task swapping. It is not a reboot-safe, cross-version, cross-platform, or long-term persistence format. Restore into a fresh runtime created from the same module bytes and layout, and relink the same host imports before loading the snapshot. Host-side resources and state are not serialized.

## Backend selection

Direct execution is the default:

```c
#define d_m3UseDirectExecutor 1
```

Setting `d_m3UseDirectExecutor=0` at compile time restores the legacy metacode backend for differential testing. The two backends are compiled exclusively, so the unused backend does not occupy the default binary or runtime structure.

## Verification

The direct executor has dedicated API tests for exact stepping and fuel counts:

```sh
sh test/direct/run.sh
```

It is also covered by the upstream Core spec suites, floating-point and multi-value workloads, WASI applications, raw-import tests, snapshot/restore tests, and the `wasm-rtos` scheduler suite.

## License

This fork is based on the original wasm3 project. Keep the original wasm3 license terms and attribution when redistributing it.
