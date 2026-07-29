//
//  m3_env.h
//
//  Created by Steven Massey on 4/19/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#ifndef m3_env_h
#define m3_env_h

#include "wasm3.h"
#include "m3_function.h"

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
}
M3Global;


//---------------------------------------------------------------------------------------------------------------------------------
typedef struct M3Module
{
    struct M3Runtime *      runtime;
    struct M3Environment *  environment;

    bytes_t                 wasmStart;
    bytes_t                 wasmEnd;

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

    M3MemoryInfo            memoryInfo;
    M3ImportInfo            memoryImport;
    bool                    memoryImported;
    const char*             memoryExportName;
    bool                    directValidated;

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

    M3SectionHandler        customSectionHandler;
}
M3Environment;

void                        Environment_Release         (IM3Environment i_environment);

// takes ownership of io_funcType and returns a pointer to the persistent version (could be same or different)
void                        Environment_AddFuncType     (IM3Environment i_environment, IM3FuncType * io_funcType);

//---------------------------------------------------------------------------------------------------------------------------------



struct M3DirectFrame;
struct M3DirectControl;

typedef struct M3Runtime
{

    IM3Environment          environment;


    IM3Module               modules;        // linked list of imported modules

    void *                  stack;
    void *                  originStack;
    u32                     stackSize;
    u32                     numStackSlots;
    IM3Function             lastCalled;     // last function that successfully executed

    u64                     fuel;
    bool                    fuelEnabled;
    bool                    suspended;
    IM3Function             suspendedFunction;
    struct M3DirectFrame *  directFrames;
    u32                     numDirectFrames;
    u32                     maxDirectFrames;
    struct M3DirectControl *directControls;
    u32                     numDirectControls;
    u32                     maxDirectControls;
    u32                     directValueTop;
    bool                    directActive;
    IM3Function             directEntry;

    void *                  userdata;

    M3Memory                memory;
    u32                     memoryLimit;

    M3ErrorInfo             error;
#if d_m3VerboseErrorMessages
    char                    error_message[256]; // the actual buffer. M3ErrorInfo can point to this
#endif

#if d_m3RecordBacktraces
    M3BacktraceInfo         backtrace;
#endif

}
M3Runtime;

void                        Runtime_Release             (IM3Runtime io_runtime);

M3Result                    ResizeMemory                (IM3Runtime io_runtime, u32 i_numPages);

typedef void *              (* ModuleVisitor)           (IM3Module i_module, void * i_info);
void *                      ForEachModule               (IM3Runtime i_runtime, ModuleVisitor i_visitor, void * i_info);

void *                      v_FindFunction              (IM3Module i_module, const char * const i_name);


d_m3EndExternC

#endif // m3_env_h
