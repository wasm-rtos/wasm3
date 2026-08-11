//
//  WebAssembly dylink.0 support. This translation unit is empty when the
//  feature is disabled so embedded builds pay no code or data cost.
//

#include "m3_config.h"

#if d_m3HasDylink

#include <limits.h>

#include "m3_dylink.h"
#include "m3_dylink_internal.h"
#include "m3_exception.h"

const M3Result m3Err_dylinkMissingSection = "module has no dylink.0 section";
const M3Result m3Err_dylinkDependencyMissing = "dylink.0 dependency is missing";
const M3Result m3Err_dylinkDuplicateSymbol = "duplicate dylink.0 symbol";
const M3Result m3Err_dylinkUnresolvedSymbol = "unresolved dylink.0 symbol";
const M3Result m3Err_dylinkTypeMismatch = "dylink.0 symbol type mismatch";
const M3Result m3Err_dylinkUnsupported = "unsupported dylink.0 feature";

enum
{
    c_m3DylinkMemInfo       = 1,
    c_m3DylinkNeeded        = 2,
    c_m3DylinkExportInfo    = 3,
    c_m3DylinkImportInfo    = 4,
    c_m3DylinkRuntimePath   = 5
};

typedef struct M3DylinkInfo
{
    cstr_t module;
    cstr_t name;
    u32 flags;
}
M3DylinkInfo;

typedef struct M3DylinkMetadata
{
    u32 memorySize;
    u32 memoryAlignment;
    u32 tableSize;
    u32 tableAlignment;

    cstr_t * needed;
    u32 numNeeded;

    M3DylinkInfo * exports;
    u32 numExports;
    M3DylinkInfo * imports;
    u32 numImports;

    bool hasMemoryInfo;
}
M3DylinkMetadata;

typedef struct M3DylinkObject
{
    cstr_t name;
    IM3Module module;
    u32 memoryBase;
    u32 memorySize;
    u32 tableBase;
    u32 tableSize;
    u32 exportTableBase;
    u32 exportTableSize;
    u32 programIndex;
    u8 visit;
}
M3DylinkObject;

typedef struct M3DylinkState M3DylinkState;

typedef struct M3DylinkRange
{
    u32 base;
    u32 size;
    struct M3DylinkRange * next;
}
M3DylinkRange;

struct M3DylinkContext
{
    M3DylinkState * state;
    IM3Module module;

    void * stack;
    u32 stackSize;
    u32 numStackSlots;
    bool ownsStack;

    u64 fuel;
    bool fuelEnabled;
    bool suspended;
    IM3Function suspendedFunction;
    IM3Function lastCalled;
    M3ContinuationFrame * continuationFrames;
    u32 numContinuationFrames;
    u32 maxContinuationFrames;
    void * userdata;

    u32 linearStackLow;
    u32 linearStackHigh;
    u32 linearStackPointer;
    u32 linearHeapLow;
    u32 linearHeapHigh;
    u32 programIndex;
    bool ownsLinearHeap;

#if d_m3EnableStrace >= 2
    u32 callDepth;
#endif
#if d_m3RecordBacktraces
    M3BacktraceInfo backtrace;
#endif
};

struct M3DylinkState
{
    IM3Runtime runtime;
    M3DylinkObject * objects;
    u32 numObjects;
    u32 objectCapacity;

    IM3Function * table;
    u32 tableSize;

    M3Global stackPointerStorage;
    M3Global stackLowStorage;
    M3Global stackHighStorage;
    M3Global * stackPointer;
    M3Global * stackLowGlobal;
    M3Global * stackHighGlobal;
    bool stackLowConfigured;
    bool stackHighConfigured;
    u32 stackLow;
    u32 stackHigh;
    u32 heapBase;
    u32 heapEnd;
    u32 memoryCursor;
    u32 tableCursor;
    u32 maximumTable;
    M3DylinkRange * freeMemoryRanges;
    M3DylinkRange * freeTableRanges;

    IM3DylinkContext * contexts;
    u32 numContexts;
    u32 contextCapacity;
    u32 numActiveContexts;
    IM3DylinkContext activeContext;
    u32 originStackSize;
    u32 originNumStackSlots;
    void * originUserdata;
    bool dynamicPrograms;
};


static void  FreeInfoArray  (M3DylinkInfo * info, u32 count)
{
    for (u32 i = 0; i < count; ++i)
    {
        m3_Free (info [i].module);
        m3_Free (info [i].name);
    }
    m3_Free (info);
}


static void  FreeMetadata  (M3DylinkMetadata * metadata)
{
    if (not metadata)
        return;
    for (u32 i = 0; i < metadata->numNeeded; ++i)
        m3_Free (metadata->needed [i]);
    m3_Free (metadata->needed);
    FreeInfoArray (metadata->exports, metadata->numExports);
    FreeInfoArray (metadata->imports, metadata->numImports);
    m3_Free (metadata);
}


static M3Result  ParseStringVector  (bytes_t * io_bytes, cbytes_t end,
                                     cstr_t ** o_strings, u32 * o_count)
{
    M3Result result = m3Err_none;
    cstr_t * strings = NULL;
    u32 count = 0;

_   (ReadLEB_u32 (& count, io_bytes, end));
    _throwif ("too many dylink.0 strings", count > d_m3MaxSaneImportsCount);
    if (count)
    {
        strings = m3_AllocArray (cstr_t, count);
        _throwifnull (strings);
        for (u32 i = 0; i < count; ++i)
_           (Read_utf8 (& strings [i], io_bytes, end));
    }

    *o_strings = strings;
    *o_count = count;
    return m3Err_none;

_catch:
    if (strings)
    {
        for (u32 i = 0; i < count; ++i)
            m3_Free (strings [i]);
    }
    m3_Free (strings);
    return result;
}


static M3Result  ParseInfoVector  (bytes_t * io_bytes, cbytes_t end,
                                   bool hasModule, M3DylinkInfo ** o_info,
                                   u32 * o_count)
{
    M3Result result = m3Err_none;
    M3DylinkInfo * info = NULL;
    u32 count = 0;

_   (ReadLEB_u32 (& count, io_bytes, end));
    _throwif ("too many dylink.0 symbols", count > d_m3MaxSaneImportsCount);
    if (count)
    {
        info = m3_AllocArray (M3DylinkInfo, count);
        _throwifnull (info);
        for (u32 i = 0; i < count; ++i)
        {
            if (hasModule)
_               (Read_utf8 (& info [i].module, io_bytes, end));
_           (Read_utf8 (& info [i].name, io_bytes, end));
_           (ReadLEB_u32 (& info [i].flags, io_bytes, end));
        }
    }

    *o_info = info;
    *o_count = count;
    return m3Err_none;

_catch:
    FreeInfoArray (info, count);
    return result;
}


M3Result  m3d_ParseSection  (IM3Module module, bytes_t bytes, cbytes_t end)
{
    if (module->dylink)
        return m3Err_wasmMalformed;

    M3DylinkMetadata * metadata = m3_AllocStruct (M3DylinkMetadata);
    if (not metadata)
        return m3Err_mallocFailed;

    M3Result result = m3Err_none;
    u32 seen = 0;
    while (bytes < end)
    {
        u8 type;
        u32 size;
_       (Read_u8 (& type, & bytes, end));
_       (ReadLEB_u32 (& size, & bytes, end));
        _throwif (m3Err_wasmSectionOverrun, (size_t) (end - bytes) < size);
        bytes_t subsection = bytes;
        cbytes_t subsectionEnd = bytes + size;
        bytes = (bytes_t) subsectionEnd;

        if (type < 32)
        {
            u32 bit = 1u << type;
            _throwif (m3Err_wasmMalformed, seen & bit);
            seen |= bit;
        }

        switch (type)
        {
        case c_m3DylinkMemInfo:
_           (ReadLEB_u32 (& metadata->memorySize, & subsection, subsectionEnd));
_           (ReadLEB_u32 (& metadata->memoryAlignment, & subsection, subsectionEnd));
_           (ReadLEB_u32 (& metadata->tableSize, & subsection, subsectionEnd));
_           (ReadLEB_u32 (& metadata->tableAlignment, & subsection, subsectionEnd));
            metadata->hasMemoryInfo = true;
            break;
        case c_m3DylinkNeeded:
_           (ParseStringVector (& subsection, subsectionEnd,
                                 & metadata->needed, & metadata->numNeeded));
            break;
        case c_m3DylinkExportInfo:
_           (ParseInfoVector (& subsection, subsectionEnd, false,
                               & metadata->exports, & metadata->numExports));
            break;
        case c_m3DylinkImportInfo:
_           (ParseInfoVector (& subsection, subsectionEnd, true,
                               & metadata->imports, & metadata->numImports));
            break;
        case c_m3DylinkRuntimePath:
        {
            cstr_t * paths = NULL;
            u32 count = 0;
_           (ParseStringVector (& subsection, subsectionEnd, & paths, & count));
            for (u32 i = 0; i < count; ++i)
                m3_Free (paths [i]);
            m3_Free (paths);
            break;
        }
        default:
            subsection = (bytes_t) subsectionEnd;
            break;
        }
        _throwif (m3Err_wasmSectionUnderrun, subsection != subsectionEnd);
    }

    _throwif (m3Err_wasmMalformed, not metadata->hasMemoryInfo);
    module->dylink = metadata;
    return m3Err_none;

_catch:
    FreeMetadata (metadata);
    return result;
}


void  m3d_ReleaseModule  (IM3Module module)
{
    if (module)
    {
        FreeMetadata ((M3DylinkMetadata *) module->dylink);
        module->dylink = NULL;
    }
}


