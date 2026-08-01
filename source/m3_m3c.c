//
//  m3_m3c.c
//
//  Persistent, relocatable wasm3 metacode cache. This translation unit is
//  empty when d_m3HasM3C is disabled.
//

#include "m3_config.h"

#if d_m3HasM3C

#include <limits.h>
#include <stdint.h>

#include "m3_m3c.h"
#include "m3_m3c_internal.h"
#include "m3_compile.h"
#include "m3_env.h"
#include "m3_exception.h"

const M3Result m3Err_m3cInvalid = "invalid .m3c image";
const M3Result m3Err_m3cIncompatible = "incompatible .m3c image";
const M3Result m3Err_m3cStorage = ".m3c storage operation failed";
const M3Result m3Err_m3cUnsupportedRelocation =
    "unsupported .m3c relocation";

enum
{
    c_m3cMagic              = 0x0043334du, // "M3C\0", little-endian
    c_m3cFormatVersion      = 1,
    c_m3cHeaderSize         = 64,
    c_m3cFunctionDescSize   = 64,
    c_m3cRelocSize          = 12,

    c_m3cFunctionCached     = 1,

    c_m3cRelocOperation     = 1,
    c_m3cRelocCode          = 2,
    c_m3cRelocFunction      = 3,
    c_m3cRelocGlobal        = 4,
    c_m3cRelocModule        = 5,
    c_m3cRelocFuncType      = 6,
    c_m3cRelocNull          = 7,

    c_m3cRecordOperation    = 1,
    c_m3cRecordPointer      = 2
};

typedef struct M3CFunctionDesc
{
    u32 flags;
    u32 wordCount;
    u32 relocCount;
    u32 entryWord;
    u32 maxStackSlots;
    u32 numRetSlots;
    u32 numRetAndArgSlots;
    u32 numLocals;
    u32 numLocalBytes;
    u32 constantsSize;
    u32 contentHash;
    u64 codeOffset;
    u64 relocOffset;
    u64 constantsOffset;
}
M3CFunctionDesc;

typedef struct M3CModuleState
{
    M3CStorage         storage;
    u64                baseOffset;
    u64                imageSize;
    u8 *               wasm;
    M3CFunctionDesc *  functions;
    u32                numFunctions;
}
M3CModuleState;

typedef struct M3CRecord
{
    pc_t   location;
    u8     kind;
}
M3CRecord;

typedef struct M3CRecorder
{
    M3CRecord * records;
    u32         count;
    u32         capacity;
    M3Result    result;
}
M3CRecorder;

typedef struct M3CPageRef
{
    IM3CodePage page;
    u32         firstWord;
}
M3CPageRef;

typedef struct M3CPreparedReloc
{
    pc_t    location;
    void *  original;
    u32     wordOffset;
    u32     target;
    u8      kind;
}
M3CPreparedReloc;


static void  WriteU32  (u8 * o, u32 v)
{
    o [0] = (u8) v;
    o [1] = (u8) (v >> 8);
    o [2] = (u8) (v >> 16);
    o [3] = (u8) (v >> 24);
}


static void  WriteU16  (u8 * o, u16 v)
{
    o [0] = (u8) v;
    o [1] = (u8) (v >> 8);
}


static void  WriteU64  (u8 * o, u64 v)
{
    WriteU32 (o, (u32) v);
    WriteU32 (o + 4, (u32) (v >> 32));
}


static u32  ReadU32  (const u8 * i)
{
    return (u32) i [0]
         | ((u32) i [1] << 8)
         | ((u32) i [2] << 16)
         | ((u32) i [3] << 24);
}


static u16  ReadU16  (const u8 * i)
{
    return (u16) ((u16) i [0] | ((u16) i [1] << 8));
}


static u64  ReadU64  (const u8 * i)
{
    return (u64) ReadU32 (i) | ((u64) ReadU32 (i + 4) << 32);
}


static bool  AddU64  (u64 a, u64 b, u64 * o)
{
    if (UINT64_MAX - a < b)
        return false;
    *o = a + b;
    return true;
}


static bool  MulU64  (u64 a, u64 b, u64 * o)
{
    if (a and b > UINT64_MAX / a)
        return false;
    *o = a * b;
    return true;
}


static bool  RangeValid  (u64 offset, u64 size, u64 total)
{
    return offset <= total and size <= total - offset;
}


static u64  Align8  (u64 value)
{
    return (value + 7u) & ~(u64) 7u;
}


static M3Result  StorageRead  (const M3CStorage * storage, u64 base,
                               u64 offset, void * data, u32 size)
{
    u64 absolute;
    if (not storage or not storage->readAt or not AddU64 (base, offset, & absolute))
        return m3Err_m3cStorage;
    if (size == 0)
        return m3Err_none;
    M3Result result = storage->readAt (storage->context, absolute, data, size);
    return result ? result : m3Err_none;
}


