//
//  m3_env.h
//
//  Created by Steven Massey on 4/19/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#ifndef m3_env_h
#define m3_env_h

#include "wasm3.h"
#include "m3_code.h"
#include "m3_compile.h"

d_m3BeginExternC


//---------------------------------------------------------------------------------------------------------------------------------

typedef struct M3MemoryInfo
{
    u32     initPages;
    u32     maxPages;
    u32     pageSize;
}
M3MemoryInfo;


typedef struct M3Memory
{
    M3MemoryHeader *        mallocated;

    u32                     numPages;
    u32                     maxPages;
    u32                     pageSize;
}
M3Memory;

typedef M3Memory *          IM3Memory;


//---------------------------------------------------------------------------------------------------------------------------------

typedef struct M3DataSegment
{
    const u8 *              initExpr;           // wasm code
    const u8 *              data;

    u32                     initExprSize;
    u32                     memoryRegion;
    u32                     size;
}
M3DataSegment;

//---------------------------------------------------------------------------------------------------------------------------------

typedef struct M3Global
{
    M3ImportInfo            import;

    union
    {
        i32 i32Value;
        i64 i64Value;
#if d_m3HasFloat
        f64 f64Value;
        f32 f32Value;
#endif
    };

    cstr_t                  name;
    bytes_t                 initExpr;       // wasm code
    u32                     initExprSize;
    u8                      type;
    bool                    imported;
    bool                    isMutable;
# if d_m3HasDylink
    struct M3Global *       linkedGlobal;
# endif
}
M3Global;


//---------------------------------------------------------------------------------------------------------------------------------
typedef struct M3Module
{
    struct M3Runtime *      runtime;
    struct M3Environment *  environment;

    bytes_t                 wasmStart;
    bytes_t                 wasmEnd;
# if d_m3HasSnapshot
    u32                     wasmHash;
# endif

    cstr_t                  name;

    u32                     numFuncTypes;
    IM3FuncType *           funcTypes;              // array of pointers to list of FuncTypes

    u32                     numFuncImports;
    u32                     numFunctions;
    u32                     allFunctions;           // allocated functions count
    M3Function *            functions;

    i32                     startFunction;

    u32                     numDataSegments;
    M3DataSegment *         dataSegments;

    //u32                     importedGlobals;
    u32                     numGlobals;
    M3Global *              globals;

    u32                     numElementSegments;
    bytes_t                 elementSection;
    bytes_t                 elementSectionEnd;

    IM3Function *           table0;
    u32                     table0Size;
    const char*             table0ExportName;

# if d_m3HasDylink
    M3ImportInfo            table0Import;
    u32                     table0InitSize;
    u32                     table0MaxSize;
    bool                    table0Imported;
    void *                  dylink;
# endif

    M3MemoryInfo            memoryInfo;
    M3ImportInfo            memoryImport;
    bool                    memoryImported;
    const char*             memoryExportName;

# if d_m3HasM3C
    void *                  m3cState;
# endif

    //bool                    hasWasmCodeCopy;

    struct M3Module *       next;
}
M3Module;

M3Result                    Module_AddGlobal            (IM3Module io_module, IM3Global * o_global, u8 i_type, bool i_mutable, bool i_isImported);

M3Result                    Module_PreallocFunctions    (IM3Module io_module, u32 i_totalFunctions);
M3Result                    Module_AddFunction          (IM3Module io_module, u32 i_typeIndex, IM3ImportInfo i_importInfo /* can be null */);
IM3Function                 Module_GetFunction          (IM3Module i_module, u32 i_functionIndex);

void                        Module_GenerateNames        (IM3Module i_module);

void                        FreeImportInfo              (M3ImportInfo * i_info);

//---------------------------------------------------------------------------------------------------------------------------------

typedef struct M3Environment
{
//    struct M3Runtime *      runtimes;

    IM3FuncType             funcTypes;                          // linked list of unique M3FuncType structs that can be compared using pointer-equivalence

    IM3FuncType             retFuncTypes [c_m3Type_unknown];    // these 'point' to elements in the linked list above.
                                                                // the number of elements must match the basic types as per M3ValueType
    M3CodePage *            pagesReleased;

    M3SectionHandler        customSectionHandler;
}
M3Environment;