static void  SaveActiveContext  (M3DylinkState * state)
{
    IM3DylinkContext context = state ? state->activeContext : NULL;
    IM3Runtime runtime = state ? state->runtime : NULL;
    if (not context or not runtime)
        return;

    context->fuel = runtime->fuel;
    context->fuelEnabled = runtime->fuelEnabled;
    context->suspended = runtime->suspended;
    context->suspendedFunction = runtime->suspendedFunction;
    context->lastCalled = runtime->lastCalled;
    context->continuationFrames = runtime->continuationFrames;
    context->numContinuationFrames = runtime->numContinuationFrames;
    context->maxContinuationFrames = runtime->maxContinuationFrames;
    context->userdata = runtime->userdata;
    if (state->stackPointer)
        context->linearStackPointer = (u32) state->stackPointer->i32Value;
#if d_m3EnableStrace >= 2
    context->callDepth = runtime->callDepth;
#endif
#if d_m3RecordBacktraces
    context->backtrace = runtime->backtrace;
#endif
}


static void  ParkRuntime  (M3DylinkState * state)
{
    if (not state or not state->runtime)
        return;
    SaveActiveContext (state);
    IM3Runtime runtime = state->runtime;
    runtime->stack = runtime->originStack;
    runtime->stackSize = state->originStackSize;
    runtime->numStackSlots = state->originNumStackSlots;
    runtime->fuel = 0;
    runtime->fuelEnabled = false;
    runtime->suspended = false;
    runtime->suspendedFunction = NULL;
    runtime->lastCalled = NULL;
    runtime->continuationFrames = NULL;
    runtime->numContinuationFrames = 0;
    runtime->maxContinuationFrames = 0;
    runtime->userdata = state->originUserdata;
#if d_m3EnableStrace >= 2
    runtime->callDepth = 0;
#endif
#if d_m3RecordBacktraces
    M3_INIT (runtime->backtrace);
#endif
    if (runtime->memory.mallocated)
    {
        runtime->memory.mallocated->runtime = runtime;
        runtime->memory.mallocated->maxStack =
            (m3slot_t *) runtime->stack + runtime->numStackSlots;
    }
    state->activeContext = NULL;
    m3_ResetErrorInfo (runtime);
}


static M3Result  ActivateContext  (IM3DylinkContext context)
{
    if (not context or not context->state or not context->state->runtime)
        return m3Err_dylinkUnsupported;

    M3DylinkState * state = context->state;
    IM3Runtime runtime = state->runtime;
    if (runtime->dylinkState != state)
        return m3Err_dylinkUnsupported;
    if (state->activeContext == context)
        return m3Err_none;

    SaveActiveContext (state);

    runtime->stack = context->stack;
    runtime->stackSize = context->stackSize;
    runtime->numStackSlots = context->numStackSlots;
    runtime->fuel = context->fuel;
    runtime->fuelEnabled = context->fuelEnabled;
    runtime->suspended = context->suspended;
    runtime->suspendedFunction = context->suspendedFunction;
    runtime->lastCalled = context->lastCalled;
    runtime->continuationFrames = context->continuationFrames;
    runtime->numContinuationFrames = context->numContinuationFrames;
    runtime->maxContinuationFrames = context->maxContinuationFrames;
    runtime->userdata = context->userdata;
#if d_m3EnableStrace >= 2
    runtime->callDepth = context->callDepth;
#endif
#if d_m3RecordBacktraces
    runtime->backtrace = context->backtrace;
#endif

    // A memory.grow in another context may have moved the allocation. Saved
    // continuation frames identify memory 0, so refresh their native pointer.
    for (u32 i = 0; i < runtime->numContinuationFrames; ++i)
        if (runtime->continuationFrames [i].mem)
            runtime->continuationFrames [i].mem = runtime->memory.mallocated;

    if (runtime->memory.mallocated)
    {
        runtime->memory.mallocated->runtime = runtime;
        runtime->memory.mallocated->maxStack =
            (m3slot_t *) runtime->stack + runtime->numStackSlots;
    }
    if (state->stackPointer)
        state->stackPointer->i32Value = (i32) context->linearStackPointer;
    if (state->stackLowGlobal)
        state->stackLowGlobal->i32Value = (i32) context->linearStackLow;
    if (state->stackHighGlobal)
        state->stackHighGlobal->i32Value = (i32) context->linearStackHigh;

    state->activeContext = context;
    m3_ResetErrorInfo (runtime);
    return m3Err_none;
}


M3Result  m3_DylinkActivateContext  (IM3DylinkContext context)
{
    return ActivateContext (context);
}


IM3Runtime  m3_DylinkGetContextRuntime  (IM3DylinkContext context)
{
    return context and context->state ? context->state->runtime : NULL;
}


IM3Module  m3_DylinkGetContextModule  (IM3DylinkContext context)
{
    return context ? context->module : NULL;
}


#if d_m3RecordBacktraces
static void  FreeContextBacktrace  (IM3DylinkContext context)
{
    IM3BacktraceFrame frame = context->backtrace.frames;
    while (frame)
    {
        IM3BacktraceFrame next = frame->next;
        m3_Free (frame);
        frame = next;
    }
    context->backtrace.frames = NULL;
    context->backtrace.lastFrame = NULL;
}
#endif


static void  FreeContextStorage  (IM3DylinkContext context)
{
    if (not context)
        return;
#if d_m3RecordBacktraces
    FreeContextBacktrace (context);
#endif
    m3_Free (context->continuationFrames);
    if (context->ownsStack)
        m3_Free (context->stack);
    m3_Free (context);
}


static void  FreeRangeList  (M3DylinkRange * range)
{
    while (range)
    {
        M3DylinkRange * next = range->next;
        m3_Free (range);
        range = next;
    }
}


void  m3d_ReleaseRuntime  (IM3Runtime runtime)
{
    if (not runtime or not runtime->dylinkState)
        return;
    M3DylinkState * state = (M3DylinkState *) runtime->dylinkState;
    ParkRuntime (state);
    for (u32 i = 0; i < state->numContexts; ++i)
        FreeContextStorage (state->contexts [i]);
    for (u32 i = 0; i < state->numObjects; ++i)
    {
        if (state->objects [i].programIndex == UINT32_MAX
            and state->objects [i].module
            and state->objects [i].module->runtime != runtime)
            m3_FreeModule (state->objects [i].module);
        m3_Free (state->objects [i].name);
    }
    m3_Free (state->objects);
    m3_Free (state->table);
    m3_Free (state->contexts);
    FreeRangeList (state->freeMemoryRanges);
    FreeRangeList (state->freeTableRanges);
    m3_Free (state);
    runtime->dylinkState = NULL;
}


IM3Function  m3d_ResolveFunction  (IM3Function function)
{
    return Function_Resolve (function);
}


M3Global *  m3d_ResolveGlobal  (M3Global * global)
{
    for (u32 depth = 0; global and global->linkedGlobal
                      and global->linkedGlobal != global
                      and depth < 64; ++depth)
        global = global->linkedGlobal;
    return global;
}


void *  m3d_GlobalValuePointer  (M3Global * global)
{
    global = m3d_ResolveGlobal (global);
    return global ? (void *) & global->i64Value : NULL;
}


IM3Function *  m3d_GetTable  (IM3Module module, u32 * o_size)
{
    if (module and module->runtime and module->runtime->dylinkState)
    {
        M3DylinkState * state = (M3DylinkState *) module->runtime->dylinkState;
        if (o_size)
            *o_size = state->tableSize;
        return state->table;
    }
    if (o_size)
        *o_size = module ? module->table0Size : 0;
    return module ? module->table0 : NULL;
}


enum
{
    c_m3DylinkBindingWeak = 1,
    c_m3DylinkBindingLocal = 2,
    c_m3DylinkVisibilityHidden = 4,
    c_m3DylinkTLS = 0x100,
    c_m3DylinkDefaultStackSize = 64 * 1024
};


static cstr_t  DuplicateString  (const char * string)
{
    if (not string)
        return NULL;
    size_t length = strlen (string) + 1;
    char * copy = m3_AllocArray (char, length);
    if (copy)
        memcpy (copy, string, length);
    return copy;
}


static i32  FindDependencyIndex  (M3DylinkState * state, const char * name)
{
    for (u32 i = 0; i < state->numObjects; ++i)
        if (state->objects [i].programIndex == UINT32_MAX
            and strcmp (state->objects [i].name, name) == 0)
            return (i32) i;
    return -1;
}


static M3Result  AddObject  (M3DylinkState * state, const char * name,
                             IM3Module module, u32 programIndex,
                             u32 * o_index)
{
    if (not name or not module)
        return m3Err_dylinkDependencyMissing;
    if (not module->dylink)
        return m3Err_dylinkMissingSection;
    if (module->runtime)
        return m3Err_moduleAlreadyLinked;

    i32 existing = programIndex == UINT32_MAX
                 ? FindDependencyIndex (state, name) : -1;
    if (existing >= 0)
    {
        if (state->objects [existing].module != module)
            return m3Err_dylinkDuplicateSymbol;
        *o_index = (u32) existing;
        return m3Err_none;
    }
    for (u32 i = 0; i < state->numObjects; ++i)
    {
        if (state->objects [i].module == module)
            return m3Err_dylinkDuplicateSymbol;
        if (programIndex != UINT32_MAX
            and state->objects [i].programIndex != UINT32_MAX
            and strcmp (state->objects [i].name, name) == 0)
            return m3Err_dylinkDuplicateSymbol;
    }

    if (state->numObjects == state->objectCapacity)
    {
        u32 oldCapacity = state->objectCapacity;
        u32 newCapacity = oldCapacity ? oldCapacity * 2u : 4u;
        if (newCapacity < oldCapacity)
            return m3Err_mallocFailed;
        M3DylinkObject * objects = m3_ReallocArray (M3DylinkObject,
                                                    state->objects,
                                                    newCapacity,
                                                    oldCapacity);
        if (not objects)
            return m3Err_mallocFailed;
        state->objects = objects;
        state->objectCapacity = newCapacity;
    }

    cstr_t copy = DuplicateString (name);
    if (not copy)
        return m3Err_mallocFailed;
    u32 index = state->numObjects++;
    M3_INIT (state->objects [index]);
    state->objects [index].name = copy;
    state->objects [index].module = module;
    state->objects [index].programIndex = programIndex;
    *o_index = index;
    return m3Err_none;
}