static M3Result  StorageWrite  (const M3CStorage * storage, u64 base,
                                u64 offset, const void * data, u32 size)
{
    u64 absolute;
    if (not storage or not storage->writeAt or not AddU64 (base, offset, & absolute))
        return m3Err_m3cStorage;
    if (size == 0)
        return m3Err_none;
    M3Result result = storage->writeAt (storage->context, absolute, data, size);
    return result ? result : m3Err_none;
}


static u32  HashUpdate  (u32 hash, const u8 * bytes, u32 size)
{
    for (u32 i = 0; i < size; ++i)
    {
        hash ^= bytes [i];
        hash *= 16777619u;
    }
    return hash;
}


static u32  HashBytes  (const u8 * bytes, u32 size)
{
    return HashUpdate (2166136261u, bytes, size);
}


static u32  ConfigFlags  (void)
{
    const u16 endian = 1;
    u32 flags = 0;
    flags |= (sizeof (void *) == 8) ? (1u << 0) : 0;
    flags |= (*(const u8 *) & endian == 0) ? (1u << 1) : 0;
    flags |= d_m3HasFloat              ? (1u << 2) : 0;
    flags |= d_m3Use32BitSlots         ? (1u << 3) : 0;
    flags |= d_m3CascadedOpcodes       ? (1u << 4) : 0;
    flags |= d_m3EnableOpTracing       ? (1u << 5) : 0;
    flags |= d_m3EnableOpProfiling     ? (1u << 6) : 0;
    flags |= d_m3RecordBacktraces      ? (1u << 7) : 0;
    flags |= d_m3SkipStackCheck        ? (1u << 8) : 0;
    flags |= d_m3SkipMemoryBoundsCheck ? (1u << 9) : 0;
    flags |= ((u32) d_m3EnableStrace & 3u) << 10;
#ifdef DEBUG
    flags |= 1u << 12;
#endif
#ifdef d_m3NoFloatDynamic
    flags |= d_m3NoFloatDynamic ? (1u << 13) : 0;
#endif
    return flags;
}


static void  EncodeFunctionDesc  (u8 * bytes, const M3CFunctionDesc * desc)
{
    memset (bytes, 0, c_m3cFunctionDescSize);
    WriteU32 (bytes + 0, desc->flags);
    WriteU32 (bytes + 4, desc->wordCount);
    WriteU32 (bytes + 8, desc->relocCount);
    WriteU32 (bytes + 12, desc->entryWord);
    WriteU16 (bytes + 16, (u16) desc->maxStackSlots);
    WriteU16 (bytes + 18, (u16) desc->numRetSlots);
    WriteU16 (bytes + 20, (u16) desc->numRetAndArgSlots);
    WriteU16 (bytes + 22, (u16) desc->numLocals);
    WriteU16 (bytes + 24, (u16) desc->numLocalBytes);
    WriteU16 (bytes + 26, (u16) desc->constantsSize);
    WriteU32 (bytes + 28, desc->contentHash);
    WriteU64 (bytes + 32, desc->codeOffset);
    WriteU64 (bytes + 40, desc->relocOffset);
    WriteU64 (bytes + 48, desc->constantsOffset);
}


static void  DecodeFunctionDesc  (M3CFunctionDesc * desc, const u8 * bytes)
{
    memset (desc, 0, sizeof (* desc));
    desc->flags             = ReadU32 (bytes + 0);
    desc->wordCount         = ReadU32 (bytes + 4);
    desc->relocCount        = ReadU32 (bytes + 8);
    desc->entryWord         = ReadU32 (bytes + 12);
    desc->maxStackSlots     = ReadU16 (bytes + 16);
    desc->numRetSlots       = ReadU16 (bytes + 18);
    desc->numRetAndArgSlots = ReadU16 (bytes + 20);
    desc->numLocals         = ReadU16 (bytes + 22);
    desc->numLocalBytes     = ReadU16 (bytes + 24);
    desc->constantsSize     = ReadU16 (bytes + 26);
    desc->contentHash       = ReadU32 (bytes + 28);
    desc->codeOffset        = ReadU64 (bytes + 32);
    desc->relocOffset       = ReadU64 (bytes + 40);
    desc->constantsOffset   = ReadU64 (bytes + 48);
}


static M3Result  AppendRecord  (M3CRecorder * recorder, pc_t location, u8 kind)
{
    if (not recorder or recorder->result)
        return recorder ? recorder->result : m3Err_none;

    if (recorder->count == recorder->capacity)
    {
        u32 oldCapacity = recorder->capacity;
        u32 newCapacity = oldCapacity ? oldCapacity * 2u : 64u;
        if (newCapacity < oldCapacity)
            return recorder->result = m3Err_mallocFailed;

        M3CRecord * records = m3_ReallocArray (M3CRecord, recorder->records,
                                                newCapacity, oldCapacity);
        if (not records)
            return recorder->result = m3Err_mallocFailed;
        recorder->records = records;
        recorder->capacity = newCapacity;
    }

    recorder->records [recorder->count++] = (M3CRecord) {
        .location = location,
        .kind = kind
    };
    return m3Err_none;
}


