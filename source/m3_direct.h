//
//  m3_direct.h
//
//  Direct WebAssembly bytecode executor.
//

#ifndef m3_direct_h
#define m3_direct_h

#include "m3_env.h"

d_m3BeginExternC

M3Result   DirectStart                 (IM3Function function);
M3Result   DirectStep                  (IM3Runtime runtime);
M3Result   DirectExecute               (IM3Runtime runtime, u64 fuel, u64 * consumed);
M3Result   DirectRun                   (IM3Runtime runtime);
void       DirectReset                 (IM3Runtime runtime);
void       DirectRelease               (IM3Runtime runtime);

M3Result   DirectEvaluateExpression    (IM3Module module, void * expressed, u8 type,
                                        bytes_t * bytes, cbytes_t end);
M3Result   DirectParseInitExpression   (IM3Module module, bytes_t * bytes, cbytes_t end);
M3Result   DirectValidateModule        (IM3Module module);
M3Result   DirectValidateFunctionGraph (IM3Function function);

M3Result   DirectGetSnapshotSize       (IM3Runtime runtime, u32 * outSize);
M3Result   DirectSaveSnapshot          (IM3Runtime runtime, u8 * buffer, u32 bufferSize, u32 * outSize);
M3Result   DirectLoadSnapshot          (IM3Runtime runtime, const u8 * buffer, u32 bufferSize);

d_m3EndExternC

#endif