static M3Result  CollectDependencies  (M3DylinkState * state, u32 index,
                                       const M3DylinkOptions * options)
{
    M3DylinkObject * object = & state->objects [index];
    if (object->visit == 2)
        return m3Err_none;
    if (object->visit == 1)
        return m3Err_none; // dependency cycle; symbols are resolved later
    object->visit = 1;

    M3DylinkMetadata * metadata = (M3DylinkMetadata *) object->module->dylink;
    for (u32 i = 0; i < metadata->numNeeded; ++i)
    {
        const char * name = metadata->needed [i];
        i32 found = FindDependencyIndex (state, name);
        u32 dependency;
        if (found < 0)
        {
            if (not options or not options->resolveModule)
                return m3Err_dylinkDependencyMissing;
            IM3Module module = NULL;
            M3Result result = options->resolveModule (options->context, name,
                                                       & module);
            if (result)
                return result;
            if (not module)
                return m3Err_dylinkDependencyMissing;
            result = AddObject (state, name, module, UINT32_MAX,
                                & dependency);
            if (result)
                return result;
        }
        else dependency = (u32) found;

        M3Result result = CollectDependencies (state, dependency, options);
        if (result)
            return result;
        object = & state->objects [index]; // AddObject may have reallocated
    }
    object->visit = 2;
    return m3Err_none;
}


static void  TopologicalVisit  (M3DylinkState * state, u32 index,
                                u32 * order, u32 * io_count)
{
    M3DylinkObject * object = & state->objects [index];
    if (object->visit == 2)
        return;
    if (object->visit == 1)
        return;
    object->visit = 1;
    M3DylinkMetadata * metadata = (M3DylinkMetadata *) object->module->dylink;
    for (u32 i = 0; i < metadata->numNeeded; ++i)
    {
        i32 dependency = FindDependencyIndex (state, metadata->needed [i]);
        if (dependency >= 0)
            TopologicalVisit (state, (u32) dependency, order, io_count);
    }
    object = & state->objects [index];
    object->visit = 2;
    order [(*io_count)++] = index;
}


static void  TopologicalVisitNew  (M3DylinkState * state, u32 index,
                                   u32 firstObject, u32 * order,
                                   u32 * io_count)
{
    if (index < firstObject)
        return;
    M3DylinkObject * object = & state->objects [index];
    if (object->visit == 2 or object->visit == 1)
        return;
    object->visit = 1;
    M3DylinkMetadata * metadata = (M3DylinkMetadata *) object->module->dylink;
    for (u32 i = 0; i < metadata->numNeeded; ++i)
    {
        i32 dependency = FindDependencyIndex (state, metadata->needed [i]);
        if (dependency >= 0)
            TopologicalVisitNew (state, (u32) dependency, firstObject,
                                 order, io_count);
    }
    object = & state->objects [index];
    object->visit = 2;
    order [(*io_count)++] = index;
}


static bool  NamesEqual  (const char * a, const char * b)
{
    return a and b and strcmp (a, b) == 0;
}


static u32  ImportFlags  (IM3Module module, const char * importModule,
                          const char * name)
{
    M3DylinkMetadata * metadata = (M3DylinkMetadata *) module->dylink;
    for (u32 i = 0; i < metadata->numImports; ++i)
    {
        M3DylinkInfo * info = & metadata->imports [i];
        if (NamesEqual (info->module, importModule)
            and NamesEqual (info->name, name))
            return info->flags;
    }
    return 0;
}


static u32  ExportFlags  (IM3Module module, const char * name)
{
    M3DylinkMetadata * metadata = (M3DylinkMetadata *) module->dylink;
    for (u32 i = 0; i < metadata->numExports; ++i)
        if (NamesEqual (metadata->exports [i].name, name))
            return metadata->exports [i].flags;
    return 0;
}


static M3Result  ValidateObject  (M3DylinkObject * object, bool isMain)
{
    IM3Module module = object->module;
    M3DylinkMetadata * metadata = (M3DylinkMetadata *) module->dylink;
    // LLVM PIE mains normally define the group's memory themselves, while
    // side modules import it.  wasm3 allocates the final combined memory
    // before loading either kind, so the main definition becomes layout
    // metadata rather than a second allocation.
    if (module->memoryImported
        ? (not NamesEqual (module->memoryImport.moduleUtf8, "env")
           or not NamesEqual (module->memoryImport.fieldUtf8, "memory"))
        : not isMain)
        return m3Err_dylinkUnsupported;
    if (module->table0Imported
        ? (not NamesEqual (module->table0Import.moduleUtf8, "env")
           or not NamesEqual (module->table0Import.fieldUtf8,
                              "__indirect_function_table"))
        : not isMain)
        return m3Err_dylinkUnsupported;
    if (metadata->memoryAlignment >= 32
        or metadata->tableAlignment >= 32)
        return m3Err_dylinkUnsupported;
    for (u32 i = 0; i < metadata->numExports; ++i)
        if (metadata->exports [i].flags & c_m3DylinkTLS)
            return m3Err_dylinkUnsupported;
    for (u32 i = 0; i < metadata->numImports; ++i)
        if (metadata->imports [i].flags & c_m3DylinkTLS)
            return m3Err_dylinkUnsupported;
    return m3Err_none;
}


static bool  AllocateAligned  (u32 * io_cursor, u32 alignmentLog,
                               u32 size, u32 * o_base)
{
    if (alignmentLog >= 32)
        return false;
    u32 mask = alignmentLog ? ((1u << alignmentLog) - 1u) : 0;
    if (*io_cursor > UINT32_MAX - mask)
        return false;
    u32 base = (*io_cursor + mask) & ~mask;
    if (size > UINT32_MAX - base)
        return false;
    *o_base = base;
    *io_cursor = base + size;
    return true;
}


static bool  AlignValue  (u32 value, u32 alignmentLog, u32 * o_value)
{
    if (alignmentLog >= 32)
        return false;
    u32 mask = alignmentLog ? ((1u << alignmentLog) - 1u) : 0;
    if (value > UINT32_MAX - mask)
        return false;
    *o_value = (value + mask) & ~mask;
    return true;
}


static bool  AllocateRange  (M3DylinkRange ** io_freeRanges,
                             u32 * io_cursor, u32 alignmentLog,
                             u32 size, u32 * o_base)
{
    if (not size)
        return AlignValue (*io_cursor, alignmentLog, o_base);

    M3DylinkRange ** link = io_freeRanges;
    while (*link)
    {
        M3DylinkRange * range = *link;
        u32 base;
        if (AlignValue (range->base, alignmentLog, & base)
            and base >= range->base
            and base - range->base <= range->size
            and size <= range->size - (base - range->base))
        {
            u32 prefix = base - range->base;
            u32 suffixBase = base + size;
            u32 suffix = range->size - prefix - size;
            M3DylinkRange * suffixRange = NULL;
            if (prefix and suffix)
            {
                suffixRange = m3_AllocStruct (M3DylinkRange);
                if (not suffixRange)
                {
                    link = & range->next;
                    continue;
                }
                suffixRange->base = suffixBase;
                suffixRange->size = suffix;
                suffixRange->next = range->next;
            }
            if (prefix)
            {
                range->size = prefix;
                if (suffixRange)
                    range->next = suffixRange;
            }
            else if (suffix)
            {
                range->base = suffixBase;
                range->size = suffix;
            }
            else
            {
                *link = range->next;
                m3_Free (range);
            }
            *o_base = base;
            return true;
        }
        link = & range->next;
    }
    return AllocateAligned (io_cursor, alignmentLog, size, o_base);
}


static void  FreeRange  (M3DylinkRange ** io_freeRanges,
                         u32 base, u32 size)
{
    if (not size)
        return;
    M3DylinkRange ** link = io_freeRanges;
    while (*link and (*link)->base < base)
        link = & (*link)->next;

    M3DylinkRange * range = m3_AllocStruct (M3DylinkRange);
    if (not range)
        return;
    range->base = base;
    range->size = size;
    range->next = *link;
    *link = range;

    M3DylinkRange * previous = NULL;
    M3DylinkRange * current = *io_freeRanges;
    while (current)
    {
        if (previous and previous->base <= UINT32_MAX - previous->size
            and previous->base + previous->size == current->base)
        {
            previous->size += current->size;
            previous->next = current->next;
            m3_Free (current);
            current = previous->next;
            continue;
        }
        previous = current;
        current = current->next;
    }
}


static bool  FunctionIsExported  (IM3Function function, const char * name)
{
    return function->export_name and NamesEqual (function->export_name, name);
}


static bool  ObjectIsVisible  (const M3DylinkState * state,
                               const M3DylinkObject * requester,
                               const M3DylinkObject * candidate)
{
    if (not requester or requester == candidate)
        return true;
    if (candidate->programIndex == UINT32_MAX)
        return true;
    if (requester->programIndex == UINT32_MAX)
        return not state->dynamicPrograms and state->numActiveContexts == 1;
    return requester->programIndex != UINT32_MAX
        and requester->programIndex == candidate->programIndex;
}