void  m3c_RecordOperation  (IM3Runtime i_runtime, pc_t i_location,
                            IM3Operation i_operation)
{
    (void) i_operation;
    if (i_runtime and i_runtime->m3cCompileContext)
        AppendRecord ((M3CRecorder *) i_runtime->m3cCompileContext,
                      i_location, c_m3cRecordOperation);
}


void  m3c_RecordPointer  (IM3Runtime i_runtime, pc_t i_location)
{
    if (i_runtime and i_runtime->m3cCompileContext)
        AppendRecord ((M3CRecorder *) i_runtime->m3cCompileContext,
                      i_location, c_m3cRecordPointer);
}


static u32  AddPageList  (M3CPageRef * refs, u32 count, IM3CodePage page)
{
    while (page)
    {
        refs [count++].page = page;
        page = page->info.next;
    }
    return count;
}


static void  SortPages  (M3CPageRef * refs, u32 count)
{
    for (u32 i = 1; i < count; ++i)
    {
        M3CPageRef value = refs [i];
        u32 j = i;
        while (j and refs [j - 1].page->info.sequence > value.page->info.sequence)
        {
            refs [j] = refs [j - 1];
            --j;
        }
        refs [j] = value;
    }
}


static bool  FindCodeWord  (const M3CPageRef * refs, u32 count,
                            const void * pointer, bool allowEnd, u32 * o_word)
{
    uintptr_t target = (uintptr_t) pointer;
    for (u32 i = 0; i < count; ++i)
    {
        uintptr_t start = (uintptr_t) GetPageStartPC (refs [i].page);
        u32 words = refs [i].page->info.lineIndex;
        uintptr_t end = start + (uintptr_t) words * sizeof (code_t);
        if (target >= start and (target < end or (allowEnd and target == end)))
        {
            uintptr_t delta = target - start;
            if (delta % sizeof (code_t))
                return false;
            u64 word = (u64) refs [i].firstWord + delta / sizeof (code_t);
            if (word > UINT32_MAX)
                return false;
            *o_word = (u32) word;
            return true;
        }
    }
    return false;
}


static M3Result  ClassifyPointer  (IM3Module module, M3Function * functions,
                                   const M3CPageRef * pages, u32 pageCount,
                                   const void * pointer, u8 * o_kind, u32 * o_target)
{
    if (not pointer)
    {
        *o_kind = c_m3cRelocNull;
        *o_target = 0;
        return m3Err_none;
    }

    if (FindCodeWord (pages, pageCount, pointer, true, o_target))
    {
        *o_kind = c_m3cRelocCode;
        return m3Err_none;
    }

    uintptr_t address = (uintptr_t) pointer;
    uintptr_t firstFunction = (uintptr_t) functions;
    uintptr_t endFunctions = firstFunction
                           + (uintptr_t) module->numFunctions * sizeof (M3Function);
    if (address >= firstFunction and address < endFunctions)
    {
        uintptr_t delta = address - firstFunction;
        if (delta % sizeof (M3Function) == 0)
        {
            *o_kind = c_m3cRelocFunction;
            *o_target = (u32) (delta / sizeof (M3Function));
            return m3Err_none;
        }
    }

    for (u32 i = 0; i < module->numGlobals; ++i)
    {
        if (pointer == & module->globals [i].i64Value)
        {
            *o_kind = c_m3cRelocGlobal;
            *o_target = i;
            return m3Err_none;
        }
    }

    if (pointer == module)
    {
        *o_kind = c_m3cRelocModule;
        *o_target = 0;
        return m3Err_none;
    }

    for (u32 i = 0; i < module->numFuncTypes; ++i)
    {
        if (pointer == module->funcTypes [i])
        {
            *o_kind = c_m3cRelocFuncType;
            *o_target = i;
            return m3Err_none;
        }
    }

    return m3Err_m3cUnsupportedRelocation;
}


static M3Result  PrepareRelocations  (IM3Module module, M3Function * functions,
                                      M3CRecorder * recorder,
                                      const M3CPageRef * pages, u32 pageCount,
                                      M3CPreparedReloc ** o_relocs)
{
    M3CPreparedReloc * relocs = NULL;
    if (recorder->count)
    {
        relocs = m3_AllocArray (M3CPreparedReloc, recorder->count);
        if (not relocs)
            return m3Err_mallocFailed;
    }

    M3Result result = m3Err_none;
    for (u32 i = 0; i < recorder->count; ++i)
    {
        M3CRecord * record = & recorder->records [i];
        M3CPreparedReloc * reloc = & relocs [i];
        reloc->location = record->location;
        reloc->original = *(void * const *) record->location;

        if (not FindCodeWord (pages, pageCount, record->location, false,
                              & reloc->wordOffset))
        {
            result = m3Err_m3cUnsupportedRelocation;
            break;
        }

        if (record->kind == c_m3cRecordOperation)
        {
            reloc->kind = c_m3cRelocOperation;
            result = m3c_OperationToId ((IM3Operation) reloc->original,
                                        & reloc->target);
        }
        else
        {
            result = ClassifyPointer (module, functions, pages, pageCount,
                                      reloc->original, & reloc->kind,
                                      & reloc->target);
        }

        if (result)
            break;
    }

    if (result)
    {
        m3_Free (relocs);
        return result;
    }

    *o_relocs = relocs;
    return m3Err_none;
}


