//
//  m3_config.h
//
//  Created by Steven Massey on 5/4/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#ifndef m3_config_h
#define m3_config_h

#include "m3_config_platforms.h"

// general --------------------------------------------------------------------

# ifndef d_m3MaxFunctionStackHeight
#   define d_m3MaxFunctionStackHeight           2000    // max: 32768
# endif

# ifndef d_m3MaxLinearMemoryPages
#   define d_m3MaxLinearMemoryPages             65536
# endif

# ifndef d_m3MaxDuplicateFunctionImpl
#   define d_m3MaxDuplicateFunctionImpl         3
# endif

# ifndef d_m3VerboseErrorMessages
#   define d_m3VerboseErrorMessages             1
# endif

# ifndef d_m3FixedHeap
#   define d_m3FixedHeap                        false
//# define d_m3FixedHeap                        (32*1024)
# endif

# ifndef d_m3FixedHeapAlign
#   define d_m3FixedHeapAlign                   16
# endif

# ifndef d_m3RecordBacktraces
#   define d_m3RecordBacktraces                 0
# endif

# ifndef d_m3EnableExceptionBreakpoint
#   define d_m3EnableExceptionBreakpoint        0       // see m3_exception.h
# endif


# ifndef d_m3EnableWasiTracing
#  define d_m3EnableWasiTracing                 0
# endif

// logging --------------------------------------------------------------------

# ifndef d_m3LogParse
#   define d_m3LogParse                         0       // .wasm binary decoding info
# endif

# ifndef d_m3LogModule
#   define d_m3LogModule                        0       // wasm module info
# endif

# ifndef d_m3LogRuntime
#   define d_m3LogRuntime                       0       // higher-level runtime information
# endif

# ifndef d_m3LogNativeStack
#   define d_m3LogNativeStack                   0       // track the memory usage of the C-stack
# endif

# ifndef d_m3LogHeapOps
#   define d_m3LogHeapOps                       0       // track heap usage
# endif

# ifndef d_m3LogTimestamps
#   define d_m3LogTimestamps                    0       // track timestamps on heap logs
# endif

// other ----------------------------------------------------------------------

# ifndef d_m3HasFloat
#   define d_m3HasFloat                         1       // implement floating point ops
# endif

#if !d_m3HasFloat && !defined(d_m3NoFloatDynamic)
#   define d_m3NoFloatDynamic                   1       // if no floats, do not fail until flops are actually executed
#endif

#endif // m3_config_h
