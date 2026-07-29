//
//  m3_core.h
//
//  Created by Steven Massey on 4/15/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#ifndef m3_core_h
#define m3_core_h

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "wasm3.h"
#include "m3_config.h"

# if defined(__cplusplus)
#   define d_m3BeginExternC     extern "C" {
#   define d_m3EndExternC       }
# else
#   define d_m3BeginExternC
#   define d_m3EndExternC
# endif

d_m3BeginExternC

#define d_m3ImplementFloat (d_m3HasFloat || d_m3NoFloatDynamic)

#if !defined(d_m3ShortTypesDefined)

typedef uint64_t        u64;
typedef int64_t         i64;
typedef uint32_t        u32;
typedef int32_t         i32;
typedef uint16_t        u16;
typedef int16_t         i16;
typedef uint8_t         u8;
typedef int8_t          i8;

#if d_m3ImplementFloat
typedef double          f64;
typedef float           f32;
#endif

#endif // d_m3ShortTypesDefined

#define PRIf32          "f"
#define PRIf64          "lf"

typedef const void *            m3ret_t;
typedef const void *            voidptr_t;
typedef const char *            cstr_t;
typedef const char * const      ccstr_t;
typedef const u8 *              bytes_t;
typedef const u8 * const        cbytes_t;

typedef u16                     m3opcode_t;

enum
{
    c_waOp_block                = 0x02,
    c_waOp_loop                 = 0x03,
    c_waOp_if                   = 0x04,
    c_waOp_else                 = 0x05,
    c_waOp_end                  = 0x0b,
    c_waOp_branch               = 0x0c,
    c_waOp_branchIf             = 0x0d,
    c_waOp_branchTable          = 0x0e,
    c_waOp_call                 = 0x10,
    c_waOp_getLocal             = 0x20,
    c_waOp_setLocal             = 0x21,
    c_waOp_teeLocal             = 0x22,
    c_waOp_getGlobal            = 0x23,
    c_waOp_i32_const            = 0x41,
    c_waOp_i64_const            = 0x42,
    c_waOp_f32_const            = 0x43,
    c_waOp_f64_const            = 0x44,
    c_waOp_extended             = 0xfc,
    c_waOp_memoryCopy           = 0xfc0a,
    c_waOp_memoryFill           = 0xfc0b
};

typedef u64                     m3slot_t;

typedef m3slot_t *              m3stack_t;

typedef
const void * const  cvptr_t;

# if defined (DEBUG)

#   define d_m3Log(CATEGORY, FMT, ...)                  printf (" %8s  |  " FMT, #CATEGORY, ##__VA_ARGS__);

#   if d_m3LogParse
#       define m3log_parse(CATEGORY, FMT, ...)          d_m3Log(CATEGORY, FMT, ##__VA_ARGS__)
#   else
#       define m3log_parse(...) {}
#   endif

#   if d_m3LogModule
#       define m3log_module(CATEGORY, FMT, ...)         d_m3Log(CATEGORY, FMT, ##__VA_ARGS__)
#   else
#       define m3log_module(...) {}
#   endif

#   if d_m3LogRuntime
#       define m3log_runtime(CATEGORY, FMT, ...)        d_m3Log(CATEGORY, FMT, ##__VA_ARGS__)
#   else
#       define m3log_runtime(...) {}
#   endif

#   define m3log(CATEGORY, FMT, ...)                    m3log_##CATEGORY (CATEGORY, FMT "\n", ##__VA_ARGS__)
# else
#   define d_m3Log(CATEGORY, FMT, ...)                  {}
#   define m3log(CATEGORY, FMT, ...)                    {}
# endif


# if defined(ASSERTS) || (defined(DEBUG) && !defined(NASSERTS))
#   define d_m3Assert(ASS)  if (!(ASS)) { printf("Assertion failed at %s:%d : %s\n", __FILE__, __LINE__, #ASS); abort(); }
# else
#   define d_m3Assert(ASS)
# endif

typedef struct M3MemoryHeader
{
    IM3Runtime      runtime;
    void *          maxStack;
    size_t          length;
}
M3MemoryHeader;

#define m3MemData(mem)                 (u8*)(((M3MemoryHeader*)(mem)) + 1)

