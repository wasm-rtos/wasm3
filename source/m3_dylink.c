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
    u32 tableBase;
    u8 visit;
}
M3DylinkObject;

typedef struct M3DylinkState
{
    M3DylinkObject * objects;
    u32 numObjects;
    u32 objectCapacity;

    IM3Function * table;
    u32 tableSize;

    M3Global * stackPointer;
    u32 stackLow;
    u32 stackHigh;
    u32 heapBase;
    u32 heapEnd;
}
M3DylinkState;


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


void  m3d_ReleaseRuntime  (IM3Runtime runtime)
{
    if (not runtime or not runtime->dylinkState)
        return;
    M3DylinkState * state = (M3DylinkState *) runtime->dylinkState;
    for (u32 i = 0; i < state->numObjects; ++i)
        m3_Free (state->objects [i].name);
    m3_Free (state->objects);
    m3_Free (state->table);
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


static i32  FindObjectIndex  (M3DylinkState * state, const char * name)
{
    for (u32 i = 0; i < state->numObjects; ++i)
        if (strcmp (state->objects [i].name, name) == 0)
            return (i32) i;
    return -1;
}


static M3Result  AddObject  (M3DylinkState * state, const char * name,
                             IM3Module module, u32 * o_index)
{
    if (not name or not module)
        return m3Err_dylinkDependencyMissing;
    if (not module->dylink)
        return m3Err_dylinkMissingSection;
    if (module->runtime)
        return m3Err_moduleAlreadyLinked;

    i32 existing = FindObjectIndex (state, name);
    if (existing >= 0)
    {
        if (state->objects [existing].module != module)
            return m3Err_dylinkDuplicateSymbol;
        *o_index = (u32) existing;
        return m3Err_none;
    }
    for (u32 i = 0; i < state->numObjects; ++i)
        if (state->objects [i].module == module)
            return m3Err_dylinkDuplicateSymbol;

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
    state->objects [index].name = copy;
    state->objects [index].module = module;
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
        i32 found = FindObjectIndex (state, name);
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
            result = AddObject (state, name, module, & dependency);
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
        i32 dependency = FindObjectIndex (state, metadata->needed [i]);
        if (dependency >= 0)
            TopologicalVisit (state, (u32) dependency, order, io_count);
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


static bool  FunctionIsExported  (IM3Function function, const char * name)
{
    return function->export_name and NamesEqual (function->export_name, name);
}


static M3Result  FindFunctionSymbol  (M3DylinkState * state,
                                      const char * name,
                                      IM3Function * o_function,
                                      M3DylinkObject ** o_owner)
{
    IM3Function found = NULL;
    M3DylinkObject * owner = NULL;
    bool foundWeak = false;
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
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
                                    const char * name,
                                    M3Global ** o_global,
                                    M3DylinkObject ** o_owner)
{
    M3Global * found = NULL;
    M3DylinkObject * owner = NULL;
    bool foundWeak = false;
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
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


static M3Result  LinkFunctions  (M3DylinkState * state)
{
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
        IM3Module module = state->objects [oi].module;
        for (u32 fi = 0; fi < module->numFuncImports; ++fi)
        {
            IM3Function import = & module->functions [fi];
            IM3Function target = NULL;
            M3Result result = FindFunctionSymbol (state,
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


static M3Result  LinkGlobalsAndBases  (M3DylinkState * state)
{
    M3Global * stackPointer = NULL;
    for (u32 oi = 0; oi < state->numObjects and not stackPointer; ++oi)
    {
        IM3Module module = state->objects [oi].module;
        for (u32 gi = 0; gi < module->numGlobals; ++gi)
        {
            M3Global * global = & module->globals [gi];
            if (global->imported
                and NamesEqual (global->import.moduleUtf8, "env")
                and NamesEqual (global->import.fieldUtf8, "__stack_pointer"))
            {
                if (global->type != c_m3Type_i32 or not global->isMutable)
                    return m3Err_dylinkTypeMismatch;
                stackPointer = global;
                break;
            }
        }
    }
    if (not stackPointer)
        return m3Err_dylinkUnresolvedSymbol;
    stackPointer->i32Value = (i32) state->stackHigh;
    state->stackPointer = stackPointer;

    for (u32 oi = 0; oi < state->numObjects; ++oi)
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
                if (not global->isMutable)
                    return m3Err_dylinkTypeMismatch;
                if (global != stackPointer)
                    global->linkedGlobal = stackPointer;
            }
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__stack_low"))
                global->i32Value = (i32) state->stackLow;
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__stack_high"))
                global->i32Value = (i32) state->stackHigh;
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__heap_base"))
                global->i32Value = (i32) state->heapBase;
            else if (NamesEqual (importModule, "env")
                     and NamesEqual (name, "__heap_end"))
                global->i32Value = (i32) state->heapEnd;
            else if (NamesEqual (importModule, "GOT.func")
                     or NamesEqual (importModule, "GOT.mem"))
                continue;
            else
            {
                M3Global * target = NULL;
                M3Result result = FindGlobalSymbol (state, name, & target, NULL);
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
    return m3Err_none;
}


static u32  CountFunctionExports  (M3DylinkState * state)
{
    u32 count = 0;
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
        IM3Module module = state->objects [oi].module;
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


static M3Result  FillExportTable  (M3DylinkState * state, u32 firstExport)
{
    u32 cursor = firstExport;
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
        IM3Module module = state->objects [oi].module;
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
    }
    return m3Err_none;
}


static M3Result  ValidateImports  (M3DylinkState * state)
{
    for (u32 oi = 0; oi < state->numObjects; ++oi)
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


static M3Result  ResolveGOT  (M3DylinkState * state)
{
    for (u32 oi = 0; oi < state->numObjects; ++oi)
    {
        IM3Module module = state->objects [oi].module;
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
                M3Result result = FindFunctionSymbol (state, name, & function,
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
                M3Result result = FindGlobalSymbol (state, name, & address,
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
                                   const u32 * order, u32 count)
{
    for (u32 i = 0; i < count; ++i)
    {
        M3Result result = m3_RunStart (state->objects [order [i]].module);
        if (result)
            return result;
    }
    for (u32 i = 0; i < count; ++i)
    {
        bool found;
        M3Result result = CallOptional (state->objects [order [i]].module,
                                        "__wasm_apply_data_relocs", & found);
        if (result)
            return result;
    }
    for (u32 i = 0; i < count; ++i)
    {
        IM3Module module = state->objects [order [i]].module;
        bool found;
        M3Result result = CallOptional (module, "_initialize", & found);
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


M3Result  m3_DylinkLoad  (IM3Runtime runtime, IM3Module mainModule,
                           const char * mainName,
                           const M3DylinkOptions * options)
{
    if (not runtime or not mainModule)
        return m3Err_dylinkMissingSection;
    if (runtime->modules or runtime->dylinkState)
        return m3Err_moduleAlreadyLinked;
    if (not mainModule->dylink)
        return m3Err_dylinkMissingSection;

    M3DylinkState * state = m3_AllocStruct (M3DylinkState);
    if (not state)
        return m3Err_mallocFailed;
    u32 mainIndex = 0;
    M3Result result = AddObject (state, mainName ? mainName : "main",
                                 mainModule, & mainIndex);
    if (result)
        goto _catch;
    result = CollectDependencies (state, mainIndex, options);
    if (result)
        goto _catch;

    u32 * order = m3_AllocArray (u32, state->numObjects);
    if (state->numObjects and not order)
    {
        result = m3Err_mallocFailed;
        goto _catch;
    }
    for (u32 i = 0; i < state->numObjects; ++i)
        state->objects [i].visit = 0;
    u32 orderCount = 0;
    TopologicalVisit (state, mainIndex, order, & orderCount);
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
        result = ValidateObject (object, i == mainIndex);
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

    u32 stackSize = options and options->linearStackSize
                  ? options->linearStackSize : c_m3DylinkDefaultStackSize;
    if (not AllocateAligned (& memoryCursor, 4, stackSize, & state->stackLow)
        or not AllocateAligned (& memoryCursor, 4, 0, & state->stackHigh))
    {
        result = m3Err_dylinkUnsupported;
        goto _catch_order;
    }
    state->heapBase = state->stackHigh;
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

    u32 exportCount = CountFunctionExports (state);
    if (exportCount > UINT32_MAX - tableCursor)
    {
        result = m3Err_dylinkUnsupported;
        goto _catch_order;
    }
    u32 firstExport = tableCursor;
    tableCursor += exportCount;
    state->tableSize = M3_MAX (tableCursor, minimumTable);
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

    result = LinkFunctions (state);
    if (result)
        goto _catch_order;
    result = LinkGlobalsAndBases (state);
    if (result)
        goto _catch_order;
    result = FillExportTable (state, firstExport);
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
    result = ValidateImports (state);
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
    result = ResolveGOT (state);
    if (result)
        goto _catch_loaded;
    result = RunInitializers (state, order, orderCount);

_catch_loaded:
    m3_Free (order);
    return result;

_catch_order:
    m3_Free (order);
_catch:
    for (u32 i = 1; i < state->numObjects; ++i)
        if (state->objects [i].module
            and not state->objects [i].module->runtime)
            m3_FreeModule (state->objects [i].module);
    for (u32 i = 0; i < state->numObjects; ++i)
        m3_Free (state->objects [i].name);
    m3_Free (state->objects);
    m3_Free (state->table);
    m3_Free (state);
    return result;
}

#endif // d_m3HasDylink
