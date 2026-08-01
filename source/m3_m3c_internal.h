// Internal compiler/runtime hooks for the optional .m3c subsystem.

#ifndef m3_m3c_internal_h
#define m3_m3c_internal_h

#include "m3_m3c.h"

#if d_m3HasM3C

#include "m3_exec_defs.h"

void        m3c_RecordOperation       (IM3Runtime i_runtime, pc_t i_location,
                                       IM3Operation i_operation);
void        m3c_RecordPointer         (IM3Runtime i_runtime, pc_t i_location);

M3Result    m3c_OperationToId         (IM3Operation i_operation, u32 * o_id);
IM3Operation m3c_OperationFromId      (u32 i_id);

bool        m3c_HasFunction           (IM3Function i_function);
M3Result    m3c_LoadFunction          (IM3Function i_function);
void        m3c_OnModuleLoaded        (IM3Module i_module);
void        m3c_ReleaseModule         (IM3Module i_module);

#endif // d_m3HasM3C
#endif // m3_m3c_internal_h