static M3Result  WriteRelocations  (const M3CStorage * storage, u64 base,
                                    u64 offset, const M3CPreparedReloc * relocs,
                                    u32 count, u32 * io_hash)
{
    if (not count)
        return m3Err_none;

    u64 byteCount64;
    if (not MulU64 (count, c_m3cRelocSize, & byteCount64)
        or byteCount64 > UINT32_MAX)
        return m3Err_m3cInvalid;

    u32 byteCount = (u32) byteCount64;
    u8 * bytes = m3_AllocArray (u8, byteCount);
    if (not bytes)
        return m3Err_mallocFailed;

    for (u32 i = 0; i < count; ++i)
    {
        u8 * out = bytes + i * c_m3cRelocSize;
        WriteU32 (out, relocs [i].wordOffset);
        out [4] = relocs [i].kind;
        out [5] = out [6] = out [7] = 0;
        WriteU32 (out + 8, relocs [i].target);
    }

    if (io_hash)
        *io_hash = HashUpdate (* io_hash, bytes, byteCount);
    M3Result result = StorageWrite (storage, base, offset, bytes, byteCount);
    m3_Free (bytes);
    return result;
}


static void  ResetCompilerPages  (IM3Runtime runtime)
{
    FreeCodePages (& runtime->pagesOpen);
    FreeCodePages (& runtime->pagesFull);
    runtime->numCodePages = 0;
    runtime->numActiveCodePages = 0;
}


static M3Result  CompileAndWriteFunction  (IM3Module module,
                                           M3Function * functions,
                                           IM3Function function,
                                           M3Runtime * compilerRuntime,
                                           M3CRecorder * recorder,
                                           const M3CStorage * storage,
                                           u64 base, u64 * io_cursor,
                                           M3CFunctionDesc * desc)
{
    M3Result result = m3Err_none;
    M3CPageRef * pages = NULL;
    M3CPreparedReloc * relocs = NULL;
    bool wordsCleared = false;

    recorder->count = 0;
    recorder->result = m3Err_none;
    compilerRuntime->m3cCompileContext = recorder;

    result = CompileFunction (function);
    if (result)
        goto _catch;
    if (recorder->result)
    {
        result = recorder->result;
        goto _catch;
    }

    u32 pageCount = CountCodePages (compilerRuntime->pagesOpen)
                  + CountCodePages (compilerRuntime->pagesFull);
    if (not pageCount)
    {
        result = m3Err_missingCompiledCode;
        goto _catch;
    }

    pages = m3_AllocArray (M3CPageRef, pageCount);
    if (not pages)
    {
        result = m3Err_mallocFailed;
        goto _catch;
    }

    u32 added = AddPageList (pages, 0, compilerRuntime->pagesOpen);
    added = AddPageList (pages, added, compilerRuntime->pagesFull);
    if (added != pageCount)
    {
        result = m3Err_m3cInvalid;
        goto _catch;
    }
    SortPages (pages, pageCount);

    u64 wordCount64 = 0;
    for (u32 i = 0; i < pageCount; ++i)
    {
        if (wordCount64 > UINT32_MAX)
        {
            result = m3Err_m3cInvalid;
            goto _catch;
        }
        pages [i].firstWord = (u32) wordCount64;
        wordCount64 += pages [i].page->info.lineIndex;
    }
    if (wordCount64 == 0 or wordCount64 > UINT32_MAX)
    {
        result = m3Err_m3cInvalid;
        goto _catch;
    }

    desc->flags = c_m3cFunctionCached;
    desc->wordCount = (u32) wordCount64;
    desc->relocCount = recorder->count;
    if (not FindCodeWord (pages, pageCount, function->compiled, false,
                          & desc->entryWord))
    {
        result = m3Err_m3cUnsupportedRelocation;
        goto _catch;
    }

    result = PrepareRelocations (module, functions, recorder, pages, pageCount,
                                 & relocs);
    if (result)
        goto _catch;

    desc->codeOffset = * io_cursor = Align8 (* io_cursor);
    u32 contentHash = 2166136261u;
    for (u32 i = 0; i < recorder->count; ++i)
        *(code_t *) relocs [i].location = NULL;
    wordsCleared = true;

    u64 codeCursor = desc->codeOffset;
    for (u32 i = 0; i < pageCount; ++i)
    {
        u64 bytes64;
        if (not MulU64 (pages [i].page->info.lineIndex, sizeof (code_t), & bytes64)
            or bytes64 > UINT32_MAX)
        {
            result = m3Err_m3cInvalid;
            goto _catch;
        }
        result = StorageWrite (storage, base, codeCursor,
                               GetPageStartPC (pages [i].page), (u32) bytes64);
        if (result)
            goto _catch;
        contentHash = HashUpdate (contentHash,
                                  (const u8 *) GetPageStartPC (pages [i].page),
                                  (u32) bytes64);
        codeCursor += bytes64;
    }

    for (u32 i = 0; i < recorder->count; ++i)
        *(code_t *) relocs [i].location = relocs [i].original;
    wordsCleared = false;
    * io_cursor = codeCursor;

    desc->relocOffset = * io_cursor;
    result = WriteRelocations (storage, base, desc->relocOffset, relocs,
                               recorder->count, & contentHash);
    if (result)
        goto _catch;
    * io_cursor += (u64) recorder->count * c_m3cRelocSize;

    desc->constantsOffset = * io_cursor;
    desc->constantsSize = function->numConstantBytes;
    if (desc->constantsSize)
    {
        result = StorageWrite (storage, base, desc->constantsOffset,
                               function->constants, desc->constantsSize);
        if (result)
            goto _catch;
        contentHash = HashUpdate (contentHash,
                                  (const u8 *) function->constants,
                                  desc->constantsSize);
        * io_cursor += desc->constantsSize;
    }
    desc->contentHash = contentHash;

    desc->maxStackSlots = function->maxStackSlots;
    desc->numRetSlots = function->numRetSlots;
    desc->numRetAndArgSlots = function->numRetAndArgSlots;
    desc->numLocals = function->numLocals;
    desc->numLocalBytes = function->numLocalBytes;

_catch:
    if (wordsCleared)
    {
        for (u32 i = 0; i < recorder->count; ++i)
            *(code_t *) relocs [i].location = relocs [i].original;
    }
    m3_Free (relocs);
    m3_Free (pages);
    m3_Free (function->constants);
    function->compiled = NULL;
    function->numConstantBytes = 0;
    ResetCompilerPages (compilerRuntime);
    return result;
}