static M3Result  FindFunctionSymbol  (M3DylinkState * state,
                                      M3DylinkObject * requester,
                                      const char * name,
                                      IM3Function * o_function,
                                      M3DylinkObject ** o_owner)
{
    IM3Function found = NULL;
    M3DylinkObject * owner = NULL;
    bool foundWeak = false;
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
        if (not ObjectIsVisible (state, requester, & state->objects [oi]))
            continue;
        IM3Module module = state->objects [oi].module;
        u32 flags = ExportFlags (module, name);
        if (flags & (c_m3DylinkBindingLocal | c_m3DylinkVisibilityHidden))
            continue;
        for (u32 fi = 0; fi < module->numFunctions; ++fi)
        {
            IM3Function candidate = & module->functions [fi];
            if (not FunctionIsExported (candidate, name))
                continue;
            bool weak = (flags & c_m3DylinkBindingWeak) != 0;
            if (found and not foundWeak and not weak and found != candidate)
                return m3Err_dylinkDuplicateSymbol;
            if (not found or (foundWeak and not weak))
            {
                found = candidate;
                owner = & state->objects [oi];
                foundWeak = weak;
            }
        }
    }
    *o_function = found;
    if (o_owner)
        *o_owner = owner;
    return m3Err_none;
}


static M3Result  FindGlobalSymbol  (M3DylinkState * state,
                                    M3DylinkObject * requester,
                                    const char * name,
                                    M3Global ** o_global,
                                    M3DylinkObject ** o_owner)
{
    M3Global * found = NULL;
    M3DylinkObject * owner = NULL;
    bool foundWeak = false;
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
        if (not ObjectIsVisible (state, requester, & state->objects [oi]))
            continue;
        IM3Module module = state->objects [oi].module;
        u32 flags = ExportFlags (module, name);
        if (flags & (c_m3DylinkBindingLocal | c_m3DylinkVisibilityHidden))
            continue;
        for (u32 gi = 0; gi < module->numGlobals; ++gi)
        {
            M3Global * candidate = & module->globals [gi];
            if (not candidate->name or not NamesEqual (candidate->name, name))
                continue;
            bool weak = (flags & c_m3DylinkBindingWeak) != 0;
            if (found and not foundWeak and not weak and found != candidate)
                return m3Err_dylinkDuplicateSymbol;
            if (not found or (foundWeak and not weak))
            {
                found = candidate;
                owner = & state->objects [oi];
                foundWeak = weak;
            }
        }
    }
    *o_global = found;
    if (o_owner)
        *o_owner = owner;
    return m3Err_none;
}


static bool  IsSpecialGlobal  (M3Global * global)
{
    const char * module = global->import.moduleUtf8;
    const char * name = global->import.fieldUtf8;
    if (NamesEqual (module, "GOT.func") or NamesEqual (module, "GOT.mem"))
        return true;
    if (not NamesEqual (module, "env"))
        return false;
    return NamesEqual (name, "__memory_base")
        or NamesEqual (name, "__table_base")
        or NamesEqual (name, "__stack_pointer")
        or NamesEqual (name, "__stack_low")
        or NamesEqual (name, "__stack_high")
        or NamesEqual (name, "__heap_base")
        or NamesEqual (name, "__heap_end");
}


static M3Result  LinkFunctionsFrom  (M3DylinkState * state, u32 firstObject)
{
    for (u32 oi = firstObject; oi < state->numObjects; ++oi)
    {
        M3DylinkObject * object = & state->objects [oi];
        IM3Module module = object->module;
        for (u32 fi = 0; fi < module->numFuncImports; ++fi)
        {
            IM3Function import = & module->functions [fi];
            IM3Function target = NULL;
            M3Result result = FindFunctionSymbol (state, object,
                                                  import->import.fieldUtf8,
                                                  & target, NULL);
            if (result)
                return result;
            if (target and target != import)
            {
                if (not AreFuncTypesEqual (import->funcType, target->funcType))
                    return m3Err_dylinkTypeMismatch;
                import->linkedFunction = target;
            }
        }
    }
    return m3Err_none;
}


static M3Result  LinkGlobalsAndBasesFrom  (M3DylinkState * state,
                                           u32 firstObject)
{
    bool foundStackPointer = firstObject != 0;
    for (u32 oi = firstObject; oi < state->numObjects; ++oi)
    {
        M3DylinkObject * object = & state->objects [oi];
        IM3Module module = object->module;
        for (u32 gi = 0; gi < module->numGlobals; ++gi)
        {
            M3Global * global = & module->globals [gi];
            if (not global->imported)
                continue;
            const char * importModule = global->import.moduleUtf8;
            const char * name = global->import.fieldUtf8;
            if (global->type != c_m3Type_i32 and IsSpecialGlobal (global))
                return m3Err_dylinkTypeMismatch;

            if (NamesEqual (importModule, "env")
                and NamesEqual (name, "__memory_base"))
                global->i32Value = (i32) object->memoryBase;
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__table_base"))
                global->i32Value = (i32) object->tableBase;
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__stack_pointer"))
            {
                if (not global->isMutable or global->type != c_m3Type_i32)
                    return m3Err_dylinkTypeMismatch;
                global->linkedGlobal = state->stackPointer;
                foundStackPointer = true;
            }
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__stack_low"))
            {
                if (not state->stackLowConfigured)
                {
                    state->stackLowStorage.type = global->type;
                    state->stackLowStorage.isMutable = global->isMutable;
                    state->stackLowConfigured = true;
                }
                else if (global->type != state->stackLowStorage.type
                         or global->isMutable !=
                            state->stackLowStorage.isMutable)
                    return m3Err_dylinkTypeMismatch;
                global->linkedGlobal = state->stackLowGlobal;
            }
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__stack_high"))
            {
                if (not state->stackHighConfigured)
                {
                    state->stackHighStorage.type = global->type;
                    state->stackHighStorage.isMutable = global->isMutable;
                    state->stackHighConfigured = true;
                }
                else if (global->type != state->stackHighStorage.type
                         or global->isMutable !=
                            state->stackHighStorage.isMutable)
                    return m3Err_dylinkTypeMismatch;
                global->linkedGlobal = state->stackHighGlobal;
            }
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__heap_base"))
            {
                IM3DylinkContext context =
                    object->programIndex != UINT32_MAX
                    and object->programIndex < state->numContexts
                        ? state->contexts [object->programIndex] : NULL;
                global->i32Value = (i32) (context
                    ? context->linearHeapLow : state->heapBase);
            }
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__heap_end"))
            {
                IM3DylinkContext context =
                    object->programIndex != UINT32_MAX
                    and object->programIndex < state->numContexts
                        ? state->contexts [object->programIndex] : NULL;
                global->i32Value = (i32) (context
                    ? context->linearHeapHigh : state->heapEnd);
            }
            else if (NamesEqual (importModule, "GOT.func")
                     or NamesEqual (importModule, "GOT.mem"))
                continue;
            else
            {
                M3Global * target = NULL;
                M3Result result = FindGlobalSymbol (state, object, name,
                                                    & target, NULL);
                if (result)
                    return result;
                if (target and target != global)
                {
                    if (target->type != global->type
                        or target->isMutable != global->isMutable)
                        return m3Err_dylinkTypeMismatch;
                    global->linkedGlobal = target;
                }
            }
        }
    }
    return foundStackPointer ? m3Err_none : m3Err_dylinkUnresolvedSymbol;
}


static u32  CountFunctionExportsFrom  (M3DylinkState * state,
                                       u32 firstObject)
{
    u32 count = 0;
    for (u32 oi = firstObject; oi < state->numObjects; ++oi)
    {
        M3DylinkObject * object = & state->objects [oi];
        IM3Module module = object->module;
        for (u32 fi = 0; fi < module->numFunctions; ++fi)
            if (module->functions [fi].export_name)
                ++count;
    }
    return count;
}


static i32  FindTableFunction  (M3DylinkState * state, IM3Function function)
{
    function = m3d_ResolveFunction (function);
    for (u32 i = 1; i < state->tableSize; ++i)
        if (m3d_ResolveFunction (state->table [i]) == function)
            return (i32) i;
    return -1;
}


static M3Result  FillExportTableFrom  (M3DylinkState * state,
                                       u32 firstObject, u32 firstExport)
{
    u32 cursor = firstExport;
    for (u32 oi = firstObject; oi < state->numObjects; ++oi)
    {
        M3DylinkObject * object = & state->objects [oi];
        IM3Module module = object->module;
        object->exportTableBase = cursor;
        for (u32 fi = 0; fi < module->numFunctions; ++fi)
        {
            IM3Function function = & module->functions [fi];
            if (not function->export_name)
                continue;
            function = m3d_ResolveFunction (function);
            if (not function or not function->module)
                continue;
            if (FindTableFunction (state, function) >= 0)
                continue;
            if (cursor >= state->tableSize)
                return m3Err_dylinkUnsupported;
            state->table [cursor++] = function;
        }
        object->exportTableSize = cursor - object->exportTableBase;
    }
    return m3Err_none;
}


