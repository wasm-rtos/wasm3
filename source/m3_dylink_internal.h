#ifndef m3_dylink_internal_h
#define m3_dylink_internal_h

#include "m3_config.h"

#if d_m3HasDylink

#include "m3_env.h"

M3Result     m3d_ParseSection       (IM3Module module, bytes_t bytes,
                                     cbytes_t end);
void         m3d_ReleaseModule      (IM3Module module);
void         m3d_ReleaseRuntime     (IM3Runtime runtime);

IM3Function  m3d_ResolveFunction    (IM3Function function);
M3Global *   m3d_ResolveGlobal      (M3Global * global);
void *       m3d_GlobalValuePointer (M3Global * global);

IM3Function * m3d_GetTable          (IM3Module module, u32 * o_size);

#endif // d_m3HasDylink
#endif // m3_dylink_internal_h