M3Result  m3_WriteM3C  (IM3Module i_module, const M3CStorage * i_storage,
                        u64 i_offset, u64 * o_size)
{
    if (o_size)
        *o_size = 0;
    if (not i_module or not i_storage or not i_storage->writeAt)
        return m3Err_m3cStorage;
    if (not i_module->wasmStart or not i_module->wasmEnd
        or i_module->wasmEnd < i_module->wasmStart)
        return m3Err_m3cInvalid;
    if (i_module->m3cState)
        return m3Err_m3cIncompatible;
#if d_m3RecordBacktraces
    // Code mapping pages will be added to the format in the bounded-cache PR.
    return m3Err_m3cIncompatible;
#endif

    u64 wasmSize64 = (u64) (i_module->wasmEnd - i_module->wasmStart);
    if (wasmSize64 == 0 or wasmSize64 > UINT32_MAX)
        return m3Err_m3cInvalid;
    u32 wasmSize = (u32) wasmSize64;

    u64 tableBytes;
    u64 cursor;
    if (not MulU64 (i_module->numFunctions, c_m3cFunctionDescSize, & tableBytes)
        or not AddU64 (c_m3cHeaderSize, tableBytes, & cursor))
        return m3Err_m3cInvalid;
    cursor = Align8 (cursor);

    M3CFunctionDesc * descs = m3_AllocArray (M3CFunctionDesc,
                                             i_module->numFunctions);
    M3Function * shadow = m3_AllocArray (M3Function, i_module->numFunctions);
    if ((i_module->numFunctions and not descs)
        or (i_module->numFunctions and not shadow))
    {
        m3_Free (descs);
        m3_Free (shadow);
        return m3Err_mallocFailed;
    }
    memset (descs, 0, sizeof (M3CFunctionDesc) * i_module->numFunctions);
    memcpy (shadow, i_module->functions,
            sizeof (M3Function) * i_module->numFunctions);

    for (u32 i = 0; i < i_module->numFunctions; ++i)
    {
        shadow [i].module = i_module;
        shadow [i].compiled = NULL;
        shadow [i].constants = NULL;
        shadow [i].numConstantBytes = 0;
        shadow [i].ownsWasmCode = false;
#if d_m3EnableCodePageRefCounting
        shadow [i].codePageRefs = NULL;
        shadow [i].numCodePageRefs = 0;
#endif
    }

    M3Function * originalFunctions = i_module->functions;
    IM3Runtime originalRuntime = i_module->runtime;
    M3Environment compilerEnvironment;
    M3Runtime compilerRuntime;
    M3CRecorder recorder;
    memset (& compilerEnvironment, 0, sizeof (compilerEnvironment));
    memset (& compilerRuntime, 0, sizeof (compilerRuntime));
    memset (& recorder, 0, sizeof (recorder));
    compilerRuntime.environment = & compilerEnvironment;
    compilerRuntime.m3cCompileContext = & recorder;

    i_module->functions = shadow;
    i_module->runtime = & compilerRuntime;

    M3Result result = m3Err_none;
    u64 wasmOffset = cursor;
    result = StorageWrite (i_storage, i_offset, wasmOffset,
                           i_module->wasmStart, wasmSize);
    if (result)
        goto _catch;
    cursor += wasmSize;

    for (u32 i = 0; i < i_module->numFunctions; ++i)
    {
        // Imported functions are linked to host code at runtime and are never
        // persisted as native callback pointers.
        if (not shadow [i].wasm)
            continue;

        result = CompileAndWriteFunction (i_module, shadow, & shadow [i],
                                          & compilerRuntime, & recorder,
                                          i_storage, i_offset, & cursor,
                                          & descs [i]);
        if (result)
            goto _catch;
    }

    u8 descBytes [c_m3cFunctionDescSize];
    for (u32 i = 0; i < i_module->numFunctions; ++i)
    {
        EncodeFunctionDesc (descBytes, & descs [i]);
        result = StorageWrite (i_storage, i_offset,
                               c_m3cHeaderSize
                                   + (u64) i * c_m3cFunctionDescSize,
                               descBytes, sizeof (descBytes));
        if (result)
            goto _catch;
    }

    u8 header [c_m3cHeaderSize];
    memset (header, 0, sizeof (header));
    WriteU32 (header + 0, c_m3cMagic);
    WriteU32 (header + 4, c_m3cFormatVersion);
    WriteU32 (header + 8, c_m3cHeaderSize);
    WriteU32 (header + 12, c_m3cFunctionDescSize);
    WriteU32 (header + 16, d_m3M3CAbiVersion);
    WriteU32 (header + 20, ConfigFlags ());
    WriteU32 (header + 24, wasmSize);
    WriteU32 (header + 28, i_module->numFunctions);
    WriteU32 (header + 32, HashBytes (i_module->wasmStart, wasmSize));
    WriteU64 (header + 40, c_m3cHeaderSize);
    WriteU64 (header + 48, wasmOffset);
    WriteU64 (header + 56, cursor);

    result = StorageWrite (i_storage, i_offset, 0, header, sizeof (header));
    if (not result and i_storage->sync)
        result = i_storage->sync (i_storage->context);
    if (not result and o_size)
        *o_size = cursor;

_catch:
    i_module->functions = originalFunctions;
    i_module->runtime = originalRuntime;
    compilerRuntime.m3cCompileContext = NULL;
    ResetCompilerPages (& compilerRuntime);
    FreeCodePages (& compilerEnvironment.pagesReleased);
    for (u32 i = 0; i < i_module->numFunctions; ++i)
        m3_Free (shadow [i].constants);
    m3_Free (recorder.records);
    m3_Free (shadow);
    m3_Free (descs);
    return result;
}