static M3Result  ValidateImportsFrom  (M3DylinkState * state,
                                       u32 firstObject)
{
    for (u32 oi = firstObject; oi < state->numObjects; ++oi)
    {
        IM3Module module = state->objects [oi].module;
        for (u32 fi = 0; fi < module->numFuncImports; ++fi)
        {
            IM3Function import = & module->functions [fi];
            IM3Function target = m3d_ResolveFunction (import);
            if ((target == import and not import->compiled)
                or not target or not target->module)
            {
                u32 flags = ImportFlags (module, import->import.moduleUtf8,
                                         import->import.fieldUtf8);
                if (not (flags & c_m3DylinkBindingWeak))
                    return m3Err_dylinkUnresolvedSymbol;
            }
        }
        for (u32 gi = 0; gi < module->numGlobals; ++gi)
        {
            M3Global * global = & module->globals [gi];
            if (not global->imported or IsSpecialGlobal (global)
                or global->linkedGlobal)
                continue;
            u32 flags = ImportFlags (module, global->import.moduleUtf8,
                                     global->import.fieldUtf8);
            if (not (flags & c_m3DylinkBindingWeak))
                return m3Err_dylinkUnresolvedSymbol;
        }
    }
    return m3Err_none;
}


static M3Result  ResolveGOTFrom  (M3DylinkState * state, u32 firstObject)
{
    for (u32 oi = firstObject; oi < state->numObjects; ++oi)
    {
        M3DylinkObject * object = & state->objects [oi];
        IM3Module module = object->module;
        for (u32 gi = 0; gi < module->numGlobals; ++gi)
        {
            M3Global * global = & module->globals [gi];
            if (not global->imported or global->type != c_m3Type_i32)
                continue;
            const char * importModule = global->import.moduleUtf8;
            const char * name = global->import.fieldUtf8;
            if (NamesEqual (importModule, "GOT.func"))
            {
                IM3Function function = NULL;
                M3Result result = FindFunctionSymbol (state, object, name,
                                                       & function,
                                                       NULL);
                if (result)
                    return result;
                i32 index = function ? FindTableFunction (state, function) : -1;
                if (index < 0)
                {
                    u32 flags = ImportFlags (module, importModule, name);
                    if (not (flags & c_m3DylinkBindingWeak))
                        return m3Err_dylinkUnresolvedSymbol;
                    index = 0;
                }
                global->i32Value = index;
            }
            else if (NamesEqual (importModule, "GOT.mem"))
            {
                M3Global * address = NULL;
                M3DylinkObject * owner = NULL;
                M3Result result = FindGlobalSymbol (state, object, name,
                                                     & address,
                                                     & owner);
                if (result)
                    return result;
                if (not address)
                {
                    u32 flags = ImportFlags (module, importModule, name);
                    if (not (flags & c_m3DylinkBindingWeak))
                        return m3Err_dylinkUnresolvedSymbol;
                    global->i32Value = 0;
                    continue;
                }
                address = m3d_ResolveGlobal (address);
                if (address->type != c_m3Type_i32)
                    return m3Err_dylinkTypeMismatch;
                u32 relative = address->i32Value;
                if (relative > UINT32_MAX - owner->memoryBase)
                    return m3Err_dylinkUnsupported;
                global->i32Value = (i32) (relative + owner->memoryBase);
            }
        }
    }
    return m3Err_none;
}


static IM3Function  FindModuleExport  (IM3Module module, const char * name)
{
    for (u32 i = 0; i < module->numFunctions; ++i)
        if (FunctionIsExported (& module->functions [i], name))
            return m3d_ResolveFunction (& module->functions [i]);
    return NULL;
}


static M3Result  CallOptional  (IM3Module module, const char * name,
                                bool * o_found)
{
    IM3Function function = FindModuleExport (module, name);
    *o_found = function != NULL;
    if (not function)
        return m3Err_none;
    if (GetFunctionNumArgs (function) or GetFunctionNumReturns (function))
        return m3Err_dylinkTypeMismatch;
    M3Result result = m3Err_none;
    if (not function->compiled)
        result = CompileFunction (function);
    if (not result)
        result = m3_CallV (function);
    return result;
}


static M3Result  RunInitializers  (M3DylinkState * state,
                                   const u32 * order, u32 count,
                                   u32 dependencyProgramIndex)
{
    for (u32 i = 0; i < count; ++i)
    {
        M3DylinkObject * object = & state->objects [order [i]];
        u32 programIndex = object->programIndex == UINT32_MAX
                         ? dependencyProgramIndex : object->programIndex;
        M3Result result = ActivateContext (state->contexts [programIndex]);
        if (result)
            return result;
        result = m3_RunStart (object->module);
        if (result)
            return result;
    }
    for (u32 i = 0; i < count; ++i)
    {
        M3DylinkObject * object = & state->objects [order [i]];
        u32 programIndex = object->programIndex == UINT32_MAX
                         ? dependencyProgramIndex : object->programIndex;
        bool found;
        M3Result result = ActivateContext (state->contexts [programIndex]);
        if (result)
            return result;
        result = CallOptional (object->module,
                                        "__wasm_apply_data_relocs", & found);
        if (result)
            return result;
    }
    for (u32 i = 0; i < count; ++i)
    {
        M3DylinkObject * object = & state->objects [order [i]];
        u32 programIndex = object->programIndex == UINT32_MAX
                         ? dependencyProgramIndex : object->programIndex;
        IM3Module module = object->module;
        bool found;
        M3Result result = ActivateContext (state->contexts [programIndex]);
        if (result)
            return result;
        result = CallOptional (module, "_initialize", & found);
        if (result)
            return result;
        if (not found)
        {
            result = CallOptional (module, "__wasm_call_ctors", & found);
            if (result)
                return result;
        }
    }
    return m3Err_none;
}


static void  ReleaseUnloadedState  (M3DylinkState * state)
{
    if (not state)
        return;
    for (u32 i = 0; i < state->numObjects; ++i)
    {
        M3DylinkObject * object = & state->objects [i];
        if (object->programIndex == UINT32_MAX and object->module
            and not object->module->runtime)
            m3_FreeModule (object->module);
        m3_Free (object->name);
    }
    for (u32 i = 0; i < state->numContexts; ++i)
    {
        IM3DylinkContext context = state->contexts [i];
        if (context and not context->ownsStack
            and context->continuationFrames ==
                state->runtime->continuationFrames)
        {
            context->continuationFrames = NULL;
#if d_m3RecordBacktraces
            M3_INIT (context->backtrace);
#endif
        }
        FreeContextStorage (context);
    }
    m3_Free (state->contexts);
    m3_Free (state->objects);
    m3_Free (state->table);
    FreeRangeList (state->freeMemoryRanges);
    FreeRangeList (state->freeTableRanges);
    m3_Free (state);
}


static M3Result  CreateContexts  (M3DylinkState * state,
                                  const M3DylinkProgram * programs,
                                  u32 numPrograms,
                                  const M3DylinkOptions * options,
    u32 * memoryCursor)
{
    IM3Runtime runtime = state->runtime;
    state->contexts = m3_AllocArray (IM3DylinkContext, numPrograms);
    if (not state->contexts)
        return m3Err_mallocFailed;
    state->numContexts = numPrograms;
    state->contextCapacity = numPrograms;

    for (u32 i = 0; i < numPrograms; ++i)
    {
        IM3DylinkContext context = m3_AllocStruct (struct M3DylinkContext);
        if (not context)
            return m3Err_mallocFailed;
        state->contexts [i] = context;
        context->state = state;
        context->module = programs [i].module;
        context->userdata = programs [i].userdata;
        context->programIndex = i;

        u32 nativeStackSize = programs [i].nativeStackSize
                            ? programs [i].nativeStackSize
                            : runtime->stackSize;
        if (nativeStackSize < sizeof (m3slot_t))
            return m3Err_dylinkUnsupported;
        if (i == 0)
        {
            if (nativeStackSize != runtime->stackSize)
                return m3Err_dylinkUnsupported;
            context->stack = runtime->stack;
            context->continuationFrames = runtime->continuationFrames;
            context->numContinuationFrames = runtime->numContinuationFrames;
            context->maxContinuationFrames = runtime->maxContinuationFrames;
            context->fuel = runtime->fuel;
            context->fuelEnabled = runtime->fuelEnabled;
            context->suspended = runtime->suspended;
            context->suspendedFunction = runtime->suspendedFunction;
            context->lastCalled = runtime->lastCalled;
#if d_m3EnableStrace >= 2
            context->callDepth = runtime->callDepth;
#endif
#if d_m3RecordBacktraces
            context->backtrace = runtime->backtrace;
#endif
        }
        else
        {
            context->stack = m3_Malloc ("Dylink context stack",
                                        nativeStackSize
                                        + 4 * sizeof (m3slot_t));
            if (not context->stack)
                return m3Err_mallocFailed;
            context->ownsStack = true;
        }
        context->stackSize = nativeStackSize;
        context->numStackSlots = nativeStackSize / sizeof (m3slot_t);

        u32 linearStackSize = programs [i].linearStackSize
                            ? programs [i].linearStackSize
                            : (options and options->linearStackSize
                                ? options->linearStackSize
                                : c_m3DylinkDefaultStackSize);
        if (not AllocateAligned (memoryCursor, 4, linearStackSize,
                                 & context->linearStackLow)
            or not AllocateAligned (memoryCursor, 4, 0,
                                    & context->linearStackHigh))
            return m3Err_dylinkUnsupported;
        context->linearStackPointer = context->linearStackHigh;

        if (programs [i].linearHeapSize)
        {
            if (not AllocateAligned (memoryCursor, 4,
                                     programs [i].linearHeapSize,
                                     & context->linearHeapLow)
                or not AllocateAligned (memoryCursor, 4, 0,
                                        & context->linearHeapHigh))
                return m3Err_dylinkUnsupported;
            context->ownsLinearHeap = true;
        }
    }

    state->numActiveContexts = numPrograms;
    state->stackLow = state->contexts [0]->linearStackLow;
    state->stackHigh = state->contexts [0]->linearStackHigh;
    return m3Err_none;
}