#define d_m3DefaultMemPageSize              65536

#define d_m3MaxSaneTypesCount               1000000
#define d_m3MaxSaneFunctionsCount           1000000
#define d_m3MaxSaneImportsCount             100000
#define d_m3MaxSaneExportsCount             100000
#define d_m3MaxSaneGlobalsCount             1000000
#define d_m3MaxSaneElementSegments          10000000
#define d_m3MaxSaneDataSegments             100000
#define d_m3MaxSaneTableSize                10000000
#define d_m3MaxSaneUtf8Length               10000
#define d_m3MaxSaneFunctionArgRetCount      1000    // still insane, but whatever

#define d_externalKind_function             0
#define d_externalKind_table                1
#define d_externalKind_memory               2
#define d_externalKind_global               3

static const char * const c_waTypes []          = { "nil", "i32", "i64", "f32", "f64", "unknown" };
static const char * const c_waCompactTypes []   = { "_", "i", "I", "f", "F", "?" };


# if d_m3VerboseErrorMessages

M3Result m3Error (M3Result i_result, IM3Runtime i_runtime, IM3Module i_module, IM3Function i_function,
                  const char * const i_file, u32 i_lineNum, const char * const i_errorMessage, ...);

#  define _m3Error(RESULT, RT, MOD, FUN, FILE, LINE, FORMAT, ...) \
            m3Error (RESULT, RT, MOD, FUN, FILE, LINE, FORMAT, ##__VA_ARGS__)

# else
#  define _m3Error(RESULT, RT, MOD, FUN, FILE, LINE, FORMAT, ...) (RESULT)
# endif

#define ErrorRuntime(RESULT, RUNTIME, FORMAT, ...)      _m3Error (RESULT, RUNTIME, NULL, NULL,  __FILE__, __LINE__, FORMAT, ##__VA_ARGS__)
#define ErrorModule(RESULT, MOD, FORMAT, ...)           _m3Error (RESULT, MOD->runtime, MOD, NULL,  __FILE__, __LINE__, FORMAT, ##__VA_ARGS__)
#if d_m3LogNativeStack
void        m3StackCheckInit        ();
void        m3StackCheck            ();
int         m3StackGetMax           ();
#else
#define     m3StackCheckInit()
#define     m3StackCheck()
#define     m3StackGetMax()         0
#endif

#if d_m3LogTimestamps
#define     PRIts                   "%llu"
uint64_t    m3_GetTimestamp         ();
#else
#define     PRIts                   "%s"
#define     m3_GetTimestamp()       ""
#endif

void        m3_Abort                (const char* message);
void *      m3_Malloc_Impl          (size_t i_size);
void *      m3_Realloc_Impl         (void * i_ptr, size_t i_newSize, size_t i_oldSize);
void        m3_Free_Impl            (void * i_ptr);
void *      m3_CopyMem              (const void * i_from, size_t i_size);

#if d_m3LogHeapOps

// Tracing format: timestamp;heap:OpCode;name;size(bytes);new items;new ptr;old items;old ptr

static inline void * m3_AllocStruct_Impl(ccstr_t name, size_t i_size) {
    void * result = m3_Malloc_Impl(i_size);
    fprintf(stderr, PRIts ";heap:AllocStruct;%s;%zu;;%p;;\n", m3_GetTimestamp(), name, i_size, result);
    return result;
}

static inline void * m3_AllocArray_Impl(ccstr_t name, size_t i_num, size_t i_size) {
    void * result = m3_Malloc_Impl(i_size * i_num);
    fprintf(stderr, PRIts ";heap:AllocArr;%s;%zu;%zu;%p;;\n", m3_GetTimestamp(), name, i_size, i_num, result);
    return result;
}

static inline void * m3_ReallocArray_Impl(ccstr_t name, void * i_ptr_old, size_t i_num_new, size_t i_num_old, size_t i_size) {
    void * result = m3_Realloc_Impl (i_ptr_old, i_size * i_num_new, i_size * i_num_old);
    fprintf(stderr, PRIts ";heap:ReallocArr;%s;%zu;%zu;%p;%zu;%p\n", m3_GetTimestamp(), name, i_size, i_num_new, result, i_num_old, i_ptr_old);
    return result;
}

static inline void * m3_Malloc (ccstr_t name, size_t i_size) {
    void * result = m3_Malloc_Impl (i_size);
    fprintf(stderr, PRIts ";heap:AllocMem;%s;%zu;;%p;;\n", m3_GetTimestamp(), name, i_size, result);
    return result;
}
static inline void * m3_Realloc (ccstr_t name, void * i_ptr, size_t i_newSize, size_t i_oldSize) {
    void * result = m3_Realloc_Impl (i_ptr, i_newSize, i_oldSize);
    fprintf(stderr, PRIts ";heap:ReallocMem;%s;;%zu;%p;%zu;%p\n", m3_GetTimestamp(), name, i_newSize, result, i_oldSize, i_ptr);
    return result;
}

#define     m3_AllocStruct(STRUCT)                  (STRUCT *)m3_AllocStruct_Impl  (#STRUCT, sizeof (STRUCT))
#define     m3_AllocArray(STRUCT, NUM)              (STRUCT *)m3_AllocArray_Impl   (#STRUCT, NUM, sizeof (STRUCT))
#define     m3_ReallocArray(STRUCT, PTR, NEW, OLD)  (STRUCT *)m3_ReallocArray_Impl (#STRUCT, (void *)(PTR), (NEW), (OLD), sizeof (STRUCT))
#define     m3_Free(P)                              do { void* p = (void*)(P);                                  \
                                                        if (p) { fprintf(stderr, PRIts ";heap:FreeMem;;;;%p;\n", m3_GetTimestamp(), p); }     \
                                                        m3_Free_Impl (p); (P) = NULL; } while(0)