void                        Environment_Release         (IM3Environment i_environment);

// takes ownership of io_funcType and returns a pointer to the persistent version (could be same or different)
void                        Environment_AddFuncType     (IM3Environment i_environment, IM3FuncType * io_funcType);

//---------------------------------------------------------------------------------------------------------------------------------


// Kinds of safe point a suspended task can be at.
enum
{
    c_m3SafePointReturn = 1,    // just after a call returned
    c_m3SafePointLoop   = 2,    // at the start of a loop iteration
    c_m3SafePointEntry  = 3     // at a function's entry
};

// op_Loop and, with snapshots, the wasm offset of its instruction.
#define d_m3LoopOpLines         (1 + d_m3HasSnapshot)

// Asks the compiler where a safe point lands in the code it emits.
typedef struct M3SafePointQuery
{
    IM3Function             function;
    u32                     wasmOffset;
    u8                      kind;
    pc_t                    pc;
}
M3SafePointQuery;

typedef struct M3ContinuationFrame
{
    pc_t                    pc;
    m3stack_t               sp;
    M3MemoryHeader *        mem;
    m3reg_t                 r0;
    bool                    allowInternalControlFlow;
# if d_m3HasFloat
    f64                     fp0;
# endif
}
M3ContinuationFrame;

typedef struct M3Runtime
{
    M3Compilation           compilation;

    IM3Environment          environment;

    M3CodePage *            pagesOpen;      // linked list of code pages with writable space on them
    M3CodePage *            pagesFull;      // linked list of at-capacity pages

    u32                     numCodePages;
    u32                     numActiveCodePages;

# if d_m3HasM3C
    void *                  m3cCompileContext;
# endif

# if d_m3HasDylink
    void *                  dylinkState;
# endif

    IM3Module               modules;        // linked list of imported modules

    void *                  stack;
    void *                  originStack;
    u32                     stackSize;
    u32                     numStackSlots;
    IM3Function             lastCalled;     // last function that successfully executed

#if d_m3HasNativeStackBudget
    u32                     nativeStackBudget;  // bytes, 0 = unlimited
    void *                  nativeStackLimit;   // set by the outermost entry
#endif

    u64                     fuel;
    bool                    fuelEnabled;
    bool                    suspended;
    IM3Function             suspendedFunction;
    M3ContinuationFrame *   continuationFrames; // ordered outer-to-inner; resume executes from the end
    u32                     numContinuationFrames;
    u32                     maxContinuationFrames;
# if d_m3HasSnapshot
    M3SafePointQuery *      safePointQueries;   // filled in while compiling, during a snapshot load
    u32                     numSafePointQueries;
# endif

    void *                  userdata;

    M3Memory                memory;
    u32                     memoryLimit;

#if d_m3EnableStrace >= 2
    u32                     callDepth;
#endif

    M3ErrorInfo             error;
#if d_m3VerboseErrorMessages
    char                    error_message[256]; // the actual buffer. M3ErrorInfo can point to this
#endif

#if d_m3RecordBacktraces
    M3BacktraceInfo         backtrace;
#endif

	u32						newCodePageSequence;
}
M3Runtime;

void                        InitRuntime                 (IM3Runtime io_runtime, u32 i_stackSizeInBytes);
void                        Runtime_Release             (IM3Runtime io_runtime);

u32                         M3_DefaultMaxPages          (u32 i_pageSize);
M3Result                    ResizeMemory                (IM3Runtime io_runtime, u32 i_numPages);

typedef void *              (* ModuleVisitor)           (IM3Module i_module, void * i_info);
void *                      ForEachModule               (IM3Runtime i_runtime, ModuleVisitor i_visitor, void * i_info);

void *                      v_FindFunction              (IM3Module i_module, const char * const i_name);

IM3CodePage                 AcquireCodePage             (IM3Runtime io_runtime);
IM3CodePage                 AcquireCodePageWithCapacity (IM3Runtime io_runtime, u32 i_lineCount);
void                        ReleaseCodePage             (IM3Runtime io_runtime, IM3CodePage i_codePage);

d_m3EndExternC

#endif // m3_env_h