static M3Result  ReserveContextSlot  (M3DylinkState * state, u32 * o_slot)
{
    for (u32 i = 0; i < state->numContexts; ++i)
    {
        if (not state->contexts [i])
        {
            *o_slot = i;
            return m3Err_none;
        }
    }
    if (state->numContexts == state->contextCapacity)
    {
        u32 oldCapacity = state->contextCapacity;
        u32 newCapacity = oldCapacity ? oldCapacity * 2u : 2u;
        if (newCapacity < oldCapacity)
            return m3Err_mallocFailed;
        IM3DylinkContext * contexts =
            m3_ReallocArray (IM3DylinkContext, state->contexts,
                             newCapacity, oldCapacity);
        if (not contexts)
            return m3Err_mallocFailed;
        state->contexts = contexts;
        state->contextCapacity = newCapacity;
    }
    *o_slot = state->numContexts++;
    return m3Err_none;
}


static bool  OriginStackInUse  (M3DylinkState * state)
{
    for (u32 i = 0; i < state->numContexts; ++i)
        if (state->contexts [i]
            and state->contexts [i]->stack == state->runtime->originStack)
            return true;
    return false;
}


static M3Result  CreateAddedContext  (M3DylinkState * state,
                                      const M3DylinkProgram * program,
                                      const M3DylinkOptions * options,
                                      u32 slot,
                                      IM3DylinkContext * o_context)
{
    IM3DylinkContext context = m3_AllocStruct (struct M3DylinkContext);
    if (not context)
        return m3Err_mallocFailed;
    context->state = state;
    context->module = program->module;
    context->userdata = program->userdata;
    context->programIndex = slot;

    u32 nativeStackSize = program->nativeStackSize
                        ? program->nativeStackSize : state->originStackSize;
    if (nativeStackSize < sizeof (m3slot_t))
    {
        m3_Free (context);
        return m3Err_dylinkUnsupported;
    }
    if (not OriginStackInUse (state)
        and nativeStackSize == state->originStackSize)
        context->stack = state->runtime->originStack;
    else
    {
        context->stack = m3_Malloc ("Dylink context stack",
                                    nativeStackSize
                                    + 4 * sizeof (m3slot_t));
        if (not context->stack)
        {
            m3_Free (context);
            return m3Err_mallocFailed;
        }
        context->ownsStack = true;
    }
    context->stackSize = nativeStackSize;
    context->numStackSlots = nativeStackSize / sizeof (m3slot_t);

    u32 linearStackSize = program->linearStackSize
                        ? program->linearStackSize
                        : (options and options->linearStackSize
                            ? options->linearStackSize
                            : c_m3DylinkDefaultStackSize);
    if (not AllocateRange (& state->freeMemoryRanges,
                           & state->memoryCursor, 4, linearStackSize,
                           & context->linearStackLow))
    {
        if (context->ownsStack)
            m3_Free (context->stack);
        m3_Free (context);
        return m3Err_dylinkUnsupported;
    }
    context->linearStackHigh = context->linearStackLow + linearStackSize;
    context->linearStackPointer = context->linearStackHigh;

    u32 linearHeapSize = program->linearHeapSize;
    if (not linearHeapSize)
    {
        M3DylinkMetadata * metadata =
            (M3DylinkMetadata *) program->module->dylink;
        u64 minimumBytes = (u64) program->module->memoryInfo.initPages
                         * state->runtime->memory.pageSize;
        u64 reservedBytes = (u64) metadata->memorySize + linearStackSize;
        if (minimumBytes > reservedBytes)
        {
            u64 derived = minimumBytes - reservedBytes;
            if (derived > UINT32_MAX)
            {
                FreeRange (& state->freeMemoryRanges,
                           context->linearStackLow, linearStackSize);
                if (context->ownsStack)
                    m3_Free (context->stack);
                m3_Free (context);
                return m3Err_dylinkUnsupported;
            }
            linearHeapSize = (u32) derived;
        }
    }
    if (linearHeapSize)
    {
        if (not AllocateRange (& state->freeMemoryRanges,
                               & state->memoryCursor, 4,
                               linearHeapSize,
                               & context->linearHeapLow))
        {
            FreeRange (& state->freeMemoryRanges, context->linearStackLow,
                       linearStackSize);
            if (context->ownsStack)
                m3_Free (context->stack);
            m3_Free (context);
            return m3Err_dylinkUnsupported;
        }
        context->linearHeapHigh = context->linearHeapLow
                                + linearHeapSize;
        context->ownsLinearHeap = true;
    }
    else
        context->linearHeapLow = context->linearHeapHigh =
            context->linearStackHigh;

    state->contexts [slot] = context;
    ++state->numActiveContexts;
    *o_context = context;
    return m3Err_none;
}


static void  ZeroLinearRange  (M3DylinkState * state, u32 base, u32 size)
{
    if (not state or not size or not state->runtime->memory.mallocated)
        return;
    u32 length = (u32) state->runtime->memory.mallocated->length;
    if (base <= length and size <= length - base)
        memset (m3MemData (state->runtime->memory.mallocated) + base, 0, size);
}


static void  ReleaseContextRanges  (M3DylinkState * state,
                                    IM3DylinkContext context)
{
    if (not state or not context)
        return;
    u32 stackSize = context->linearStackHigh - context->linearStackLow;
    ZeroLinearRange (state, context->linearStackLow, stackSize);
    FreeRange (& state->freeMemoryRanges, context->linearStackLow, stackSize);
    if (context->ownsLinearHeap)
    {
        u32 heapSize = context->linearHeapHigh - context->linearHeapLow;
        ZeroLinearRange (state, context->linearHeapLow, heapSize);
        FreeRange (& state->freeMemoryRanges, context->linearHeapLow,
                   heapSize);
    }
}


static void  UnlinkRuntimeModule  (IM3Runtime runtime, IM3Module module)
{
    if (not runtime or not module)
        return;
    IM3Module * link = & runtime->modules;
    while (*link and *link != module)
        link = & (*link)->next;
    if (*link == module)
        *link = module->next;
    module->next = NULL;
    module->runtime = NULL;
}


static void  ReleaseObjectRanges  (M3DylinkState * state,
                                   M3DylinkObject * object)
{
    if (object->memorySize)
    {
        ZeroLinearRange (state, object->memoryBase, object->memorySize);
        FreeRange (& state->freeMemoryRanges, object->memoryBase,
                   object->memorySize);
    }
    if (object->tableSize)
    {
        if (object->tableBase <= state->tableSize
            and object->tableSize <= state->tableSize - object->tableBase)
            memset (state->table + object->tableBase, 0,
                    object->tableSize * sizeof (IM3Function));
        FreeRange (& state->freeTableRanges, object->tableBase,
                   object->tableSize);
    }
    if (object->exportTableSize)
    {
        if (object->exportTableBase <= state->tableSize
            and object->exportTableSize <=
                state->tableSize - object->exportTableBase)
            memset (state->table + object->exportTableBase, 0,
                    object->exportTableSize * sizeof (IM3Function));
        FreeRange (& state->freeTableRanges, object->exportTableBase,
                   object->exportTableSize);
    }
}


