//
//  m3_m3c.h
//
//  Optional persistent cache for wasm3 metacode. Enable with d_m3HasM3C=1.
//

#ifndef m3_m3c_h
#define m3_m3c_h

#include "m3_config.h"
#include "wasm3.h"

#if d_m3HasM3C

#ifdef __cplusplus
extern "C" {
#endif

// wasm3 deliberately knows nothing about filesystems, SD cards, or flash.
// The embedder supplies positional I/O for any storage it wants to use.
typedef M3Result (* M3CReadAt)  (void * context, uint64_t offset,
                                 void * data, uint32_t size);
typedef M3Result (* M3CWriteAt) (void * context, uint64_t offset,
                                 const void * data, uint32_t size);
typedef M3Result (* M3CSync)    (void * context);

typedef struct M3CStorage
{
    void *          context;
    M3CReadAt       readAt;
    M3CWriteAt      writeAt;
    M3CSync         sync;       // optional
}
M3CStorage;

extern const M3Result m3Err_m3cInvalid;
extern const M3Result m3Err_m3cIncompatible;
extern const M3Result m3Err_m3cStorage;
extern const M3Result m3Err_m3cUnsupportedRelocation;

// Compile all defined functions and write one versioned .m3c image. The image
// starts at i_offset, so the storage can be a file, partition, object, or a
// slice of a larger container. o_size receives the number of bytes written.
// The parsed module itself is not modified.
M3Result  m3_WriteM3C  (IM3Module i_module,
                        const M3CStorage * i_storage,
                        uint64_t i_offset,
                        uint64_t * o_size);

// Parse a standalone .m3c image. The image contains the original module
// metadata plus relocatable wasm3 metacode. Defined functions are materialized
// lazily from i_storage when first called. The callback table is copied, but
// its context and backing storage must remain valid for the module lifetime.
M3Result  m3_ParseM3C  (IM3Environment i_environment,
                        IM3Module * o_module,
                        const M3CStorage * i_storage,
                        uint64_t i_offset);

#ifdef __cplusplus
}
#endif

#endif // d_m3HasM3C
#endif // m3_m3c_h