static bool  FunctionDescValid  (const M3CFunctionDesc * desc, u64 imageSize)
{
    u64 codeBytes;
    u64 relocBytes;
    if (not MulU64 (desc->wordCount, sizeof (code_t), & codeBytes)
        or not MulU64 (desc->relocCount, c_m3cRelocSize, & relocBytes))
        return false;
    if (desc->flags & ~c_m3cFunctionCached)
        return false;
    if (desc->flags & c_m3cFunctionCached)
    {
        if (not desc->wordCount or desc->entryWord >= desc->wordCount)
            return false;
        if (not RangeValid (desc->codeOffset, codeBytes, imageSize)
            or not RangeValid (desc->relocOffset, relocBytes, imageSize)
            or not RangeValid (desc->constantsOffset, desc->constantsSize,
                               imageSize))
            return false;
        if (desc->maxStackSlots > UINT16_MAX
            or desc->numRetSlots > UINT16_MAX
            or desc->numRetAndArgSlots > UINT16_MAX
            or desc->numLocals > UINT16_MAX
            or desc->numLocalBytes > UINT16_MAX
            or desc->constantsSize > UINT16_MAX)
            return false;
    }
    return true;
}


M3Result  m3_ParseM3C  (IM3Environment i_environment, IM3Module * o_module,
                        const M3CStorage * i_storage, u64 i_offset)
{
    if (o_module)
        *o_module = NULL;
    if (not i_environment or not o_module or not i_storage or not i_storage->readAt)
        return m3Err_m3cStorage;

    u8 header [c_m3cHeaderSize];
    M3Result result = StorageRead (i_storage, i_offset, 0, header,
                                   sizeof (header));
    if (result)
        return result;

    if (ReadU32 (header + 0) != c_m3cMagic
        or ReadU32 (header + 4) != c_m3cFormatVersion
        or ReadU32 (header + 8) != c_m3cHeaderSize
        or ReadU32 (header + 12) != c_m3cFunctionDescSize)
        return m3Err_m3cInvalid;
    if (ReadU32 (header + 16) != d_m3M3CAbiVersion
        or ReadU32 (header + 20) != ConfigFlags ())
        return m3Err_m3cIncompatible;
#if d_m3RecordBacktraces
    return m3Err_m3cIncompatible;
#endif

    u32 wasmSize = ReadU32 (header + 24);
    u32 numFunctions = ReadU32 (header + 28);
    u32 wasmHash = ReadU32 (header + 32);
    u64 tableOffset = ReadU64 (header + 40);
    u64 wasmOffset = ReadU64 (header + 48);
    u64 imageSize = ReadU64 (header + 56);
    u64 tableBytes;
    if (not wasmSize or imageSize < c_m3cHeaderSize
        or not MulU64 (numFunctions, c_m3cFunctionDescSize, & tableBytes)
        or not RangeValid (tableOffset, tableBytes, imageSize)
        or not RangeValid (wasmOffset, wasmSize, imageSize))
        return m3Err_m3cInvalid;

    u8 * wasm = m3_AllocArray (u8, wasmSize);
    if (not wasm)
        return m3Err_mallocFailed;
    result = StorageRead (i_storage, i_offset, wasmOffset, wasm, wasmSize);
    if (result)
    {
        m3_Free (wasm);
        return result;
    }
    if (HashBytes (wasm, wasmSize) != wasmHash)
    {
        m3_Free (wasm);
        return m3Err_m3cInvalid;
    }

    IM3Module module = NULL;
    result = m3_ParseModule (i_environment, & module, wasm, wasmSize);
    if (result)
    {
        m3_Free (wasm);
        return result;
    }
    if (module->numFunctions != numFunctions)
    {
        m3_FreeModule (module);
        m3_Free (wasm);
        return m3Err_m3cInvalid;
    }

    M3CModuleState * state = m3_AllocStruct (M3CModuleState);
    M3CFunctionDesc * descs = m3_AllocArray (M3CFunctionDesc, numFunctions);
    if (not state or (numFunctions and not descs))
    {
        m3_Free (state);
        m3_Free (descs);
        m3_FreeModule (module);
        m3_Free (wasm);
        return m3Err_mallocFailed;
    }

    u8 descBytes [c_m3cFunctionDescSize];
    for (u32 i = 0; i < numFunctions; ++i)
    {
        result = StorageRead (i_storage, i_offset,
                              tableOffset + (u64) i * c_m3cFunctionDescSize,
                              descBytes, sizeof (descBytes));
        if (result)
            goto _catch;
        DecodeFunctionDesc (& descs [i], descBytes);
        if (not FunctionDescValid (& descs [i], imageSize))
        {
            result = m3Err_m3cInvalid;
            goto _catch;
        }

        bool defined = module->functions [i].wasm != NULL;
        bool cached = (descs [i].flags & c_m3cFunctionCached) != 0;
        if (defined != cached)
        {
            result = m3Err_m3cInvalid;
            goto _catch;
        }
    }

    memset (state, 0, sizeof (* state));
    state->storage = * i_storage;
    state->baseOffset = i_offset;
    state->imageSize = imageSize;
    state->wasm = wasm;
    state->functions = descs;
    state->numFunctions = numFunctions;
    module->m3cState = state;
    *o_module = module;
    return m3Err_none;

_catch:
    m3_Free (state);
    m3_Free (descs);
    m3_FreeModule (module);
    m3_Free (wasm);
    return result;
}


