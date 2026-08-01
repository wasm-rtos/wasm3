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

#ifdef __cplusplus
}
#endif

#endif // d_m3HasDylink
#endif // m3_dylink_h
