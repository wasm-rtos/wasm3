//
//  Optional runtime linker for the WebAssembly dylink.0 convention.
//

#ifndef m3_dylink_h
#define m3_dylink_h

#include "m3_config.h"
#include "wasm3.h"

#if d_m3HasDylink

#ifdef __cplusplus
extern "C" {
#endif

// The resolver returns a parsed module for a DT_NEEDED-style dependency.
// It may use m3_ParseModule or m3_ParseM3C; the linker deliberately knows
// nothing about filesystems, SD cards, flash, or browser storage.
typedef M3Result (* M3DylinkResolveModule) (void * context,
                                            const char * name,
                                            IM3Module * o_module);

// Called after each module has joined the runtime but before relocations and
// constructors run. Embedders can bind WASI or other host imports here.
typedef M3Result (* M3DylinkLinkHostImports) (void * context,
                                              IM3Module module);

typedef struct M3DylinkOptions
{
    void *                      context;
    M3DylinkResolveModule       resolveModule;   // optional if no dependencies
    M3DylinkLinkHostImports     linkHostImports; // optional
    uint32_t                    linearStackSize; // 0 selects 64 KiB
}
M3DylinkOptions;

struct M3DylinkContext;
typedef struct M3DylinkContext * IM3DylinkContext;

// A program is a PIE root in a shared link group. Dependencies reached through
// dylink.0 DT_NEEDED entries are instantiated once and are visible to every
// program. Program-defined symbols remain private to that program, so several
// applications may export the same entry-point names.
typedef struct M3DylinkProgram
{
    const char *                name;
    IM3Module                  module;
    uint32_t                    nativeStackSize; // 0 uses the runtime stack size
    uint32_t                    linearStackSize; // 0 uses options/default
    void *                      userdata;
}
M3DylinkProgram;

extern const M3Result m3Err_dylinkMissingSection;
extern const M3Result m3Err_dylinkDependencyMissing;
extern const M3Result m3Err_dylinkDuplicateSymbol;
extern const M3Result m3Err_dylinkUnresolvedSymbol;
extern const M3Result m3Err_dylinkTypeMismatch;
extern const M3Result m3Err_dylinkUnsupported;

// Load a position-independent main module and its dylink.0 dependencies into
// one shared linear-memory/table link group. On success ownership of every
// module is transferred to the runtime, just like m3_LoadModule.
M3Result  m3_DylinkLoad  (IM3Runtime runtime,
                           IM3Module mainModule,
                           const char * mainName,
                           const M3DylinkOptions * options);

// Load several independent PIE programs with one resident dependency set,
// linear memory, and indirect-function table. Each returned context owns its
// execution stack, fuel/continuation state, and a non-overlapping linear stack.
// The runtime and all contexts must be externally serialized.
M3Result  m3_DylinkLoadGroup  (IM3Runtime runtime,
                                const M3DylinkProgram * programs,
                                uint32_t numPrograms,
                                const M3DylinkOptions * options,
                                IM3DylinkContext * outContexts);

// Select a program's execution state before using the regular m3_Call,
// m3_Resume, fuel, result, or userdata APIs on the group's runtime.
M3Result  m3_DylinkActivateContext  (IM3DylinkContext context);
IM3Runtime m3_DylinkGetContextRuntime (IM3DylinkContext context);
IM3Module  m3_DylinkGetContextModule  (IM3DylinkContext context);

#ifdef __cplusplus
}
#endif

#endif // d_m3HasDylink
#endif // m3_dylink_h