static bool  GetFunctionIndex  (IM3Function function, u32 * o_index)
{
    if (not function or not function->module or not function->module->functions)
        return false;
    uintptr_t address = (uintptr_t) function;
    uintptr_t first = (uintptr_t) function->module->functions;
    uintptr_t end = first
                  + (uintptr_t) function->module->numFunctions * sizeof (M3Function);
    if (address < first or address >= end)
        return false;
    uintptr_t delta = address - first;
    if (delta % sizeof (M3Function))
        return false;
    *o_index = (u32) (delta / sizeof (M3Function));
    return true;
}


bool  m3c_HasFunction  (IM3Function i_function)
{
    u32 index;
    if (not GetFunctionIndex (i_function, & index))
        return false;
    M3CModuleState * state = (M3CModuleState *) i_function->module->m3cState;
    return state and index < state->numFunctions
        and (state->functions [index].flags & c_m3cFunctionCached);
}


static M3Result  ApplyRelocation  (IM3Function function, code_t * code,
                                   u32 wordCount, const u8 * bytes)
{
    u32 word = ReadU32 (bytes);
    u8 kind = bytes [4];
    u32 target = ReadU32 (bytes + 8);
    IM3Module module = function->module;
    if (word >= wordCount)
        return m3Err_m3cInvalid;

    void * value = NULL;
    switch (kind)
    {
    case c_m3cRelocOperation:
        value = (void *) m3c_OperationFromId (target);
        if (not value)
            return m3Err_m3cIncompatible;
        break;
    case c_m3cRelocCode:
        if (target > wordCount)
            return m3Err_m3cInvalid;
        value = (void *) (code + target);
        break;
    case c_m3cRelocFunction:
        if (target >= module->numFunctions)
            return m3Err_m3cInvalid;
        value = & module->functions [target];
        break;
    case c_m3cRelocGlobal:
        if (target >= module->numGlobals)
            return m3Err_m3cInvalid;
        value = & module->globals [target].i64Value;
        break;
    case c_m3cRelocModule:
        if (target)
            return m3Err_m3cInvalid;
        value = module;
        break;
    case c_m3cRelocFuncType:
        if (target >= module->numFuncTypes)
            return m3Err_m3cInvalid;
        value = module->funcTypes [target];
        break;
    case c_m3cRelocNull:
        if (target)
            return m3Err_m3cInvalid;
        value = NULL;
        break;
    default:
        return m3Err_m3cInvalid;
    }

    code [word] = value;
    return m3Err_none;
}


