# wasm3 runtime-control fork

A custom [`wasm3`](https://github.com/wasm3/wasm3) fork with **fuel control**, **runtime suspension**, **resume support**, **process-local runtime snapshots**, an optional **persistent `.m3c` metacode cache**, and optional **`dylink.0` WebAssembly libraries**.

This fork adds public APIs for controlling WebAssembly execution with per-runtime fuel, suspending execution when fuel is exhausted, resuming suspended runtimes, saving runtime snapshots to byte buffers, and restoring snapshots into fresh runtimes created from the same WASM module.

The original wasm3 project is a high-performance WebAssembly interpreter written in C. This fork keeps wasm3 as an interpreter, but extends it with runtime-control features needed for task scheduling, runtime swapping, and memory-pressure handling in higher-level systems.

This fork is intended to be used as the WebAssembly backend for `wasm-rtos`.

## What this fork adds

This fork adds runtime-level control features that are not part of upstream wasm3:

* Per-runtime fuel control.
* Runtime suspension when fuel is exhausted.
* Resume support for suspended runtimes.
* Process-local runtime snapshot save/load/restore support.
* Snapshot support for stack, globals, linear memory, fuel state, and continuation frames.
* Snapshot/resume support around host imports when the same imports are linked again before restoring.
* Optional, storage-agnostic `.m3c` images with relocatable wasm3 metacode.
* Optional `dylink.0` link groups with shared linear memory and a shared function table.

This fork does not add JIT or AOT compilation. It remains interpreter-only.

## Public API added by this fork

```c
extern const char* m3Err_fuelExhausted;
extern const char* m3Err_runtimeSuspended;
extern const char* m3Err_snapshotInvalid;
extern const char* m3Err_snapshotUnsupported;
extern const char* m3Err_snapshotBufferTooSmall;

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
```

The optional `.m3c` API is declared separately in `source/m3_m3c.h`.
The optional dynamic-linking API is declared separately in `source/m3_dylink.h`.

## Persistent `.m3c` metacode

Enable this subsystem with `d_m3HasM3C=1`, or with `BUILD_M3C=ON` when using CMake. It is compiled out by default; the disabled build contains neither its implementation nor its error strings.

`.m3c` stores the original module metadata together with already compiled, relocatable wasm3 metacode. `m3_ParseM3C()` reconstructs the module, and each defined function is read and relocated only when it is first needed. The temporary source-Wasm copy is released after `m3_LoadModule()` finishes module initialization.

The API uses caller-provided positional `readAt`, `writeAt`, and optional `sync` callbacks. wasm3 therefore has no dependency on SD, FAT, flash drivers, or a particular operating system; the same API can target a memory buffer, file, block device, or custom object store.

```c
#include "m3_m3c.h"

M3CStorage storage = {
    .context = my_storage,
    .readAt = storage_read_at,
    .writeAt = storage_write_at,
    .sync = storage_sync,
};

uint64_t image_size;
M3Result result = m3_WriteM3C(module, &storage, 0, &image_size);

IM3Module cached_module;
result = m3_ParseM3C(environment, &cached_module, &storage, 0);
```

The v1 image is target-, configuration-, and metacode-ABI-specific. It validates its header, source Wasm, function bounds, and per-function contents before execution. Host callback pointers are never persisted; imports must be linked normally after loading the cached module.

This first version materializes a called function's metacode in a normal wasm3 code page and keeps it for the runtime lifetime. It does not yet execute directly from storage or evict live code pages; bounded caching requires indirect call sites so eviction cannot leave stale code pointers.

## WebAssembly `dylink.0` libraries

Enable this subsystem with `d_m3HasDylink=1`, or with `BUILD_DYLINK=ON` when using CMake. Zig builds use `-Ddylink=true`. It is compiled out by default; the disabled build contains no linker state or linker error strings.

`m3_DylinkLoad()` loads a position-independent main module and its `NEEDED` libraries as one link group. The implementation follows the standard `dylink.0` conventions used by LLVM: every module receives an aligned `__memory_base` and `__table_base`, while all modules share one linear memory, function table, and `__stack_pointer`. It resolves direct function imports, `GOT.func`, and `GOT.mem`, then runs data relocations and constructors in dependency order.

The module resolver returns an already parsed `IM3Module`, so storage remains a host decision. A resolver may return a module from `m3_ParseModule()` or `m3_ParseM3C()`; `.wasm` and `.m3c` modules can therefore be mixed in one link group.

```c
#include "m3_dylink.h"

static M3Result resolve_library(void *context, const char *name,
                                IM3Module *out_module)
{
    // Read from SD, a file, flash, IndexedDB, or another host-owned store.
    return parse_named_module(context, name, out_module);
}

M3DylinkOptions options = {
    .context = library_store,
    .resolveModule = resolve_library,
    .linkHostImports = link_wasi_and_device_imports,
    .linearStackSize = 64 * 1024,
};

M3Result result = m3_DylinkLoad(runtime, app_module, "watch-app",
                                &options);
```

Pointers produced by C remain 32-bit offsets in the shared linear memory. This lets a library return `const char *`, accept arrays or structs, and receive Wasm function pointers without host-side pointer wrappers.

This first linker stage supports LLVM-style PIE mains and side modules. Non-PIE mains, TLS, runtime `dlopen`/`dlclose`, unloading, and symbol-version namespaces are intentionally rejected rather than partially emulated.

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
set fuel
run with m3_Call()
fuel is exhausted
runtime becomes suspended
save snapshot
destroy runtime

create fresh runtime
load the same WASM module
link the same host imports again
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