static M3Result  LoadGroup  (IM3Runtime runtime,
                              const M3DylinkProgram * programs,
                              uint32_t numPrograms,
                              const M3DylinkOptions * options,
                              IM3DylinkContext * outContexts,
                              bool dynamicPrograms)
{
    if (not runtime or not programs or not numPrograms)
        return m3Err_dylinkMissingSection;
    if (runtime->modules or runtime->dylinkState)
        return m3Err_moduleAlreadyLinked;
    for (u32 i = 0; i < numPrograms; ++i)
    {
        if (outContexts)
            outContexts [i] = NULL;
        if (not programs [i].module or not programs [i].module->dylink)
            return m3Err_dylinkMissingSection;
    }

    M3DylinkState * state = m3_AllocStruct (M3DylinkState);
    if (not state)
        return m3Err_mallocFailed;
    state->runtime = runtime;
    state->originStackSize = runtime->stackSize;
    state->originNumStackSlots = runtime->numStackSlots;
    state->originUserdata = dynamicPrograms ? NULL : runtime->userdata;
    state->dynamicPrograms = dynamicPrograms;
    state->stackPointerStorage.type = c_m3Type_i32;
    state->stackPointerStorage.isMutable = true;
    state->stackPointer = & state->stackPointerStorage;
    state->stackLowGlobal = & state->stackLowStorage;
    state->stackHighGlobal = & state->stackHighStorage;

    u32 * programObjectIndices = m3_AllocArray (u32, numPrograms);
    if (not programObjectIndices)
    {
        ReleaseUnloadedState (state);
        return m3Err_mallocFailed;
    }
    M3Result result = m3Err_none;
    for (u32 i = 0; i < numPrograms; ++i)
    {
        const char * name = programs [i].name ? programs [i].name : "main";
        result = AddObject (state, name, programs [i].module, i,
                            & programObjectIndices [i]);
        if (result)
            goto _catch;
    }
    for (u32 i = 0; i < numPrograms; ++i)
    {
        result = CollectDependencies (state, programObjectIndices [i], options);
        if (result)
            goto _catch;
    }

    u32 * order = m3_AllocArray (u32, state->numObjects);
    if (state->numObjects and not order)
    {
        result = m3Err_mallocFailed;
        goto _catch;
    }
    for (u32 i = 0; i < state->numObjects; ++i)
        state->objects [i].visit = 0;
    u32 orderCount = 0;
    for (u32 i = 0; i < numPrograms; ++i)
        TopologicalVisit (state, programObjectIndices [i], order, & orderCount);
    for (u32 i = 0; i < state->numObjects; ++i)
        if (state->objects [i].visit != 2)
            TopologicalVisit (state, i, order, & orderCount);

    u32 memoryCursor = 0;
    u32 tableCursor = 1; // table slot zero is the null function pointer
    u32 pageSize = 0;
    u32 minimumPages = 0;
    u32 maximumPages = 0;
    u32 minimumTable = 1;
    u32 maximumTable = 0;
    for (u32 i = 0; i < state->numObjects; ++i)
    {
        M3DylinkObject * object = & state->objects [i];
        result = ValidateObject (object,
                                 object->programIndex != UINT32_MAX);
        if (result)
            goto _catch_order;
        M3DylinkMetadata * metadata = (M3DylinkMetadata *) object->module->dylink;
        if (not AllocateAligned (& memoryCursor, metadata->memoryAlignment,
                                 metadata->memorySize, & object->memoryBase)
            or not AllocateAligned (& tableCursor, metadata->tableAlignment,
                                    metadata->tableSize, & object->tableBase))
        {
            result = m3Err_dylinkUnsupported;
            goto _catch_order;
        }
        object->memorySize = metadata->memorySize;
        object->tableSize = metadata->tableSize;

        u32 modulePageSize = object->module->memoryInfo.pageSize
                           ? object->module->memoryInfo.pageSize
                           : d_m3DefaultMemPageSize;
        if (pageSize and pageSize != modulePageSize)
        {
            result = m3Err_dylinkUnsupported;
            goto _catch_order;
        }
        pageSize = modulePageSize;
        minimumPages = M3_MAX (minimumPages,
                               object->module->memoryInfo.initPages);
        u32 moduleMax = object->module->memoryInfo.maxPages;
        if (moduleMax and (not maximumPages or moduleMax < maximumPages))
            maximumPages = moduleMax;
        minimumTable = M3_MAX (minimumTable, object->module->table0InitSize);
        u32 tableMax = object->module->table0MaxSize;
        if (tableMax and (not maximumTable or tableMax < maximumTable))
            maximumTable = tableMax;
    }

    result = CreateContexts (state, programs, numPrograms, options,
                             & memoryCursor);
    if (result)
        goto _catch_order;
    state->stackPointerStorage.i32Value = (i32) state->stackHigh;
    state->stackLowStorage.i32Value = (i32) state->stackLow;
    state->stackHighStorage.i32Value = (i32) state->stackHigh;
    state->heapBase = memoryCursor;
    if (not pageSize)
        pageSize = d_m3DefaultMemPageSize;
    u64 requiredBytes = memoryCursor;
    u64 importedBytes = (u64) minimumPages * pageSize;
    if (requiredBytes < importedBytes)
        requiredBytes = importedBytes;
    u64 pages64 = (requiredBytes + pageSize - 1u) / pageSize;
    if (pages64 > UINT32_MAX
        or (maximumPages and pages64 > maximumPages))
    {
        result = m3Err_wasmMemoryOverflow;
        goto _catch_order;
    }
    u32 pages = (u32) pages64;
    u64 allocatedBytes = pages64 * pageSize;
    if (allocatedBytes > UINT32_MAX)
    {
        result = m3Err_dylinkUnsupported;
        goto _catch_order;
    }
    state->heapEnd = (u32) allocatedBytes;
    state->memoryCursor = state->heapEnd;
    for (u32 i = 0; i < state->numContexts; ++i)
    {
        IM3DylinkContext context = state->contexts [i];
        if (not context->linearHeapHigh)
        {
            context->linearHeapLow = state->heapBase;
            context->linearHeapHigh = state->heapEnd;
        }
    }

    u32 exportCount = CountFunctionExportsFrom (state, 0);
    if (exportCount > UINT32_MAX - tableCursor)
    {
        result = m3Err_dylinkUnsupported;
        goto _catch_order;
    }
    u32 firstExport = tableCursor;
    tableCursor += exportCount;
    state->tableSize = M3_MAX (tableCursor, minimumTable);
    state->tableCursor = state->tableSize;
    state->maximumTable = maximumTable;
    if (maximumTable and state->tableSize > maximumTable)
    {
        result = m3Err_dylinkUnsupported;
        goto _catch_order;
    }
    state->table = m3_AllocArray (IM3Function, state->tableSize);
    if (state->tableSize and not state->table)
    {
        result = m3Err_mallocFailed;
        goto _catch_order;
    }

    runtime->memory.pageSize = pageSize;
    runtime->memory.maxPages = maximumPages ? maximumPages : 65536;
    result = ResizeMemory (runtime, pages);
    if (result)
        goto _catch_order;
    if (not runtime->memory.mallocated
        or runtime->memory.mallocated->length < requiredBytes)
    {
        result = m3Err_wasmMemoryOverflow;
        goto _catch_order;
    }

    result = LinkFunctionsFrom (state, 0);
    if (result)
        goto _catch_order;
    result = LinkGlobalsAndBasesFrom (state, 0);
    if (result)
        goto _catch_order;
    result = FillExportTableFrom (state, 0, firstExport);
    if (result)
        goto _catch_order;

    // Give the embedder a chance to bind non-dylink host imports while no
    // module ownership has yet moved into the runtime.
    if (options and options->linkHostImports)
    {
        for (u32 i = 0; i < orderCount; ++i)
        {
            IM3Module module = state->objects [order [i]].module;
            module->runtime = runtime;
            result = options->linkHostImports (options->context, module);
            module->runtime = NULL;
            if (result)
                goto _catch_order;
        }
    }
    result = ValidateImportsFrom (state, 0);
    if (result)
        goto _catch_order;

    runtime->dylinkState = state;
    for (u32 i = 0; i < state->numObjects; ++i)
        m3_SetModuleName (state->objects [i].module, state->objects [i].name);
    for (u32 i = 0; i < orderCount; ++i)
    {
        result = m3_LoadModule (runtime, state->objects [order [i]].module);
        if (result)
            goto _catch_loaded;
    }
    result = ResolveGOTFrom (state, 0);
    if (result)
        goto _catch_loaded;
    result = ActivateContext (state->contexts [0]);
    if (result)
        goto _catch_loaded;
    result = RunInitializers (state, order, orderCount, 0);

    if (not result and outContexts)
        for (u32 i = 0; i < numPrograms; ++i)
            outContexts [i] = state->contexts [i];

_catch_loaded:
    m3_Free (order);
    m3_Free (programObjectIndices);
    return result;

_catch_order:
    m3_Free (order);
_catch:
    m3_Free (programObjectIndices);
    ReleaseUnloadedState (state);
    return result;
}


M3Result  m3_DylinkLoadGroup  (IM3Runtime runtime,
                                const M3DylinkProgram * programs,
                                uint32_t numPrograms,
                                const M3DylinkOptions * options,
                                IM3DylinkContext * outContexts)
{
    return LoadGroup (runtime, programs, numPrograms, options, outContexts,
                      true);
}


static void  TrimContextSlots  (M3DylinkState * state)
{
    while (state->numContexts and
           not state->contexts [state->numContexts - 1])
        --state->numContexts;
}


static i32  FindProgramObjectIndex  (M3DylinkState * state,
                                     IM3Module module)
{
    for (u32 i = 0; i < state->numObjects; ++i)
        if (state->objects [i].programIndex != UINT32_MAX
            and state->objects [i].module == module)
            return (i32) i;
    return -1;
}


static bool  ProgramNameExists  (M3DylinkState * state, const char * name)
{
    for (u32 i = 0; i < state->numObjects; ++i)
        if (state->objects [i].programIndex != UINT32_MAX
            and NamesEqual (state->objects [i].name, name))
            return true;
    return false;
}


static void  RollBackAddedProgram  (M3DylinkState * state,
                                    u32 firstObject,
                                    IM3Module programModule,
                                    IM3DylinkContext context,
                                    IM3DylinkContext previousActive)
{
    if (context and state->activeContext == context)
    {
        if (previousActive and previousActive->state == state)
            (void) ActivateContext (previousActive);
        else
            ParkRuntime (state);
    }
    if (context)
    {
        u32 slot = context->programIndex;
        if (slot < state->numContexts and state->contexts [slot] == context)
            state->contexts [slot] = NULL;
        if (state->numActiveContexts)
            --state->numActiveContexts;
        ReleaseContextRanges (state, context);
        FreeContextStorage (context);
        TrimContextSlots (state);
    }

    for (u32 i = firstObject; i < state->numObjects; ++i)
    {
        M3DylinkObject * object = & state->objects [i];
        ReleaseObjectRanges (state, object);
        if (object->module and object->module->runtime == state->runtime)
            UnlinkRuntimeModule (state->runtime, object->module);
        if (object->module and object->module != programModule)
            m3_FreeModule (object->module);
        m3_Free (object->name);
        M3_INIT (*object);
    }
    state->numObjects = firstObject;
    TrimContextSlots (state);
}


