# wasm3 runtime-control fork

A custom [`wasm3`](https://github.com/wasm3/wasm3) fork with **fuel control**, **runtime suspension**, **resume support**, **process-local runtime snapshot save/load/restore**, **shared linear memory between runtimes**, and **direct Wasm-to-Wasm function linking**.

This fork adds public APIs for controlling WebAssembly execution with per-runtime fuel, suspending execution when fuel is exhausted, resuming suspended runtimes, saving runtime snapshots to byte buffers, restoring snapshots into fresh runtimes created from the same WASM module, attaching an empty runtime to another runtime's linear memory, and binding a function import directly to an export from another module in the same runtime.

The original wasm3 project is a high-performance WebAssembly interpreter written in C. This fork keeps wasm3 as an interpreter, but extends it with runtime-control features needed for task scheduling, runtime swapping, and memory-pressure handling in higher-level systems.

This fork is intended to be used as the WebAssembly backend for `microwasm-os`.

## What this fork adds

This fork adds runtime-level control features that are not part of upstream wasm3:

* Per-runtime fuel control.
* Runtime suspension when fuel is exhausted.
* Resume support for suspended runtimes.
* Process-local runtime snapshot save/load/restore support.
* Snapshot support for stack, globals, linear memory, fuel state, and continuation frames.
* Snapshot/resume support around host imports when the same imports are linked again before restoring.
* Reference-counted linear-memory sharing between independently owned runtimes.
* Direct function linking between WebAssembly modules loaded in one runtime.

This fork does not add JIT or AOT compilation. It remains interpreter-only.

## Public API added by this fork

```c
extern const char* m3Err_fuelExhausted;
extern const char* m3Err_runtimeSuspended;
extern const char* m3Err_snapshotInvalid;
extern const char* m3Err_snapshotUnsupported;
extern const char* m3Err_snapshotBufferTooSmall;
extern const char* m3Err_sharedMemoryUnavailable;
extern const char* m3Err_sharedMemoryInUse;
extern const char* m3Err_sharedMemoryIncompatible;
extern const char* m3Err_functionAlreadyLinked;
extern const char* m3Err_functionTypeMismatch;
extern const char* m3Err_functionRuntimeMismatch;

void m3_SetFuel(IM3Runtime runtime, uint64_t fuel);
void m3_AddFuel(IM3Runtime runtime, uint64_t fuel);
void m3_DisableFuel(IM3Runtime runtime);

uint64_t m3_GetFuel(IM3Runtime runtime);
uint32_t m3_IsFuelEnabled(IM3Runtime runtime);

uint32_t m3_IsSuspended(IM3Runtime runtime);
M3Result m3_Resume(IM3Runtime runtime);

M3Result m3_GetRuntimeSnapshotSize(
    IM3Runtime runtime,
    uint32_t* out_size
);

M3Result m3_SaveRuntimeSnapshot(
    IM3Runtime runtime,
    uint8_t* buffer,
    uint32_t buffer_size,
    uint32_t* out_size
);

M3Result m3_LoadRuntimeSnapshot(
    IM3Runtime runtime,
    const uint8_t* buffer,
    uint32_t buffer_size
);

M3Result m3_ShareRuntimeMemory(
    IM3Runtime target_runtime,
    IM3Runtime source_runtime
);

M3Result m3_FindFunctionInModule(
    IM3Function* out_function,
    IM3Module module,
    const char* function_name
);

M3Result m3_LinkWasmFunction(
    IM3Module importing_module,
    const char* import_module_name,
    const char* import_function_name,
    IM3Function exported_function
);
```

## Shared runtime linear memory

`m3_ShareRuntimeMemory(target, source)` makes an empty target runtime retain the source runtime's initialized linear memory. A module loaded into the target afterward must import a compatible memory.

Only the linear memory is shared. Runtime stacks, globals, tables, code, fuel, suspension state, and user data remain independent. `memory.grow` updates the shared allocation, so every attached runtime observes the new size and obtains the current pointer through `m3_GetMemory()`.

The source runtime may be freed before the target. The shared allocation remains alive until the last attached runtime is freed.

The target runtime must not already contain a module, continuation, suspension state, or allocated linear memory. The imported memory's page size and limits must be compatible with the shared allocation. A runtime already attached to shared memory cannot load a module that defines its own memory.

Runtime execution and memory-management calls must be externally serialized. The reference count and shared memory contents are not synchronized for concurrent access.

Runtime snapshots are unsupported while the linear memory has more than one owner. A per-runtime snapshot cannot represent the live shared-memory relationship.

## Same-runtime Wasm module linking

`m3_LinkWasmFunction()` binds matching function imports in one loaded module directly to a defined WebAssembly export from another module loaded in the same runtime.

The call remains entirely inside wasm3. It does not use a native callback, a host-side handle, or a second runtime. The caller and library therefore use the same:

* Linear memory.
* Wasm stack.
* Fuel budget.
* Suspension and continuation state.
* Runtime snapshot.

The modules keep their own globals, tables, exports, and start functions. If a module has a start function, link all of its dependencies first and then call `m3_RunStart()` explicitly.

The importing module and exporting function must already be loaded into the same runtime. The import and export function types must match exactly. Link imports before compiling or calling the importing module.

Use `m3_FindFunctionInModule()` for unambiguous export lookup when multiple loaded libraries use the same export name:

```c
IM3Function process_buffer = NULL;

M3Result result = m3_FindFunctionInModule(
    &process_buffer,
    library_module,
    "process_buffer"
);

if (result == m3Err_none)
{
    result = m3_LinkWasmFunction(
        app_module,
        "mylib",
        "process_buffer",
        process_buffer
    );
}
```

A typical shared-memory layout loads the application module that defines memory first, followed by library modules compiled to import a compatible memory.

This API links explicit core WebAssembly function imports and exports. It is not a `dylink.0` loader: it does not perform relocations, allocate side-module data, merge tables, resolve imported globals, or automatically discover dependencies. General-purpose C/C++ shared objects still need a higher-level loader and dynamic-linking ABI.

Snapshot restore requires the same modules in the same order and the same Wasm and host imports linked before `m3_LoadRuntimeSnapshot()`.

## Fuel control

Fuel is runtime-local.

When fuel is enabled, wasm3 decreases the runtime fuel while executing WebAssembly code. When fuel reaches zero, execution stops and returns `m3Err_fuelExhausted`.

Fuel accounting runs in one shared metacode dispatcher instead of being
inlined into every operation handler. This keeps the compiled interpreter
smaller without changing fuel, suspension, or resume semantics.

If wasm3 can capture the current continuation, the runtime becomes suspended and can later be resumed with `m3_Resume()`.

Use `m3_SetFuel()` to set a new fuel value.

Use `m3_AddFuel()` to add more fuel before resuming.

Use `m3_DisableFuel()` to run without fuel accounting.

Use `m3_GetFuel()` to read the current remaining fuel.

Use `m3_IsFuelEnabled()` to check whether fuel accounting is enabled for the runtime.

## Suspension and resume

A runtime is suspended when execution stopped because fuel was exhausted and wasm3 captured a resumable continuation.

Use `m3_IsSuspended()` to check whether a runtime is currently suspended.

Use `m3_Resume()` to continue execution from the suspended point. Before resuming, add more fuel with `m3_AddFuel()` unless fuel has been disabled.

Calling `m3_Call()` on a suspended runtime returns `m3Err_runtimeSuspended`.

## Runtime snapshots

Snapshots can only be saved from suspended runtimes.

A snapshot stores enough runtime state to recreate the runtime later and continue execution from the same suspended point.

Snapshot v1 stores:

* Fuel state.
* Suspended function identity.
* Continuation frames.
* Program-counter offsets.
* Stack contents.
* Linear memory.
* Globals.
* Register state used by continuation frames.
* Floating-point continuation state when floating point support is enabled.

Use `m3_GetRuntimeSnapshotSize()` to query the required snapshot buffer size.

Use `m3_SaveRuntimeSnapshot()` to save a suspended runtime into a caller-provided byte buffer.

Use `m3_LoadRuntimeSnapshot()` to restore a snapshot into a fresh runtime created from the same WASM module.

## Snapshot v1 scope

Snapshot v1 is intentionally limited.

Snapshot v1 is:

* Process-local.
* Same-binary.
* Same-module.
* Same wasm3 build.
* Intended for runtime swapping inside one running process.
* Intended for task scheduling and RAM pressure handling in a higher-level runtime.

Snapshot v1 is not:

* A reboot-safe persistent format.
* A cross-version serialization format.
* A portable checkpoint format.
* A save file format.
* Guaranteed to work across different wasm3 builds, compiler settings, module layouts, platforms, or machines.

The snapshot format is designed for controlled runtime swapping, not long-term persistence.

## Expected snapshot flow

The expected flow is:

```text
create runtime
load the same WASM module
link required host imports
link required Wasm imports
set fuel
run with m3_Call()
fuel is exhausted
runtime becomes suspended
save snapshot
destroy runtime

create fresh runtime
load the same WASM module
link the same host imports again
link the same Wasm imports again
load snapshot
add fuel
resume with m3_Resume()
```

The restored runtime must use the same WASM module layout as the original runtime.

## Host imports

Host imports are supported around snapshot/resume, but host-side state is not part of the wasm3 snapshot.

If a module uses host imports, the same imports must be linked again before `m3_LoadRuntimeSnapshot()`.

Correct order:

```text
create fresh runtime
load same module
link host imports
load snapshot
resume
```

Incorrect order:

```text
create fresh runtime
load same module
load snapshot
link host imports later
resume
```

Host-side counters, file handles, device state, OS state, graphics state, audio buffers, and external resources must be managed by the host application or OS layer.

## Error values

`m3Err_fuelExhausted` is returned when runtime fuel reaches zero during execution.

`m3Err_runtimeSuspended` is returned when attempting to call into a runtime that is already suspended.

`m3Err_snapshotInvalid` is returned when snapshot bytes are invalid, corrupted, incompatible, or do not match the current runtime/module layout.

`m3Err_snapshotUnsupported` is returned when the requested snapshot operation is not supported for the current runtime state. For example, saving a snapshot from a non-suspended runtime is unsupported.

`m3Err_snapshotBufferTooSmall` is returned when the provided output buffer is smaller than the required snapshot size.

`m3Err_sharedMemoryUnavailable` is returned when the source runtime has no initialized linear memory that can be retained.

`m3Err_sharedMemoryInUse` is returned when the target runtime is not empty.

`m3Err_sharedMemoryIncompatible` is returned when a module defines its own memory on an attached runtime or imports memory with incompatible limits.

`m3Err_functionAlreadyLinked` is returned when a Wasm import is already bound to a different Wasm function or to a raw host function.

`m3Err_functionTypeMismatch` is returned when the import and export function types differ.

`m3Err_functionRuntimeMismatch` is returned when the importing module and exported function belong to different runtimes.

## Intended use in microwasm-os

This fork is intended to support task-like execution of WebAssembly programs.

A higher-level OS/runtime can:

* Create one wasm3 runtime per task.
* Run a task with a fixed fuel budget.
* Stop the task when fuel is exhausted.
* Resume the task later.
* Save a task snapshot when it should be swapped out.
* Destroy the runtime to release RAM.
* Recreate the runtime later.
* Load the snapshot.
* Continue execution from the same suspended point.

This allows `microwasm-os` to implement scheduling, sleeping, and runtime swapping above wasm3 without modifying WASM programs.

## Repository notes

This fork keeps the upstream wasm3 source layout where possible.

Generated WASM binaries must not be committed.

Temporary local test files such as `app.wasm` should remain ignored.

The temporary local test harness files used during development are not part of the public fork API and should not be kept in the repository root.

## Mini examples

### Run with fuel and resume

```c
m3_SetFuel(runtime, 1000);

M3Result result = m3_Call(function, argc, args);

while (result == m3Err_fuelExhausted)
{
    if (!m3_IsSuspended(runtime))
        break;

    m3_AddFuel(runtime, 1000);
    result = m3_Resume(runtime);
}
```

### Save a suspended runtime snapshot

```c
uint32_t snapshot_size = 0;

M3Result result = m3_GetRuntimeSnapshotSize(runtime, &snapshot_size);
if (result != m3Err_none)
{
    return result;
}

uint8_t* snapshot = malloc(snapshot_size);
if (!snapshot)
{
    return m3Err_mallocFailed;
}

result = m3_SaveRuntimeSnapshot(
    runtime,
    snapshot,
    snapshot_size,
    &snapshot_size
);
```

### Restore and resume a snapshot

```c
IM3Runtime fresh_runtime = create_runtime_from_same_wasm();

link_same_host_imports(fresh_runtime);

M3Result result = m3_LoadRuntimeSnapshot(
    fresh_runtime,
    snapshot,
    snapshot_size
);

if (result != m3Err_none)
{
    return result;
}

m3_AddFuel(fresh_runtime, 1000);
result = m3_Resume(fresh_runtime);
```

### Scheduler-style execution

```c
M3Result run_task_slice(IM3Runtime runtime, uint64_t fuel_per_slice)
{
    M3Result result;

    if (m3_IsSuspended(runtime))
    {
        m3_AddFuel(runtime, fuel_per_slice);
        result = m3_Resume(runtime);
    }
    else
    {
        m3_SetFuel(runtime, fuel_per_slice);
        result = m3_Call(function, argc, args);
    }

    return result;
}
```

## Development note

The custom changes in this fork were designed, reviewed, and iterated with the help of ChatGPT Plus and Codex.

ChatGPT Plus was used for architecture discussion, API design review, README planning, and pull request review. Codex was used to implement and iterate the code changes in this repository.

## License

This fork is based on the original wasm3 project.

Keep the original wasm3 license terms and attribution when redistributing this fork.