#else
#define     m3_Malloc(NAME, SIZE)                   m3_Malloc_Impl(SIZE)
#define     m3_Realloc(NAME, PTR, NEW, OLD)         m3_Realloc_Impl(PTR, NEW, OLD)
#define     m3_AllocStruct(STRUCT)                  (STRUCT *)m3_Malloc_Impl (sizeof (STRUCT))
#define     m3_AllocArray(STRUCT, NUM)              (STRUCT *)m3_Malloc_Impl (sizeof (STRUCT) * (NUM))
#define     m3_ReallocArray(STRUCT, PTR, NEW, OLD)  (STRUCT *)m3_Realloc_Impl ((void *)(PTR), sizeof (STRUCT) * (NEW), sizeof (STRUCT) * (OLD))
#define     m3_Free(P)                              do { m3_Free_Impl ((void*)(P)); (P) = NULL; } while(0)
#endif

M3Result    NormalizeType           (u8 * o_type, i8 i_convolutedWasmType);

bool        IsIntType               (u8 i_wasmType);
bool        IsFpType                (u8 i_wasmType);
bool        Is64BitType             (u8 i_m3Type);
u32         SizeOfType              (u8 i_m3Type);

M3Result    Read_u64                (u64 * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    Read_u32                (u32 * o_value, bytes_t * io_bytes, cbytes_t i_end);
#if d_m3ImplementFloat
M3Result    Read_f64                (f64 * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    Read_f32                (f32 * o_value, bytes_t * io_bytes, cbytes_t i_end);
#endif
M3Result    Read_u8                 (u8  * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLebUnsigned         (u64 * o_value, u32 i_maxNumBits, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLebSigned           (i64 * o_value, u32 i_maxNumBits, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLEB_u32             (u32 * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLEB_u7              (u8  * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLEB_i7              (i8  * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLEB_i32             (i32 * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    ReadLEB_i64             (i64 * o_value, bytes_t * io_bytes, cbytes_t i_end);
M3Result    Read_utf8               (cstr_t * o_utf8, bytes_t * io_bytes, cbytes_t i_end);

void        ReportError             (IM3Runtime io_runtime, IM3Module i_module, IM3Function i_function, ccstr_t i_errorMessage, ccstr_t i_file, u32 i_lineNum);

# if d_m3RecordBacktraces
void        PushBacktraceFrame         (IM3Runtime io_runtime, bytes_t i_pc);
void        FillBacktraceFunctionInfo  (IM3Runtime io_runtime, IM3Function i_function);
void        ClearBacktrace             (IM3Runtime io_runtime);
# endif

d_m3EndExternC

#endif // m3_core_h