M3Result  m3_DylinkAddProgram  (IM3Runtime runtime,
                                 const M3DylinkProgram * program,
                                 const M3DylinkOptions * options,
                                 IM3DylinkContext * outContext)
{
    if (outContext)
        *outContext = NULL;
    if (not runtime or not program or not program->module or not outContext)
        return m3Err_dylinkMissingSection;
    if (not program->module->dylink)
        return m3Err_dylinkMissingSection;
    if (program->module->runtime)
        return m3Err_moduleAlreadyLinked;

    M3DylinkState * state = (M3DylinkState *) runtime->dylinkState;
    if (not state or state->runtime != runtime or not state->dynamicPrograms)
        return m3Err_dylinkUnsupported;
    const char * name = program->name ? program->name : "main";
    if (ProgramNameExists (state, name))
        return m3Err_dylinkDuplicateSymbol;

    u32 firstObject = state->numObjects;
    u32 slot = 0;
    u32 programObjectIndex = 0;
    u32 * order = NULL;
    u32 orderCount = 0;
    u32 firstExport = 0;
    u32 exportCount = 0;
    bool exportRangeAllocated = false;
    u32 minimumTable = state->tableSize;
    u32 maximumTable = state->maximumTable;
    u32 maximumPages = runtime->memory.maxPages;
    IM3DylinkContext context = NULL;
    IM3DylinkContext previousActive = state->activeContext;
    M3Result result = ReserveContextSlot (state, & slot);
    if (result)
        return result;

    result = AddObject (state, name, program->module, slot,
                        & programObjectIndex);
    if (result)
        goto _catch;
    result = CollectDependencies (state, programObjectIndex, options);
    if (result)
        goto _catch;

    order = m3_AllocArray (u32, state->numObjects - firstObject);
    if (state->numObjects != firstObject and not order)
    {
        result = m3Err_mallocFailed;
        goto _catch;
    }
    for (u32 i = firstObject; i < state->numObjects; ++i)
        state->objects [i].visit = 0;
    TopologicalVisitNew (state, programObjectIndex, firstObject, order,
                         & orderCount);

    for (u32 i = 0; i < orderCount; ++i)
    {
        M3DylinkObject * object = & state->objects [order [i]];
        M3DylinkMetadata * metadata =
            (M3DylinkMetadata *) object->module->dylink;
        result = ValidateObject (object,
                                 object->programIndex != UINT32_MAX);
        if (result)
            goto _catch;
        if (not AllocateRange (& state->freeMemoryRanges,
                               & state->memoryCursor,
                               metadata->memoryAlignment,
                               metadata->memorySize,
                               & object->memoryBase))
        {
            result = m3Err_dylinkUnsupported;
            goto _catch;
        }
        object->memorySize = metadata->memorySize;
        if (not AllocateRange (& state->freeTableRanges,
                               & state->tableCursor,
                               metadata->tableAlignment,
                               metadata->tableSize,
                               & object->tableBase))
        {
            result = m3Err_dylinkUnsupported;
            goto _catch;
        }
        object->tableSize = metadata->tableSize;

        u32 pageSize = object->module->memoryInfo.pageSize
                     ? object->module->memoryInfo.pageSize
                     : d_m3DefaultMemPageSize;
        if (pageSize != runtime->memory.pageSize)
        {
            result = m3Err_dylinkUnsupported;
            goto _catch;
        }
        u32 moduleMaxPages = object->module->memoryInfo.maxPages;
        if (moduleMaxPages and moduleMaxPages < maximumPages)
            maximumPages = moduleMaxPages;
        minimumTable = M3_MAX (minimumTable,
                               object->module->table0InitSize);
        u32 moduleMaxTable = object->module->table0MaxSize;
        if (moduleMaxTable
            and (not maximumTable or moduleMaxTable < maximumTable))
            maximumTable = moduleMaxTable;
    }

    result = CreateAddedContext (state, program, options, slot, & context);
    if (result)
        goto _catch;

    exportCount = CountFunctionExportsFrom (state, firstObject);
    if (not AllocateRange (& state->freeTableRanges, & state->tableCursor,
                           0, exportCount, & firstExport))
    {
        result = m3Err_dylinkUnsupported;
        goto _catch;
    }
    exportRangeAllocated = exportCount != 0;

    u32 requiredTable = M3_MAX (state->tableCursor, minimumTable);
    if (maximumTable and requiredTable > maximumTable)
    {
        result = m3Err_dylinkUnsupported;
        goto _catch;
    }
    if (requiredTable > state->tableSize)
    {
        IM3Function * table = m3_ReallocArray (IM3Function, state->table,
                                                requiredTable,
                                                state->tableSize);
        if (not table)
        {
            result = m3Err_mallocFailed;
            goto _catch;
        }
        state->table = table;
        state->tableSize = requiredTable;
    }

    u64 requiredBytes = state->memoryCursor;
    u64 pages64 = (requiredBytes + runtime->memory.pageSize - 1u)
                / runtime->memory.pageSize;
    if (pages64 > UINT32_MAX or pages64 > maximumPages)
    {
        result = m3Err_wasmMemoryOverflow;
        goto _catch;
    }
    if ((u32) pages64 > runtime->memory.numPages)
    {
        result = ResizeMemory (runtime, (u32) pages64);
        if (result)
            goto _catch;
    }

    result = LinkFunctionsFrom (state, firstObject);
    if (result)
        goto _catch;
    result = LinkGlobalsAndBasesFrom (state, firstObject);
    if (result)
        goto _catch;
    result = FillExportTableFrom (state, firstObject, firstExport);
    if (result)
        goto _catch;

    if (options and options->linkHostImports)
    {
        for (u32 i = 0; i < orderCount; ++i)
        {
            IM3Module module = state->objects [order [i]].module;
            module->runtime = runtime;
            result = options->linkHostImports (options->context, module);
            module->runtime = NULL;
            if (result)
                goto _catch;
        }
    }
    result = ValidateImportsFrom (state, firstObject);
    if (result)
        goto _catch;

    for (u32 i = firstObject; i < state->numObjects; ++i)
        m3_SetModuleName (state->objects [i].module,
                          state->objects [i].name);
    for (u32 i = 0; i < orderCount; ++i)
    {
        result = m3_LoadModule (runtime, state->objects [order [i]].module);
        if (result)
            goto _catch;
    }
    result = ResolveGOTFrom (state, firstObject);
    if (result)
        goto _catch;
    result = ActivateContext (context);
    if (result)
        goto _catch;
    result = RunInitializers (state, order, orderCount, slot);
    if (result)
        goto _catch;

    runtime->memory.maxPages = maximumPages;
    state->maximumTable = maximumTable;

    if (exportRangeAllocated)
    {
        u32 actualExports = 0;
        for (u32 i = firstObject; i < state->numObjects; ++i)
            actualExports += state->objects [i].exportTableSize;
        if (actualExports < exportCount)
            FreeRange (& state->freeTableRanges,
                       firstExport + actualExports,
                       exportCount - actualExports);
    }
    *outContext = context;
    m3_Free (order);
    return m3Err_none;

_catch:
    if (exportRangeAllocated)
    {
        u32 assignedExports = 0;
        for (u32 i = firstObject; i < state->numObjects; ++i)
            assignedExports += state->objects [i].exportTableSize;
        if (assignedExports < exportCount)
            FreeRange (& state->freeTableRanges,
                       firstExport + assignedExports,
                       exportCount - assignedExports);
    }
    RollBackAddedProgram (state, firstObject, program->module, context,
                          previousActive);
    m3_Free (order);
    return result;
}


M3Result  m3_DylinkRemoveProgram  (IM3DylinkContext context)
{
    if (not context or not context->state)
        return m3Err_dylinkUnsupported;
    M3DylinkState * state = context->state;
    if (not state->dynamicPrograms or not state->runtime
        or state->runtime->dylinkState != state)
        return m3Err_dylinkUnsupported;
    u32 slot = context->programIndex;
    if (slot >= state->numContexts or state->contexts [slot] != context)
        return m3Err_dylinkUnsupported;
    i32 objectIndex = FindProgramObjectIndex (state, context->module);
    if (objectIndex < 0)
        return m3Err_dylinkUnsupported;

    if (state->activeContext == context)
    {
        IM3DylinkContext replacement = NULL;
        for (u32 i = 0; i < state->numContexts; ++i)
            if (state->contexts [i] and state->contexts [i] != context)
            {
                replacement = state->contexts [i];
                break;
            }
        if (replacement)
        {
            M3Result result = ActivateContext (replacement);
            if (result)
                return result;
        }
        else
            ParkRuntime (state);
    }

    state->contexts [slot] = NULL;
    if (state->numActiveContexts)
        --state->numActiveContexts;
    ReleaseContextRanges (state, context);

    M3DylinkObject object = state->objects [(u32) objectIndex];
    ReleaseObjectRanges (state, & object);
    UnlinkRuntimeModule (state->runtime, object.module);
    m3_Free (object.name);
    for (u32 i = (u32) objectIndex + 1; i < state->numObjects; ++i)
        state->objects [i - 1] = state->objects [i];
    --state->numObjects;
    M3_INIT (state->objects [state->numObjects]);

    context->state = NULL;
    FreeContextStorage (context);
    m3_FreeModule (object.module);
    TrimContextSlots (state);
    return m3Err_none;
}


uint32_t  m3_DylinkGetProgramCount  (IM3Runtime runtime)
{
    M3DylinkState * state = runtime
        ? (M3DylinkState *) runtime->dylinkState : NULL;
    return state ? state->numActiveContexts : 0;
}


M3Result  m3_DylinkLoad  (IM3Runtime runtime, IM3Module mainModule,
                           const char * mainName,
                           const M3DylinkOptions * options)
{
    M3DylinkProgram program;
    M3_INIT (program);
    program.name = mainName ? mainName : "main";
    program.module = mainModule;
    program.nativeStackSize = runtime ? runtime->stackSize : 0;
    program.userdata = runtime ? runtime->userdata : NULL;
    return LoadGroup (runtime, & program, 1, options, NULL, false);
}

#endif // d_m3HasDylink