M3Result  m3c_LoadFunction  (IM3Function i_function)
{
    u32 index;
    if (not GetFunctionIndex (i_function, & index))
        return m3Err_m3cInvalid;
    IM3Module module = i_function->module;
    M3CModuleState * state = (M3CModuleState *) module->m3cState;
    if (not state or index >= state->numFunctions)
        return m3Err_m3cInvalid;
    const M3CFunctionDesc * desc = & state->functions [index];
    if (not (desc->flags & c_m3cFunctionCached))
        return m3Err_missingCompiledCode;
    if (i_function->compiled)
        return m3Err_none;
    if (not module->runtime)
        return m3Err_moduleNotLinked;

    IM3CodePage page = AcquireCodePageWithCapacity (module->runtime,
                                                    desc->wordCount);
    if (not page)
        return m3Err_mallocFailedCodePage;

    u32 initialLine = page->info.lineIndex;
    code_t * code = & page->code [initialLine];
    u64 codeBytes64;
    M3Result result = m3Err_none;
    void * constants = NULL;
    if (not MulU64 (desc->wordCount, sizeof (code_t), & codeBytes64)
        or codeBytes64 > UINT32_MAX)
    {
        result = m3Err_m3cInvalid;
        goto _catch;
    }

    result = StorageRead (& state->storage, state->baseOffset,
                          desc->codeOffset, code, (u32) codeBytes64);
    if (result)
        goto _catch;
    page->info.lineIndex += desc->wordCount;
    u32 contentHash = HashUpdate (2166136261u, (const u8 *) code,
                                  (u32) codeBytes64);

    u8 reloc [c_m3cRelocSize];
    for (u32 i = 0; i < desc->relocCount; ++i)
    {
        result = StorageRead (& state->storage, state->baseOffset,
                              desc->relocOffset + (u64) i * c_m3cRelocSize,
                              reloc, sizeof (reloc));
        if (result)
            goto _catch;
        contentHash = HashUpdate (contentHash, reloc, sizeof (reloc));
        result = ApplyRelocation (i_function, code, desc->wordCount, reloc);
        if (result)
            goto _catch;
    }

    if (desc->constantsSize)
    {
        constants = m3_Malloc ("M3C constants", desc->constantsSize);
        if (not constants)
        {
            result = m3Err_mallocFailed;
            goto _catch;
        }
        result = StorageRead (& state->storage, state->baseOffset,
                              desc->constantsOffset, constants,
                              desc->constantsSize);
        if (result)
            goto _catch;
        contentHash = HashUpdate (contentHash, (const u8 *) constants,
                                  desc->constantsSize);
    }

    if (contentHash != desc->contentHash)
    {
        result = m3Err_m3cInvalid;
        goto _catch;
    }

    m3_Free (i_function->constants);
    i_function->constants = constants;
    constants = NULL;
    i_function->numConstantBytes = (u16) desc->constantsSize;
    i_function->maxStackSlots = (u16) desc->maxStackSlots;
    i_function->numRetSlots = (u16) desc->numRetSlots;
    i_function->numRetAndArgSlots = (u16) desc->numRetAndArgSlots;
    i_function->numLocals = (u16) desc->numLocals;
    i_function->numLocalBytes = (u16) desc->numLocalBytes;
    i_function->compiled = code + desc->entryWord;
    ReleaseCodePage (module->runtime, page);
    return m3Err_none;

_catch:
    m3_Free (constants);
    page->info.lineIndex = initialLine;
    ReleaseCodePage (module->runtime, page);
    return result;
}


void  m3c_OnModuleLoaded  (IM3Module i_module)
{
    if (not i_module or not i_module->m3cState)
        return;
    M3CModuleState * state = (M3CModuleState *) i_module->m3cState;
    m3_Free (state->wasm);
    i_module->wasmStart = NULL;
    i_module->wasmEnd = NULL;
    i_module->elementSection = NULL;
    i_module->elementSectionEnd = NULL;
    for (u32 i = 0; i < i_module->numFunctions; ++i)
    {
        if (state->functions [i].flags & c_m3cFunctionCached)
        {
            i_module->functions [i].wasm = NULL;
            i_module->functions [i].wasmEnd = NULL;
        }
    }
    for (u32 i = 0; i < i_module->numGlobals; ++i)
        i_module->globals [i].initExpr = NULL;
    for (u32 i = 0; i < i_module->numDataSegments; ++i)
    {
        i_module->dataSegments [i].initExpr = NULL;
        i_module->dataSegments [i].data = NULL;
    }
}


void  m3c_ReleaseModule  (IM3Module i_module)
{
    if (not i_module or not i_module->m3cState)
        return;
    M3CModuleState * state = (M3CModuleState *) i_module->m3cState;
    m3_Free (state->wasm);
    m3_Free (state->functions);
    m3_Free (state);
    i_module->m3cState = NULL;
}

#endif // d_m3HasM3C
