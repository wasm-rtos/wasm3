//
//  m3_direct.c
//
//  A small, resumable interpreter for the original WebAssembly bytecode.
//  It executes original WebAssembly instructions without creating an
//  intermediate instruction stream. The program counter always points into
//  the module's persistent .wasm bytes.
//

#include "m3_config.h"


#include "m3_direct.h"
#include "m3_math_utils.h"

#include <limits.h>
#include <math.h>

typedef struct M3DirectFrame
{
    IM3Function function;
    bytes_t pc;
    bytes_t end;
    u32 localBase;
    u32 stackBase;
    u32 controlBase;
}
M3DirectFrame;

typedef struct M3DirectControl
{
    bytes_t bodyPc;
    bytes_t elsePc;
    bytes_t endPc;
    u32 stackHeight;
    u16 numParams;
    u16 numResults;
    u8 opcode;
}
M3DirectControl;

enum
{
    c_m3DirectFunctionControl = 0xff
};

static u64 * DirectValues (IM3Runtime runtime)
{
    return (u64 *) runtime->stack;
}

static u32 DirectValueCapacity (IM3Runtime runtime)
{
    return runtime->stackSize / (u32) sizeof (u64);
}

static M3Result DirectEnsureFrames (IM3Runtime runtime, u32 needed)
{
    if (needed <= runtime->maxDirectFrames)
        return m3Err_none;

    u32 oldMax = runtime->maxDirectFrames;
    u32 newMax = oldMax ? oldMax : 8;
    while (newMax < needed)
    {
        if (newMax > UINT32_MAX / 2)
            return m3Err_mallocFailed;
        newMax *= 2;
    }

    M3DirectFrame * frames = m3_ReallocArray (M3DirectFrame, runtime->directFrames, newMax, oldMax);
    if (not frames)
        return m3Err_mallocFailed;

    runtime->directFrames = frames;
    runtime->maxDirectFrames = newMax;
    return m3Err_none;
}

static M3Result DirectEnsureControls (IM3Runtime runtime, u32 needed)
{
    if (needed <= runtime->maxDirectControls)
        return m3Err_none;

    u32 oldMax = runtime->maxDirectControls;
    u32 newMax = oldMax ? oldMax : 16;
    while (newMax < needed)
    {
        if (newMax > UINT32_MAX / 2)
            return m3Err_mallocFailed;
        newMax *= 2;
    }

    M3DirectControl * controls = m3_ReallocArray (M3DirectControl, runtime->directControls, newMax, oldMax);
    if (not controls)
        return m3Err_mallocFailed;

    runtime->directControls = controls;
    runtime->maxDirectControls = newMax;
    return m3Err_none;
}

static M3Result DirectPush (IM3Runtime runtime, u64 value)
{
    if (runtime->directValueTop >= DirectValueCapacity (runtime))
        return m3Err_trapStackOverflow;

    DirectValues (runtime)[runtime->directValueTop++] = value;
    return m3Err_none;
}

static M3Result DirectPop (IM3Runtime runtime, u64 * value)
{
    if (runtime->directValueTop == 0)
        return m3Err_functionStackUnderrun;

    *value = DirectValues (runtime)[--runtime->directValueTop];
    return m3Err_none;
}

static M3Result DirectPeek (IM3Runtime runtime, u64 * value)
{
    if (runtime->directValueTop == 0)
        return m3Err_functionStackUnderrun;

    *value = DirectValues (runtime)[runtime->directValueTop - 1];
    return m3Err_none;
}

static f32 DirectAsF32 (u64 bits)
{
    u32 word = (u32) bits;
    f32 value;
    memcpy (&value, &word, sizeof value);
    return value;
}

static f64 DirectAsF64 (u64 bits)
{
    f64 value;
    memcpy (&value, &bits, sizeof value);
    return value;
}

static u64 DirectFromF32 (f32 value)
{
    u32 bits;
    memcpy (&bits, &value, sizeof bits);
    return bits;
}

static u64 DirectFromF64 (f64 value)
{
    u64 bits;
    memcpy (&bits, &value, sizeof bits);
    return bits;
}

static M3Result DirectReadOpcode (m3opcode_t * opcode, bytes_t * bytes, cbytes_t end)
{
    u8 first;
    M3Result result = Read_u8 (& first, bytes, end);
    if (result)
        return result;

    if (first == c_waOp_extended)
    {
        u32 extended;
        result = ReadLEB_u32 (& extended, bytes, end);
        if (result)
            return result;
        if (extended > 0xff)
            return m3Err_unknownOpcode;
        *opcode = (m3opcode_t)((first << 8) | extended);
    }
    else
    {
        *opcode = first;
    }
    return m3Err_none;
}

static M3Result DirectReadBlockType (IM3Module module, IM3FuncType * type, bytes_t * bytes, cbytes_t end)
{
    i64 encoded;
    M3Result result = ReadLebSigned (& encoded, 33, bytes, end);
    if (result)
        return result;

    if (encoded < 0)
    {
        u8 valueType;
        result = NormalizeType (& valueType, (i8) encoded);
        if (result)
            return result;
        *type = module->environment->retFuncTypes[valueType];
    }
    else
    {
        if ((u64) encoded >= module->numFuncTypes)
            return m3Err_wasmMalformed;
        *type = module->funcTypes[encoded];
    }
    return m3Err_none;
}

static M3Result DirectSkipImmediate (IM3Module module, m3opcode_t opcode, bytes_t * bytes, cbytes_t end)
{
    M3Result result = m3Err_none;
    u32 ignored;

    switch (opcode)
    {
    case 0x02: case 0x03: case 0x04:
    {
        IM3FuncType type;
        return DirectReadBlockType (module, & type, bytes, end);
    }

    case 0x0c: case 0x0d:
    case 0x10: case 0x12:
    case 0x20: case 0x21: case 0x22:
    case 0x23: case 0x24:
        return ReadLEB_u32 (& ignored, bytes, end);

    case 0x0e:
    {
        u32 count;
        result = ReadLEB_u32 (& count, bytes, end);
        if (result)
            return result;
        if (count > d_m3MaxSaneTableSize)
            return m3Err_wasmMalformed;
        for (u32 i = 0; i <= count; ++i)
        {
            result = ReadLEB_u32 (& ignored, bytes, end);
            if (result)
                return result;
        }
        return m3Err_none;
    }

    case 0x11: case 0x13:
        result = ReadLEB_u32 (& ignored, bytes, end);
        if (result)
            return result;
        return ReadLEB_u32 (& ignored, bytes, end);

    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x3c: case 0x3d: case 0x3e:
        result = ReadLEB_u32 (& ignored, bytes, end);
        if (result)
            return result;
        return ReadLEB_u32 (& ignored, bytes, end);

    case 0x3f: case 0x40:
        return ReadLEB_u32 (& ignored, bytes, end);

    case 0x41:
    {
        i32 value;
        return ReadLEB_i32 (& value, bytes, end);
    }
    case 0x42:
    {
        i64 value;
        return ReadLEB_i64 (& value, bytes, end);
    }
    case 0x43:
    {
        f32 value;
        return Read_f32 (& value, bytes, end);
    }
    case 0x44:
    {
        f64 value;
        return Read_f64 (& value, bytes, end);
    }

    case c_waOp_memoryCopy:
        result = ReadLEB_u32 (& ignored, bytes, end);
        if (result)
            return result;
        return ReadLEB_u32 (& ignored, bytes, end);

    case c_waOp_memoryFill:
        return ReadLEB_u32 (& ignored, bytes, end);

    default:
        return m3Err_none;
    }
}

static M3Result DirectFindBlockBounds (IM3Module module, bytes_t body, cbytes_t limit,
                                       bytes_t * elsePc, bytes_t * endPc)
{
    u32 depth = 1;
    bytes_t pc = body;
    *elsePc = NULL;
    *endPc = NULL;

    while (pc < limit)
    {
        bytes_t instruction = pc;
        m3opcode_t opcode;
        M3Result result = DirectReadOpcode (& opcode, & pc, limit);
        if (result)
            return result;

        if (opcode == c_waOp_block || opcode == c_waOp_loop || opcode == c_waOp_if)
        {
            result = DirectSkipImmediate (module, opcode, & pc, limit);
            if (result)
                return result;
            ++depth;
        }
        else if (opcode == c_waOp_else)
        {
            if (depth == 1)
            {
                if (*elsePc)
                    return m3Err_wasmMalformed;
                *elsePc = instruction;
            }
        }
        else if (opcode == c_waOp_end)
        {
            if (--depth == 0)
            {
                *endPc = instruction;
                return m3Err_none;
            }
        }
        else
        {
            result = DirectSkipImmediate (module, opcode, & pc, limit);
            if (result)
                return result;
        }
    }

    return m3Err_wasmMalformed;
}

static M3Result DirectParseFunctionBody (IM3Function function, bytes_t * code, u32 * numLocals)
{
    bytes_t pc = function->wasm;
    u32 bodySize;
    M3Result result;

    if (not pc)
        return m3Err_functionImportMissing;

    result = ReadLEB_u32 (& bodySize, & pc, function->wasmEnd);
    if (result)
        return result;
    if (bodySize != (u32)(function->wasmEnd - pc))
        return m3Err_wasmMalformed;

    u32 groups;
    result = ReadLEB_u32 (& groups, & pc, function->wasmEnd);
    if (result)
        return result;

    u32 total = 0;
    for (u32 i = 0; i < groups; ++i)
    {
        u32 count;
        i8 wasmType;
        u8 type;

        result = ReadLEB_u32 (& count, & pc, function->wasmEnd);
        if (result)
            return result;
        result = ReadLEB_i7 (& wasmType, & pc, function->wasmEnd);
        if (result)
            return result;
        result = NormalizeType (& type, wasmType);
        if (result)
            return result;
        if (count > UINT32_MAX - total)
            return m3Err_functionStackOverflow;
        total += count;
    }

    *code = pc;
    *numLocals = total;
    return m3Err_none;
}

static M3Result DirectValidateFunction (IM3Function function)
{
    bytes_t pc;
    u32 numLocals;
    M3Result result = DirectParseFunctionBody (function, & pc, & numLocals);
    if (result)
        return result;
    if (numLocals > UINT16_MAX)
        return m3Err_functionStackOverflow;
    function->numLocals = (u16)numLocals;

    u8 controls[d_m3MaxFunctionStackHeight];
    u32 depth = 1;
    controls[0] = c_m3DirectFunctionControl;

    while (pc < function->wasmEnd)
    {
        m3opcode_t opcode;
        result = DirectReadOpcode (& opcode, & pc, function->wasmEnd);
        if (result)
            return result;

        switch (opcode)
        {
        case 0x00: case 0x01:
        case 0x0f: case 0x1a: case 0x1b:
            break;

        case c_waOp_block:
        case c_waOp_loop:
        case c_waOp_if:
        {
            IM3FuncType type;
            result = DirectReadBlockType (function->module, & type, & pc, function->wasmEnd);
            if (result)
                return result;
            if (depth >= d_m3MaxFunctionStackHeight)
                return m3Err_functionStackOverflow;
            controls[depth++] = (u8)opcode;
            break;
        }

        case c_waOp_else:
            if (depth <= 1 || controls[depth - 1] != c_waOp_if)
                return m3Err_wasmMalformed;
            controls[depth - 1] = (u8)(c_waOp_if | 0x80);
            break;

        case c_waOp_end:
            if (depth == 0)
                return m3Err_wasmMalformed;
            --depth;
            if (depth == 0)
                return pc == function->wasmEnd ? m3Err_none : m3Err_wasmMalformed;
            break;

        case c_waOp_branch:
        case c_waOp_branchIf:
        {
            u32 label;
            result = ReadLEB_u32 (& label, & pc, function->wasmEnd);
            if (result) return result;
            if (label >= depth) return m3Err_wasmMalformed;
            break;
        }

        case c_waOp_branchTable:
        {
            u32 count;
            result = ReadLEB_u32 (& count, & pc, function->wasmEnd);
            if (result) return result;
            if (count > d_m3MaxSaneTableSize) return m3Err_wasmMalformed;
            for (u32 i = 0; i <= count; ++i)
            {
                u32 label;
                result = ReadLEB_u32 (& label, & pc, function->wasmEnd);
                if (result) return result;
                if (label >= depth) return m3Err_wasmMalformed;
            }
            break;
        }

        case c_waOp_call:
        case 0x12:
        {
            u32 index;
            result = ReadLEB_u32 (& index, & pc, function->wasmEnd);
            if (result) return result;
            if (index >= function->module->numFunctions) return m3Err_wasmMalformed;
            break;
        }

        case 0x11:
        case 0x13:
        {
            u32 typeIndex, tableIndex;
            result = ReadLEB_u32 (& typeIndex, & pc, function->wasmEnd);
            if (result) return result;
            result = ReadLEB_u32 (& tableIndex, & pc, function->wasmEnd);
            if (result) return result;
            if (typeIndex >= function->module->numFuncTypes || tableIndex != 0)
                return m3Err_wasmMalformed;
            break;
        }

        case c_waOp_getLocal:
        case c_waOp_setLocal:
        case c_waOp_teeLocal:
        {
            u32 index;
            result = ReadLEB_u32 (& index, & pc, function->wasmEnd);
            if (result) return result;
            if (index >= function->funcType->numArgs + numLocals)
                return m3Err_wasmMalformed;
            break;
        }

        case c_waOp_getGlobal:
        case 0x24:
        {
            u32 index;
            result = ReadLEB_u32 (& index, & pc, function->wasmEnd);
            if (result) return result;
            if (index >= function->module->numGlobals)
                return m3Err_globaIndexOutOfBounds;
            break;
        }

        case 0x28: case 0x29: case 0x2a: case 0x2b:
        case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3a: case 0x3b:
        case 0x3c: case 0x3d: case 0x3e:
        {
            u32 alignment, offset;
            result = ReadLEB_u32 (& alignment, & pc, function->wasmEnd);
            if (result) return result;
            result = ReadLEB_u32 (& offset, & pc, function->wasmEnd);
            if (result) return result;
            break;
        }

        case 0x3f: case 0x40:
        {
            u32 memoryIndex;
            result = ReadLEB_u32 (& memoryIndex, & pc, function->wasmEnd);
            if (result) return result;
            if (memoryIndex != 0) return m3Err_wasmMalformed;
            break;
        }

        case c_waOp_i32_const:
        {
            i32 value;
            result = ReadLEB_i32 (& value, & pc, function->wasmEnd);
            if (result) return result;
            break;
        }
        case c_waOp_i64_const:
        {
            i64 value;
            result = ReadLEB_i64 (& value, & pc, function->wasmEnd);
            if (result) return result;
            break;
        }
#if d_m3HasFloat
        case c_waOp_f32_const:
        {
            f32 value;
            result = Read_f32 (& value, & pc, function->wasmEnd);
            if (result) return result;
            break;
        }
        case c_waOp_f64_const:
        {
            f64 value;
            result = Read_f64 (& value, & pc, function->wasmEnd);
            if (result) return result;
            break;
        }
#endif

        default:
            if (opcode >= 0x45 && opcode <= 0xc4)
            {
#if !d_m3HasFloat
                if ((opcode >= 0x5b && opcode <= 0x66) ||
                    (opcode >= 0x8b && opcode <= 0xa6) ||
                    (opcode >= 0xa8 && opcode <= 0xab) ||
                    (opcode >= 0xae && opcode <= 0xbf))
                    return m3Err_unknownOpcode;
#endif
                break;
            }
            if ((opcode >> 8) == c_waOp_extended)
            {
                u32 sub = opcode & 0xff;
                if (sub <= 7)
                {
#if !d_m3HasFloat
                    return m3Err_unknownOpcode;
#else
                    break;
#endif
                }
                if (opcode == c_waOp_memoryCopy)
                {
                    u32 sourceMemory, targetMemory;
                    result = ReadLEB_u32 (& sourceMemory, & pc, function->wasmEnd);
                    if (result) return result;
                    result = ReadLEB_u32 (& targetMemory, & pc, function->wasmEnd);
                    if (result) return result;
                    if (sourceMemory != 0 || targetMemory != 0) return m3Err_wasmMalformed;
                    break;
                }
                if (opcode == c_waOp_memoryFill)
                {
                    u32 memoryIndex;
                    result = ReadLEB_u32 (& memoryIndex, & pc, function->wasmEnd);
                    if (result) return result;
                    if (memoryIndex != 0) return m3Err_wasmMalformed;
                    break;
                }
            }
            return m3Err_unknownOpcode;
        }
    }

    return m3Err_wasmMalformed;
}

M3Result DirectValidateModule (IM3Module module)
{
    if (not module)
        return m3Err_moduleNotLinked;
    if (module->directValidated)
        return m3Err_none;

    for (u32 i = 0; i < module->numFunctions; ++i)
    {
        IM3Function function = &module->functions[i];
        if (not function->wasm)
            continue;

        M3Result result = DirectValidateFunction (function);
        if (result)
            return result;
    }

    module->directValidated = true;
    return m3Err_none;
}

M3Result DirectValidateFunctionGraph (IM3Function function)
{
    if (not function || not function->module)
        return m3Err_moduleNotLinked;

    IM3Module module = function->module;
    M3Result result = DirectValidateModule (module);
    if (result)
        return result;
    if (function->directLinked)
        return m3Err_none;

    if (not function->wasm)
    {
        if (not function->rawFunction)
            return m3Err_functionImportMissing;
        function->directLinked = true;
        return m3Err_none;
    }

    // Validate imports referenced directly by this function. Callees are
    // validated lazily when execution first reaches them.
    bytes_t pc;
    u32 numLocals;
    result = DirectParseFunctionBody (function, & pc, & numLocals);
    if (result)
        return result;

    while (pc < function->wasmEnd)
    {
        m3opcode_t opcode;
        result = DirectReadOpcode (& opcode, & pc, function->wasmEnd);
        if (result)
            return result;

        if (opcode == c_waOp_call || opcode == 0x12)
        {
            u32 targetIndex;
            result = ReadLEB_u32 (& targetIndex, & pc, function->wasmEnd);
            if (result)
                return result;
            if (targetIndex >= module->numFunctions)
                return m3Err_wasmMalformed;

            IM3Function target = &module->functions[targetIndex];
            if (not target->wasm && not target->rawFunction)
            {
                m3log (module, "missing direct import: %s.%s",
                       target->import.moduleUtf8 ? target->import.moduleUtf8 : "",
                       target->import.fieldUtf8 ? target->import.fieldUtf8 : "");
                return m3Err_functionImportMissing;
            }
        }
        else
        {
            result = DirectSkipImmediate (module, opcode, & pc, function->wasmEnd);
            if (result)
                return result;
        }
    }
    function->directLinked = true;
    return m3Err_none;
}

static M3Result DirectPushControl (IM3Runtime runtime, const M3DirectControl * control)
{
    M3Result result = DirectEnsureControls (runtime, runtime->numDirectControls + 1);
    if (result)
        return result;

    runtime->directControls[runtime->numDirectControls++] = *control;
    return m3Err_none;
}

static M3Result DirectPushFrame (IM3Runtime runtime, IM3Function function)
{
    M3Result result;
    bytes_t code = NULL;
    u32 numLocals = 0;
    u32 numArgs = function->funcType->numArgs;
    u32 numRets = function->funcType->numRets;

    if (runtime->numDirectFrames >= d_m3MaxFunctionStackHeight ||
        runtime->numDirectFrames >= DirectValueCapacity (runtime))
        return m3Err_trapStackOverflow;
    if (runtime->directValueTop < numArgs)
        return m3Err_functionStackUnderrun;

    result = DirectParseFunctionBody (function, & code, & numLocals);
    if (result)
        return result;

    if (numLocals > DirectValueCapacity (runtime) - runtime->directValueTop)
        return m3Err_trapStackOverflow;

    result = DirectEnsureFrames (runtime, runtime->numDirectFrames + 1);
    if (result)
        return result;
    result = DirectEnsureControls (runtime, runtime->numDirectControls + 1);
    if (result)
        return result;

    u32 localBase = runtime->directValueTop - numArgs;
    memset (DirectValues (runtime) + runtime->directValueTop, 0, numLocals * sizeof (u64));
    runtime->directValueTop += numLocals;

    M3DirectFrame * frame = &runtime->directFrames[runtime->numDirectFrames++];
    frame->function = function;
    frame->pc = code;
    frame->end = function->wasmEnd;
    frame->localBase = localBase;
    frame->stackBase = runtime->directValueTop;
    frame->controlBase = runtime->numDirectControls;

    M3DirectControl control;
    M3_INIT (control);
    control.bodyPc = code;
    control.endPc = function->wasmEnd - 1;
    control.stackHeight = frame->stackBase;
    control.numResults = (u16) numRets;
    control.opcode = c_m3DirectFunctionControl;
    runtime->directControls[runtime->numDirectControls++] = control;

    function->numLocals = (u16) M3_MIN (numLocals, UINT16_MAX);
    return m3Err_none;
}

static M3Result DirectReturnFrame (IM3Runtime runtime)
{
    if (runtime->numDirectFrames == 0)
        return m3Err_wasmMalformed;

    M3DirectFrame * frame = &runtime->directFrames[runtime->numDirectFrames - 1];
    u32 numRets = frame->function->funcType->numRets;
    if (runtime->directValueTop < numRets)
        return m3Err_functionStackUnderrun;

    u32 source = runtime->directValueTop - numRets;
    memmove (DirectValues (runtime) + frame->localBase,
             DirectValues (runtime) + source,
             numRets * sizeof (u64));
    runtime->directValueTop = frame->localBase + numRets;
    runtime->numDirectControls = frame->controlBase;
    --runtime->numDirectFrames;

    if (runtime->numDirectFrames == 0)
    {
        runtime->directActive = false;
        runtime->suspended = false;
        runtime->lastCalled = runtime->directEntry;
    }
    return m3Err_none;
}

void DirectReset (IM3Runtime runtime)
{
    if (not runtime)
        return;

    runtime->numDirectFrames = 0;
    runtime->numDirectControls = 0;
    runtime->directValueTop = 0;
    runtime->directActive = false;
    runtime->directEntry = NULL;
    runtime->suspended = false;
    runtime->suspendedFunction = NULL;
}

void DirectRelease (IM3Runtime runtime)
{
    if (not runtime)
        return;

    m3_Free (runtime->directFrames);
    m3_Free (runtime->directControls);
    runtime->directFrames = NULL;
    runtime->directControls = NULL;
    runtime->maxDirectFrames = 0;
    runtime->maxDirectControls = 0;
    DirectReset (runtime);
}

uint64_t m3_GetRuntimeMemoryUsage (IM3Runtime runtime)
{
    if (not runtime)
        return 0;

    uint64_t size = sizeof (M3Runtime);
    size += (uint64_t) runtime->stackSize + 4u * sizeof (m3slot_t);
    size += (uint64_t) runtime->maxDirectFrames * sizeof (M3DirectFrame);
    size += (uint64_t) runtime->maxDirectControls * sizeof (M3DirectControl);

    if (runtime->memory.mallocated)
        size += sizeof (M3MemoryHeader) + runtime->memory.mallocated->length;

    return size;
}

M3Result DirectStart (IM3Function function)
{
    if (not function || not function->module || not function->module->runtime)
        return m3Err_moduleNotLinked;
    if (not function->wasm)
        return m3Err_functionImportMissing;

    IM3Runtime runtime = function->module->runtime;
    if (runtime->directActive || runtime->suspended)
        return m3Err_runtimeSuspended;

    M3Result result = DirectValidateFunctionGraph (function);
    if (result)
        return result;

    IM3Module module = function->module;
    i32 startIndex = module->startFunction;
    IM3Function startFunction = NULL;
    if (startIndex >= 0)
    {
        if ((u32)startIndex >= module->numFunctions)
            return m3Err_wasmMalformed;
        startFunction = &module->functions[startIndex];
        if (startFunction->funcType->numArgs != 0 ||
            startFunction->funcType->numRets != 0)
            return m3Err_argumentCountMismatch;
        result = DirectValidateFunctionGraph (startFunction);
        if (result)
            return result;
    }

    DirectReset (runtime);

    u32 numArgs = function->funcType->numArgs;
    u32 numRets = function->funcType->numRets;
    if (numArgs > DirectValueCapacity (runtime) ||
        numRets > DirectValueCapacity (runtime) - numArgs)
        return m3Err_trapStackOverflow;

    memmove (DirectValues (runtime),
             DirectValues (runtime) + numRets,
             numArgs * sizeof (u64));
    runtime->directValueTop = numArgs;
    runtime->directEntry = function;
    runtime->directActive = true;
    runtime->lastCalled = NULL;

    result = DirectPushFrame (runtime, function);
    if (result)
    {
        DirectReset (runtime);
        return result;
    }

    // Execute the module start function before the requested entry without
    // losing the pending entry call. It is pushed as the innermost frame, so
    // fuel exhaustion can suspend and resume across initialization normally.
    if (startFunction)
    {
        module->startFunction = -1;
        result = DirectPushFrame (runtime, startFunction);
        if (result)
        {
            module->startFunction = startIndex;
            DirectReset (runtime);
        }
    }
    return result;
}

static M3Result DirectUnwindValues (IM3Runtime runtime, u32 height, u32 arity)
{
    if (height > runtime->directValueTop || runtime->directValueTop - height < arity)
        return m3Err_functionStackUnderrun;

    u32 source = runtime->directValueTop - arity;
    memmove (DirectValues (runtime) + height,
             DirectValues (runtime) + source,
             arity * sizeof (u64));
    runtime->directValueTop = height + arity;
    return m3Err_none;
}

static M3Result DirectEnterControl (IM3Runtime runtime, M3DirectFrame * frame,
                                    m3opcode_t opcode, IM3FuncType type, bytes_t body)
{
    M3DirectControl control;
    M3_INIT (control);

    if (runtime->directValueTop < type->numArgs)
        return m3Err_functionStackUnderrun;

    M3Result result = DirectFindBlockBounds (frame->function->module, body, frame->end,
                                              & control.elsePc, & control.endPc);
    if (result)
        return result;

    control.bodyPc = body;
    control.stackHeight = runtime->directValueTop - type->numArgs;
    control.numParams = type->numArgs;
    control.numResults = type->numRets;
    control.opcode = (u8) opcode;
    return DirectPushControl (runtime, & control);
}

static M3Result DirectEndControl (IM3Runtime runtime)
{
    if (runtime->numDirectFrames == 0)
        return m3Err_wasmMalformed;

    M3DirectFrame * frame = &runtime->directFrames[runtime->numDirectFrames - 1];
    if (runtime->numDirectControls <= frame->controlBase)
        return m3Err_wasmMalformed;

    M3DirectControl * control = &runtime->directControls[runtime->numDirectControls - 1];
    if (control->opcode == c_m3DirectFunctionControl)
        return DirectReturnFrame (runtime);

    M3Result result = DirectUnwindValues (runtime, control->stackHeight, control->numResults);
    if (result)
        return result;
    --runtime->numDirectControls;
    return m3Err_none;
}

static M3Result DirectElseControl (IM3Runtime runtime, M3DirectFrame * frame)
{
    if (runtime->numDirectControls <= frame->controlBase)
        return m3Err_wasmMalformed;

    M3DirectControl * control = &runtime->directControls[runtime->numDirectControls - 1];
    if (control->opcode != c_waOp_if || not control->elsePc)
        return m3Err_wasmMalformed;

    M3Result result = DirectUnwindValues (runtime, control->stackHeight, control->numResults);
    if (result)
        return result;

    frame->pc = control->endPc + 1;
    --runtime->numDirectControls;
    return m3Err_none;
}

static M3Result DirectBranch (IM3Runtime runtime, u32 depth)
{
    if (runtime->numDirectFrames == 0)
        return m3Err_wasmMalformed;

    M3DirectFrame * frame = &runtime->directFrames[runtime->numDirectFrames - 1];
    u32 controlsInFrame = runtime->numDirectControls - frame->controlBase;
    if (depth >= controlsInFrame)
        return m3Err_wasmMalformed;

    u32 targetIndex = runtime->numDirectControls - 1 - depth;
    M3DirectControl * target = &runtime->directControls[targetIndex];
    u32 arity = target->opcode == c_waOp_loop ? target->numParams : target->numResults;
    M3Result result = DirectUnwindValues (runtime, target->stackHeight, arity);
    if (result)
        return result;

    if (target->opcode == c_m3DirectFunctionControl)
        return DirectReturnFrame (runtime);

    if (target->opcode == c_waOp_loop)
    {
        runtime->numDirectControls = targetIndex + 1;
        frame->pc = target->bodyPc;
    }
    else
    {
        bytes_t next = target->endPc + 1;
        runtime->numDirectControls = targetIndex;
        frame->pc = next;
    }
    return m3Err_none;
}

static M3Result DirectCallRaw (IM3Runtime runtime, IM3Function function)
{
    u32 numArgs = function->funcType->numArgs;
    u32 numRets = function->funcType->numRets;
    if (runtime->directValueTop < numArgs)
        return m3Err_functionStackUnderrun;
    if (not function->rawFunction)
        return m3Err_functionImportMissing;

    u32 base = runtime->directValueTop - numArgs;
    if (numRets > DirectValueCapacity (runtime) - runtime->directValueTop)
        return m3Err_trapStackOverflow;

    u64 * values = DirectValues (runtime);
    memmove (values + base + numRets, values + base, numArgs * sizeof (u64));
    memset (values + base, 0, numRets * sizeof (u64));

    // The public raw-call ABI stores each value in an eight-byte slot, but
    // 32-bit C values occupy the first four bytes of that slot. Convert the
    // native integer representation used by the interpreter before crossing
    // that boundary. This is a no-op in practice on little-endian targets and
    // is required on big-endian targets.
    for (u32 i = 0; i < numArgs; ++i)
    {
        u8 type = d_FuncArgType (function->funcType, i);
        if (type == c_m3Type_i32 || type == c_m3Type_f32)
        {
            u32 word = (u32) values[base + numRets + i];
            values[base + numRets + i] = 0;
            memcpy (&values[base + numRets + i], &word, sizeof word);
        }
    }

    M3ImportContext context;
    context.function = function;
    context.userdata = (void *) function->rawUserdata;

    M3RawCall call = function->rawFunction;
    void * memory = runtime->memory.mallocated ? m3MemData (runtime->memory.mallocated) : NULL;
    M3Result result = (M3Result) call (runtime, & context, values + base, memory);
    if (not result)
    {
        for (u32 i = 0; i < numRets; ++i)
        {
            u8 type = d_FuncRetType (function->funcType, i);
            if (type == c_m3Type_i32 || type == c_m3Type_f32)
            {
                u32 word;
                memcpy (&word, &values[base + i], sizeof word);
                values[base + i] = word;
            }
        }
    }
    runtime->directValueTop = base + numRets;
    return result;
}

static M3Result DirectCallFunction (IM3Runtime runtime, IM3Function function, bool tailCall)
{
    M3Result result = DirectValidateFunctionGraph (function);
    if (result)
        return result;

    result = m3_Yield ();
    if (result)
        return result;

    if (function->rawFunction || not function->wasm)
    {
        result = DirectCallRaw (runtime, function);
        if (not result && tailCall)
            result = DirectReturnFrame (runtime);
        return result;
    }

    if (not tailCall)
        return DirectPushFrame (runtime, function);

    if (runtime->numDirectFrames == 0)
        return m3Err_wasmMalformed;

    u32 numArgs = function->funcType->numArgs;
    if (runtime->directValueTop < numArgs)
        return m3Err_functionStackUnderrun;

    M3DirectFrame * oldFrame = &runtime->directFrames[runtime->numDirectFrames - 1];
    u32 source = runtime->directValueTop - numArgs;
    memmove (DirectValues (runtime) + oldFrame->localBase,
             DirectValues (runtime) + source,
             numArgs * sizeof (u64));
    runtime->directValueTop = oldFrame->localBase + numArgs;
    runtime->numDirectControls = oldFrame->controlBase;
    --runtime->numDirectFrames;
    return DirectPushFrame (runtime, function);
}

static M3Result DirectMemoryBounds (IM3Runtime runtime, u64 address, u32 size, u8 ** pointer)
{
    if (not runtime->memory.mallocated ||
        address > runtime->memory.mallocated->length ||
        size > runtime->memory.mallocated->length - (size_t) address)
        return m3Err_trapOutOfBoundsMemoryAccess;

    *pointer = m3MemData (runtime->memory.mallocated) + (size_t) address;
    return m3Err_none;
}

static M3Result DirectReadMemoryImmediate (M3DirectFrame * frame, u32 * offset)
{
    u32 alignment;
    M3Result result = ReadLEB_u32 (& alignment, & frame->pc, frame->end);
    if (result)
        return result;
    return ReadLEB_u32 (offset, & frame->pc, frame->end);
}

static M3Result DirectLoadBits (IM3Runtime runtime, M3DirectFrame * frame,
                                u32 byteCount, bool signExtend, u32 resultBits)
{
    u32 offset;
    M3Result result = DirectReadMemoryImmediate (frame, & offset);
    if (result)
        return result;

    u64 address = 0;
    result = DirectPop (runtime, & address);
    if (result)
        return result;
    address = (u32) address;
    address += offset;

    u8 * source = NULL;
    result = DirectMemoryBounds (runtime, address, byteCount, & source);
    if (result)
        return result;

    u64 value = 0;
    for (u32 i = 0; i < byteCount; ++i)
        value |= (u64) source[i] << (i * 8);

    if (signExtend && byteCount < 8)
    {
        u32 shift = 64 - byteCount * 8;
        value = (u64)(((i64)(value << shift)) >> shift);
    }
    if (resultBits == 32)
        value = (u32) value;
    return DirectPush (runtime, value);
}

static M3Result DirectStoreBits (IM3Runtime runtime, M3DirectFrame * frame, u32 byteCount)
{
    u32 offset;
    M3Result result = DirectReadMemoryImmediate (frame, & offset);
    if (result)
        return result;

    u64 value = 0;
    u64 address = 0;
    result = DirectPop (runtime, & value);
    if (result)
        return result;
    result = DirectPop (runtime, & address);
    if (result)
        return result;
    address = (u32) address;
    address += offset;

    u8 * destination = NULL;
    result = DirectMemoryBounds (runtime, address, byteCount, & destination);
    if (result)
        return result;

    for (u32 i = 0; i < byteCount; ++i)
        destination[i] = (u8) (value >> (i * 8));
    return m3Err_none;
}

static M3Result DirectGetGlobalValue (IM3Global global, u64 * value)
{
    switch (global->type)
    {
    case c_m3Type_i32:
        *value = (u32) global->i32Value;
        return m3Err_none;
    case c_m3Type_i64:
        *value = (u64) global->i64Value;
        return m3Err_none;
#if d_m3HasFloat
    case c_m3Type_f32:
        *value = DirectFromF32 (global->f32Value);
        return m3Err_none;
    case c_m3Type_f64:
        *value = DirectFromF64 (global->f64Value);
        return m3Err_none;
#endif
    default:
        return m3Err_invalidTypeId;
    }
}

static M3Result DirectSetGlobalValue (IM3Global global, u64 value)
{
    if (not global->isMutable)
        return m3Err_settingImmutableGlobal;

    switch (global->type)
    {
    case c_m3Type_i32:
        global->i32Value = (i32) value;
        return m3Err_none;
    case c_m3Type_i64:
        global->i64Value = (i64) value;
        return m3Err_none;
#if d_m3HasFloat
    case c_m3Type_f32:
        global->f32Value = DirectAsF32 (value);
        return m3Err_none;
    case c_m3Type_f64:
        global->f64Value = DirectAsF64 (value);
        return m3Err_none;
#endif
    default:
        return m3Err_invalidTypeId;
    }
}

M3Result DirectParseInitExpression (IM3Module module, bytes_t * bytes, cbytes_t end)
{
    bytes_t pc = *bytes;
    m3opcode_t opcode;
    M3Result result = DirectReadOpcode (& opcode, & pc, end);
    if (result)
        return result;

    switch (opcode)
    {
    case c_waOp_i32_const:
    {
        i32 value;
        result = ReadLEB_i32 (& value, & pc, end);
        break;
    }
    case c_waOp_i64_const:
    {
        i64 value;
        result = ReadLEB_i64 (& value, & pc, end);
        break;
    }
#if d_m3HasFloat
    case c_waOp_f32_const:
    {
        f32 value;
        result = Read_f32 (& value, & pc, end);
        break;
    }
    case c_waOp_f64_const:
    {
        f64 value;
        result = Read_f64 (& value, & pc, end);
        break;
    }
#endif
    case c_waOp_getGlobal:
    {
        u32 index;
        result = ReadLEB_u32 (& index, & pc, end);
        if (not result && index >= module->numGlobals)
            result = m3Err_globaIndexOutOfBounds;
        break;
    }
    default:
        result = m3Err_restrictedOpcode;
        break;
    }
    if (result)
        return result;

    result = DirectReadOpcode (& opcode, & pc, end);
    if (result)
        return result;
    if (opcode != c_waOp_end)
        return m3Err_wasmMalformed;

    *bytes = pc;
    return m3Err_none;
}

M3Result DirectEvaluateExpression (IM3Module module, void * expressed, u8 type,
                                   bytes_t * bytes, cbytes_t end)
{
    bytes_t pc = *bytes;
    m3opcode_t opcode;
    u64 value = 0;
    M3Result result = DirectReadOpcode (& opcode, & pc, end);
    if (result)
        return result;

    switch (opcode)
    {
    case c_waOp_i32_const:
    {
        i32 v;
        result = ReadLEB_i32 (& v, & pc, end);
        value = (u32) v;
        break;
    }
    case c_waOp_i64_const:
    {
        i64 v;
        result = ReadLEB_i64 (& v, & pc, end);
        value = (u64) v;
        break;
    }
#if d_m3HasFloat
    case c_waOp_f32_const:
    {
        f32 v;
        result = Read_f32 (& v, & pc, end);
        value = DirectFromF32 (v);
        break;
    }
    case c_waOp_f64_const:
    {
        f64 v;
        result = Read_f64 (& v, & pc, end);
        value = DirectFromF64 (v);
        break;
    }
#endif
    case c_waOp_getGlobal:
    {
        u32 index;
        result = ReadLEB_u32 (& index, & pc, end);
        if (not result)
        {
            if (index >= module->numGlobals)
                result = m3Err_globaIndexOutOfBounds;
            else
                result = DirectGetGlobalValue (& module->globals[index], & value);
        }
        break;
    }
    default:
        result = m3Err_restrictedOpcode;
        break;
    }
    if (result)
        return result;

    m3opcode_t endOpcode;
    result = DirectReadOpcode (& endOpcode, & pc, end);
    if (result)
        return result;
    if (endOpcode != c_waOp_end)
        return m3Err_wasmMalformed;

    if (SizeOfType (type) == sizeof (u32))
        *(u32 *) expressed = (u32) value;
    else
        *(u64 *) expressed = value;
    *bytes = pc;
    return m3Err_none;
}

static M3Result DirectPop2 (IM3Runtime runtime, u64 * a, u64 * b)
{
    M3Result result = DirectPop (runtime, b);
    if (result)
        return result;
    return DirectPop (runtime, a);
}

static M3Result DirectInteger32 (IM3Runtime runtime, m3opcode_t opcode)
{
    u64 av = 0, bv = 0;
    M3Result result;

    if (opcode >= 0x67 && opcode <= 0x69)
    {
        result = DirectPop (runtime, & av);
        if (result)
            return result;
        u32 a = (u32) av;
        u32 value;
        if (opcode == 0x67)
            value = a ? (u32)__builtin_clz (a) : 32;
        else if (opcode == 0x68)
            value = a ? (u32)__builtin_ctz (a) : 32;
        else
            value = (u32)__builtin_popcount (a);
        return DirectPush (runtime, value);
    }

    result = DirectPop2 (runtime, & av, & bv);
    if (result)
        return result;
    u32 a = (u32) av;
    u32 b = (u32) bv;
    u32 value;

    switch (opcode)
    {
    case 0x6a: value = a + b; break;
    case 0x6b: value = a - b; break;
    case 0x6c: value = a * b; break;
    case 0x6d:
        if ((i32)b == 0) return m3Err_trapDivisionByZero;
        if ((i32)a == INT32_MIN && (i32)b == -1) return m3Err_trapIntegerOverflow;
        value = (u32)((i32)a / (i32)b);
        break;
    case 0x6e:
        if (b == 0) return m3Err_trapDivisionByZero;
        value = a / b;
        break;
    case 0x6f:
        if ((i32)b == 0) return m3Err_trapDivisionByZero;
        value = ((i32)a == INT32_MIN && (i32)b == -1) ? 0 : (u32)((i32)a % (i32)b);
        break;
    case 0x70:
        if (b == 0) return m3Err_trapDivisionByZero;
        value = a % b;
        break;
    case 0x71: value = a & b; break;
    case 0x72: value = a | b; break;
    case 0x73: value = a ^ b; break;
    case 0x74: value = a << (b & 31); break;
    case 0x75: value = (u32)((i32)a >> (b & 31)); break;
    case 0x76: value = a >> (b & 31); break;
    case 0x77: value = rotl32 (a, b); break;
    case 0x78: value = rotr32 (a, b); break;
    default: return m3Err_unknownOpcode;
    }
    return DirectPush (runtime, value);
}

static M3Result DirectInteger64 (IM3Runtime runtime, m3opcode_t opcode)
{
    u64 a = 0, b = 0;
    M3Result result;

    if (opcode >= 0x79 && opcode <= 0x7b)
    {
        result = DirectPop (runtime, & a);
        if (result)
            return result;
        u64 value;
        if (opcode == 0x79)
            value = a ? (u64)__builtin_clzll (a) : 64;
        else if (opcode == 0x7a)
            value = a ? (u64)__builtin_ctzll (a) : 64;
        else
            value = (u64)__builtin_popcountll (a);
        return DirectPush (runtime, value);
    }

    result = DirectPop2 (runtime, & a, & b);
    if (result)
        return result;
    u64 value;

    switch (opcode)
    {
    case 0x7c: value = a + b; break;
    case 0x7d: value = a - b; break;
    case 0x7e: value = a * b; break;
    case 0x7f:
        if ((i64)b == 0) return m3Err_trapDivisionByZero;
        if ((i64)a == INT64_MIN && (i64)b == -1) return m3Err_trapIntegerOverflow;
        value = (u64)((i64)a / (i64)b);
        break;
    case 0x80:
        if (b == 0) return m3Err_trapDivisionByZero;
        value = a / b;
        break;
    case 0x81:
        if ((i64)b == 0) return m3Err_trapDivisionByZero;
        value = ((i64)a == INT64_MIN && (i64)b == -1) ? 0 : (u64)((i64)a % (i64)b);
        break;
    case 0x82:
        if (b == 0) return m3Err_trapDivisionByZero;
        value = a % b;
        break;
    case 0x83: value = a & b; break;
    case 0x84: value = a | b; break;
    case 0x85: value = a ^ b; break;
    case 0x86: value = a << (b & 63); break;
    case 0x87: value = (u64)((i64)a >> (b & 63)); break;
    case 0x88: value = a >> (b & 63); break;
    case 0x89: value = rotl64 (a, b); break;
    case 0x8a: value = rotr64 (a, b); break;
    default: return m3Err_unknownOpcode;
    }
    return DirectPush (runtime, value);
}

static M3Result DirectCompare (IM3Runtime runtime, m3opcode_t opcode)
{
    u64 a = 0, b = 0;
    M3Result result;

    if (opcode == 0x45 || opcode == 0x50)
    {
        result = DirectPop (runtime, & a);
        if (result)
            return result;
        if (opcode == 0x45)
            return DirectPush (runtime, (u32)a == 0);
        return DirectPush (runtime, a == 0);
    }

    result = DirectPop2 (runtime, & a, & b);
    if (result)
        return result;

    bool value;
    switch (opcode)
    {
    case 0x46: value = (u32)a == (u32)b; break;
    case 0x47: value = (u32)a != (u32)b; break;
    case 0x48: value = (i32)a <  (i32)b; break;
    case 0x49: value = (u32)a <  (u32)b; break;
    case 0x4a: value = (i32)a >  (i32)b; break;
    case 0x4b: value = (u32)a >  (u32)b; break;
    case 0x4c: value = (i32)a <= (i32)b; break;
    case 0x4d: value = (u32)a <= (u32)b; break;
    case 0x4e: value = (i32)a >= (i32)b; break;
    case 0x4f: value = (u32)a >= (u32)b; break;

    case 0x51: value = a == b; break;
    case 0x52: value = a != b; break;
    case 0x53: value = (i64)a <  (i64)b; break;
    case 0x54: value = a < b; break;
    case 0x55: value = (i64)a >  (i64)b; break;
    case 0x56: value = a > b; break;
    case 0x57: value = (i64)a <= (i64)b; break;
    case 0x58: value = a <= b; break;
    case 0x59: value = (i64)a >= (i64)b; break;
    case 0x5a: value = a >= b; break;

#if d_m3HasFloat
    case 0x5b: value = DirectAsF32 (a) == DirectAsF32 (b); break;
    case 0x5c: value = DirectAsF32 (a) != DirectAsF32 (b); break;
    case 0x5d: value = DirectAsF32 (a) <  DirectAsF32 (b); break;
    case 0x5e: value = DirectAsF32 (a) >  DirectAsF32 (b); break;
    case 0x5f: value = DirectAsF32 (a) <= DirectAsF32 (b); break;
    case 0x60: value = DirectAsF32 (a) >= DirectAsF32 (b); break;
    case 0x61: value = DirectAsF64 (a) == DirectAsF64 (b); break;
    case 0x62: value = DirectAsF64 (a) != DirectAsF64 (b); break;
    case 0x63: value = DirectAsF64 (a) <  DirectAsF64 (b); break;
    case 0x64: value = DirectAsF64 (a) >  DirectAsF64 (b); break;
    case 0x65: value = DirectAsF64 (a) <= DirectAsF64 (b); break;
    case 0x66: value = DirectAsF64 (a) >= DirectAsF64 (b); break;
#endif
    default: return m3Err_unknownOpcode;
    }
    return DirectPush (runtime, value ? 1 : 0);
}

#if d_m3HasFloat
static M3Result DirectFloat32 (IM3Runtime runtime, m3opcode_t opcode)
{
    u64 bitsA = 0, bitsB = 0;
    M3Result result = DirectPop (runtime, & bitsA);
    if (result)
        return result;
    f32 a = DirectAsF32 (bitsA);
    f32 value;

    if (opcode <= 0x91)
    {
        switch (opcode)
        {
        case 0x8b: value = fabsf (a); break;
        case 0x8c: value = -a; break;
        case 0x8d: value = ceilf (a); break;
        case 0x8e: value = floorf (a); break;
        case 0x8f: value = truncf (a); break;
        case 0x90: value = rintf (a); break;
        case 0x91: value = sqrtf (a); break;
        default: return m3Err_unknownOpcode;
        }
        return DirectPush (runtime, DirectFromF32 (value));
    }

    result = DirectPop (runtime, & bitsB);
    if (result)
        return result;
    f32 b = a;
    a = DirectAsF32 (bitsB);
    switch (opcode)
    {
    case 0x92: value = a + b; break;
    case 0x93: value = a - b; break;
    case 0x94: value = a * b; break;
    case 0x95: value = a / b; break;
    case 0x96: value = min_f32 (a, b); break;
    case 0x97: value = max_f32 (a, b); break;
    case 0x98: value = copysignf (a, b); break;
    default: return m3Err_unknownOpcode;
    }
    return DirectPush (runtime, DirectFromF32 (value));
}

static M3Result DirectFloat64 (IM3Runtime runtime, m3opcode_t opcode)
{
    u64 bitsA = 0, bitsB = 0;
    M3Result result = DirectPop (runtime, & bitsA);
    if (result)
        return result;
    f64 a = DirectAsF64 (bitsA);
    f64 value;

    if (opcode <= 0x9f)
    {
        switch (opcode)
        {
        case 0x99: value = fabs (a); break;
        case 0x9a: value = -a; break;
        case 0x9b: value = ceil (a); break;
        case 0x9c: value = floor (a); break;
        case 0x9d: value = trunc (a); break;
        case 0x9e: value = rint (a); break;
        case 0x9f: value = sqrt (a); break;
        default: return m3Err_unknownOpcode;
        }
        return DirectPush (runtime, DirectFromF64 (value));
    }

    result = DirectPop (runtime, & bitsB);
    if (result)
        return result;
    f64 b = a;
    a = DirectAsF64 (bitsB);
    switch (opcode)
    {
    case 0xa0: value = a + b; break;
    case 0xa1: value = a - b; break;
    case 0xa2: value = a * b; break;
    case 0xa3: value = a / b; break;
    case 0xa4: value = min_f64 (a, b); break;
    case 0xa5: value = max_f64 (a, b); break;
    case 0xa6: value = copysign (a, b); break;
    default: return m3Err_unknownOpcode;
    }
    return DirectPush (runtime, DirectFromF64 (value));
}

static M3Result DirectTruncate (long double value, bool sourceF32,
                                bool isUnsigned, u32 bits, bool saturating,
                                u64 * output)
{
    if (isnan ((double) value))
    {
        if (not saturating)
            return m3Err_trapIntegerConversion;
        *output = 0;
        return m3Err_none;
    }

    long double lower;
    if (isUnsigned)
        lower = -1.0L;
    else if (bits == 32)
        lower = sourceF32 ? -2147483904.0L : -2147483649.0L;
    else
        lower = sourceF32 ? -9223373136366403584.0L
                          : -9223372036854777856.0L;
    long double upper = isUnsigned ?
                        (bits == 32 ? 4294967296.0L : 18446744073709551616.0L) :
                        (bits == 32 ? 2147483648.0L : 9223372036854775808.0L);

    if (value <= lower)
    {
        if (not saturating)
            return m3Err_trapIntegerOverflow;
        *output = isUnsigned ? 0 : (bits == 32 ? (u32)INT32_MIN : (u64)INT64_MIN);
        return m3Err_none;
    }
    if (value >= upper)
    {
        if (not saturating)
            return m3Err_trapIntegerOverflow;
        *output = isUnsigned ? (bits == 32 ? UINT32_MAX : UINT64_MAX) :
                               (bits == 32 ? INT32_MAX : INT64_MAX);
        return m3Err_none;
    }

    if (isUnsigned)
        *output = bits == 32 ? (u32) value : (u64) value;
    else
        *output = bits == 32 ? (u32)(i32) value : (u64)(i64) value;
    return m3Err_none;
}
#endif

static M3Result DirectConvert (IM3Runtime runtime, m3opcode_t opcode)
{
    u64 value = 0;
    M3Result result = DirectPop (runtime, & value);
    if (result)
        return result;

    switch (opcode)
    {
    case 0xa7:
        value = (u32) value;
        break;

#if d_m3HasFloat
    case 0xa8: case 0xa9: case 0xaa: case 0xab:
    case 0xae: case 0xaf: case 0xb0: case 0xb1:
    {
        bool sourceF32 = opcode == 0xa8 || opcode == 0xa9 || opcode == 0xae || opcode == 0xaf;
        bool isUnsigned = opcode == 0xa9 || opcode == 0xab || opcode == 0xaf || opcode == 0xb1;
        u32 bits = opcode <= 0xab ? 32 : 64;
        long double input = sourceF32 ? (long double)DirectAsF32 (value)
                                     : (long double)DirectAsF64 (value);
        result = DirectTruncate (input, sourceF32, isUnsigned, bits, false, & value);
        if (result)
            return result;
        break;
    }
#endif

    case 0xac: value = (u64)(i64)(i32)(u32)value; break;
    case 0xad: value = (u32)value; break;

#if d_m3HasFloat
    case 0xb2: value = DirectFromF32 ((f32)(i32)(u32)value); break;
    case 0xb3: value = DirectFromF32 ((f32)(u32)value); break;
    case 0xb4: value = DirectFromF32 ((f32)(i64)value); break;
    case 0xb5: value = DirectFromF32 ((f32)(u64)value); break;
    case 0xb6: value = DirectFromF32 ((f32)DirectAsF64 (value)); break;
    case 0xb7: value = DirectFromF64 ((f64)(i32)(u32)value); break;
    case 0xb8: value = DirectFromF64 ((f64)(u32)value); break;
    case 0xb9: value = DirectFromF64 ((f64)(i64)value); break;
    case 0xba: value = DirectFromF64 ((f64)(u64)value); break;
    case 0xbb: value = DirectFromF64 ((f64)DirectAsF32 (value)); break;
    case 0xbc: value = (u32)value; break;
    case 0xbd: break;
    case 0xbe: value = (u32)value; break;
    case 0xbf: break;
#endif

    case 0xc0: value = (u32)(i32)(i8)(u8)value; break;
    case 0xc1: value = (u32)(i32)(i16)(u16)value; break;
    case 0xc2: value = (u64)(i64)(i8)(u8)value; break;
    case 0xc3: value = (u64)(i64)(i16)(u16)value; break;
    case 0xc4: value = (u64)(i64)(i32)(u32)value; break;
    default:
        return m3Err_unknownOpcode;
    }
    return DirectPush (runtime, value);
}

static M3Result DirectExtended (IM3Runtime runtime, M3DirectFrame * frame, m3opcode_t opcode)
{
    if (opcode >= 0xfc00 && opcode <= 0xfc07)
    {
#if d_m3HasFloat
        u64 input = 0;
        M3Result result = DirectPop (runtime, & input);
        if (result)
            return result;

        u32 sub = opcode & 0xff;
        bool sourceF32 = sub == 0 || sub == 1 || sub == 4 || sub == 5;
        bool isUnsigned = sub == 1 || sub == 3 || sub == 5 || sub == 7;
        u32 bits = sub <= 3 ? 32 : 64;
        long double value = sourceF32 ? (long double)DirectAsF32 (input)
                                     : (long double)DirectAsF64 (input);
        u64 output;
        result = DirectTruncate (value, sourceF32, isUnsigned, bits, true, & output);
        if (result)
            return result;
        return DirectPush (runtime, output);
#else
        return m3Err_unknownOpcode;
#endif
    }

    if (opcode == c_waOp_memoryCopy)
    {
        u32 sourceMemory, targetMemory;
        M3Result result = ReadLEB_u32 (& sourceMemory, & frame->pc, frame->end);
        if (result)
            return result;
        result = ReadLEB_u32 (& targetMemory, & frame->pc, frame->end);
        if (result)
            return result;
        if (sourceMemory != 0 || targetMemory != 0)
            return m3Err_wasmMalformed;

        u64 sizeValue = 0, sourceValue = 0, destinationValue = 0;
        result = DirectPop (runtime, & sizeValue);
        if (result) return result;
        result = DirectPop (runtime, & sourceValue);
        if (result) return result;
        result = DirectPop (runtime, & destinationValue);
        if (result) return result;

        u32 size = (u32) sizeValue;
        u64 source = (u32) sourceValue;
        u64 destination = (u32) destinationValue;
        u8 * sourcePtr = NULL;
        u8 * destinationPtr = NULL;
        result = DirectMemoryBounds (runtime, source, size, & sourcePtr);
        if (result) return result;
        result = DirectMemoryBounds (runtime, destination, size, & destinationPtr);
        if (result) return result;
        memmove (destinationPtr, sourcePtr, size);
        return m3Err_none;
    }

    if (opcode == c_waOp_memoryFill)
    {
        u32 memoryIndex;
        M3Result result = ReadLEB_u32 (& memoryIndex, & frame->pc, frame->end);
        if (result)
            return result;
        if (memoryIndex != 0)
            return m3Err_wasmMalformed;

        u64 sizeValue = 0, byteValue = 0, destinationValue = 0;
        result = DirectPop (runtime, & sizeValue);
        if (result) return result;
        result = DirectPop (runtime, & byteValue);
        if (result) return result;
        result = DirectPop (runtime, & destinationValue);
        if (result) return result;

        u32 size = (u32) sizeValue;
        u64 destination = (u32) destinationValue;
        u8 * destinationPtr = NULL;
        result = DirectMemoryBounds (runtime, destination, size, & destinationPtr);
        if (result) return result;
        memset (destinationPtr, (u8) byteValue, size);
        return m3Err_none;
    }

    return m3Err_unknownOpcode;
}

static M3Result DirectExecuteInstruction (IM3Runtime runtime)
{
    if (runtime->numDirectFrames == 0)
        return m3Err_runtimeSuspended;

    M3DirectFrame * frame = &runtime->directFrames[runtime->numDirectFrames - 1];
    if (frame->pc >= frame->end)
        return m3Err_wasmMalformed;

    m3opcode_t opcode;
    M3Result result = DirectReadOpcode (& opcode, & frame->pc, frame->end);
    if (result)
        return result;

    switch (opcode)
    {
    case 0x00:
        return m3Err_trapUnreachable;
    case 0x01:
        return m3Err_none;

    case c_waOp_block:
    case c_waOp_loop:
    {
        IM3FuncType type;
        result = DirectReadBlockType (frame->function->module, & type, & frame->pc, frame->end);
        if (result)
            return result;
        return DirectEnterControl (runtime, frame, opcode, type, frame->pc);
    }

    case c_waOp_if:
    {
        IM3FuncType type;
        result = DirectReadBlockType (frame->function->module, & type, & frame->pc, frame->end);
        if (result)
            return result;

        u64 condition = 0;
        result = DirectPop (runtime, & condition);
        if (result)
            return result;
        result = DirectEnterControl (runtime, frame, opcode, type, frame->pc);
        if (result)
            return result;

        if ((u32) condition == 0)
        {
            M3DirectControl * control = &runtime->directControls[runtime->numDirectControls - 1];
            frame->pc = control->elsePc ? control->elsePc + 1 : control->endPc;
        }
        return m3Err_none;
    }

    case c_waOp_else:
        return DirectElseControl (runtime, frame);
    case c_waOp_end:
        return DirectEndControl (runtime);

    case c_waOp_branch:
    {
        u32 depth;
        result = ReadLEB_u32 (& depth, & frame->pc, frame->end);
        return result ? result : DirectBranch (runtime, depth);
    }

    case c_waOp_branchIf:
    {
        u32 depth;
        result = ReadLEB_u32 (& depth, & frame->pc, frame->end);
        if (result)
            return result;
        u64 condition = 0;
        result = DirectPop (runtime, & condition);
        if (result)
            return result;
        return (u32) condition ? DirectBranch (runtime, depth) : m3Err_none;
    }

    case c_waOp_branchTable:
    {
        u32 count;
        result = ReadLEB_u32 (& count, & frame->pc, frame->end);
        if (result)
            return result;
        if (count > d_m3MaxSaneTableSize)
            return m3Err_wasmMalformed;

        u64 selectorValue = 0;
        result = DirectPop (runtime, & selectorValue);
        if (result)
            return result;
        u32 selector = (u32) selectorValue;
        u32 selectedDepth = 0;
        for (u32 i = 0; i <= count; ++i)
        {
            u32 depth;
            result = ReadLEB_u32 (& depth, & frame->pc, frame->end);
            if (result)
                return result;
            if (i == selector || (selector >= count && i == count))
                selectedDepth = depth;
        }
        return DirectBranch (runtime, selectedDepth);
    }

    case 0x0f:
        return DirectReturnFrame (runtime);

    case c_waOp_call:
    case 0x12:
    {
        u32 index;
        result = ReadLEB_u32 (& index, & frame->pc, frame->end);
        if (result)
            return result;
        IM3Module module = frame->function->module;
        if (index >= module->numFunctions)
            return m3Err_wasmMalformed;
        return DirectCallFunction (runtime, & module->functions[index], opcode == 0x12);
    }

    case 0x11:
    case 0x13:
    {
        u32 typeIndex, tableIndex;
        result = ReadLEB_u32 (& typeIndex, & frame->pc, frame->end);
        if (result) return result;
        result = ReadLEB_u32 (& tableIndex, & frame->pc, frame->end);
        if (result) return result;
        IM3Module module = frame->function->module;
        if (typeIndex >= module->numFuncTypes || tableIndex != 0)
            return m3Err_wasmMalformed;

        u64 elementValue = 0;
        result = DirectPop (runtime, & elementValue);
        if (result)
            return result;
        u32 element = (u32) elementValue;
        if (element >= module->table0Size)
            return m3Err_trapTableIndexOutOfRange;
        IM3Function function = module->table0[element];
        if (not function)
            return m3Err_trapTableElementIsNull;
        if (not AreFuncTypesEqual (module->funcTypes[typeIndex], function->funcType))
            return m3Err_trapIndirectCallTypeMismatch;
        return DirectCallFunction (runtime, function, opcode == 0x13);
    }

    case 0x1a:
    {
        u64 ignored = 0;
        return DirectPop (runtime, & ignored);
    }
    case 0x1b:
    {
        u64 condition = 0, second = 0, first = 0;
        result = DirectPop (runtime, & condition);
        if (result) return result;
        result = DirectPop (runtime, & second);
        if (result) return result;
        result = DirectPop (runtime, & first);
        if (result) return result;
        return DirectPush (runtime, (u32)condition ? first : second);
    }

    case c_waOp_getLocal:
    case c_waOp_setLocal:
    case c_waOp_teeLocal:
    {
        u32 index;
        result = ReadLEB_u32 (& index, & frame->pc, frame->end);
        if (result)
            return result;
        u32 localCount = frame->function->funcType->numArgs + frame->function->numLocals;
        if (index >= localCount)
            return m3Err_wasmMalformed;
        u32 slot = frame->localBase + index;
        if (slot >= DirectValueCapacity (runtime))
            return m3Err_trapStackOverflow;

        if (opcode == c_waOp_getLocal)
            return DirectPush (runtime, DirectValues (runtime)[slot]);

        u64 value = 0;
        result = opcode == c_waOp_teeLocal ? DirectPeek (runtime, & value)
                                           : DirectPop (runtime, & value);
        if (result)
            return result;
        DirectValues (runtime)[slot] = value;
        return m3Err_none;
    }

    case c_waOp_getGlobal:
    case 0x24:
    {
        u32 index;
        result = ReadLEB_u32 (& index, & frame->pc, frame->end);
        if (result)
            return result;
        IM3Module module = frame->function->module;
        if (index >= module->numGlobals)
            return m3Err_globaIndexOutOfBounds;

        if (opcode == c_waOp_getGlobal)
        {
            u64 value = 0;
            result = DirectGetGlobalValue (& module->globals[index], & value);
            return result ? result : DirectPush (runtime, value);
        }

        u64 value = 0;
        result = DirectPop (runtime, & value);
        return result ? result : DirectSetGlobalValue (& module->globals[index], value);
    }

    case 0x28: return DirectLoadBits (runtime, frame, 4, false, 32);
    case 0x29: return DirectLoadBits (runtime, frame, 8, false, 64);
    case 0x2a: return DirectLoadBits (runtime, frame, 4, false, 32);
    case 0x2b: return DirectLoadBits (runtime, frame, 8, false, 64);
    case 0x2c: return DirectLoadBits (runtime, frame, 1, true, 32);
    case 0x2d: return DirectLoadBits (runtime, frame, 1, false, 32);
    case 0x2e: return DirectLoadBits (runtime, frame, 2, true, 32);
    case 0x2f: return DirectLoadBits (runtime, frame, 2, false, 32);
    case 0x30: return DirectLoadBits (runtime, frame, 1, true, 64);
    case 0x31: return DirectLoadBits (runtime, frame, 1, false, 64);
    case 0x32: return DirectLoadBits (runtime, frame, 2, true, 64);
    case 0x33: return DirectLoadBits (runtime, frame, 2, false, 64);
    case 0x34: return DirectLoadBits (runtime, frame, 4, true, 64);
    case 0x35: return DirectLoadBits (runtime, frame, 4, false, 64);
    case 0x36: return DirectStoreBits (runtime, frame, 4);
    case 0x37: return DirectStoreBits (runtime, frame, 8);
    case 0x38: return DirectStoreBits (runtime, frame, 4);
    case 0x39: return DirectStoreBits (runtime, frame, 8);
    case 0x3a: return DirectStoreBits (runtime, frame, 1);
    case 0x3b: return DirectStoreBits (runtime, frame, 2);
    case 0x3c: return DirectStoreBits (runtime, frame, 1);
    case 0x3d: return DirectStoreBits (runtime, frame, 2);
    case 0x3e: return DirectStoreBits (runtime, frame, 4);

    case 0x3f:
    {
        u32 memoryIndex;
        result = ReadLEB_u32 (& memoryIndex, & frame->pc, frame->end);
        if (result) return result;
        if (memoryIndex != 0) return m3Err_wasmMalformed;
        return DirectPush (runtime, runtime->memory.numPages);
    }

    case 0x40:
    {
        u32 memoryIndex;
        result = ReadLEB_u32 (& memoryIndex, & frame->pc, frame->end);
        if (result) return result;
        if (memoryIndex != 0) return m3Err_wasmMalformed;
        u64 pagesValue = 0;
        result = DirectPop (runtime, & pagesValue);
        if (result) return result;
        i32 pages = (i32)(u32)pagesValue;
        u32 previous = runtime->memory.numPages;
        if (pages < 0 || (u32)pages > UINT32_MAX - previous ||
            ResizeMemory (runtime, previous + (u32)pages))
            return DirectPush (runtime, UINT32_MAX);
        return DirectPush (runtime, previous);
    }

    case c_waOp_i32_const:
    {
        i32 value;
        result = ReadLEB_i32 (& value, & frame->pc, frame->end);
        return result ? result : DirectPush (runtime, (u32)value);
    }
    case c_waOp_i64_const:
    {
        i64 value;
        result = ReadLEB_i64 (& value, & frame->pc, frame->end);
        return result ? result : DirectPush (runtime, (u64)value);
    }
#if d_m3HasFloat
    case c_waOp_f32_const:
    {
        f32 value;
        result = Read_f32 (& value, & frame->pc, frame->end);
        return result ? result : DirectPush (runtime, DirectFromF32 (value));
    }
    case c_waOp_f64_const:
    {
        f64 value;
        result = Read_f64 (& value, & frame->pc, frame->end);
        return result ? result : DirectPush (runtime, DirectFromF64 (value));
    }
#endif

    default:
        if (opcode >= 0x45 && opcode <= 0x66)
            return DirectCompare (runtime, opcode);
        if (opcode >= 0x67 && opcode <= 0x78)
            return DirectInteger32 (runtime, opcode);
        if (opcode >= 0x79 && opcode <= 0x8a)
            return DirectInteger64 (runtime, opcode);
#if d_m3HasFloat
        if (opcode >= 0x8b && opcode <= 0x98)
            return DirectFloat32 (runtime, opcode);
        if (opcode >= 0x99 && opcode <= 0xa6)
            return DirectFloat64 (runtime, opcode);
#endif
        if (opcode >= 0xa7 && opcode <= 0xc4)
            return DirectConvert (runtime, opcode);
        if ((opcode >> 8) == c_waOp_extended)
            return DirectExtended (runtime, frame, opcode);
        return m3Err_unknownOpcode;
    }
}

M3Result DirectExecute (IM3Runtime runtime, u64 fuel, u64 * consumed)
{
    if (consumed)
        *consumed = 0;
    if (not runtime || not runtime->directActive)
        return m3Err_runtimeSuspended;

    runtime->suspended = false;
    runtime->fuelEnabled = true;
    runtime->fuel = fuel;

    u64 executed = 0;
    while (runtime->directActive && executed < fuel && runtime->fuel > 0)
    {
        --runtime->fuel;
        M3Result result = DirectExecuteInstruction (runtime);
        ++executed;

        if (result)
        {
            if (consumed)
                *consumed = executed;
            if (result == m3Err_fuelExhausted)
            {
                runtime->suspended = true;
                runtime->suspendedFunction = runtime->directEntry;
                return result;
            }

            DirectReset (runtime);
            runtime->lastCalled = NULL;
            return result;
        }
    }

    if (consumed)
        *consumed = executed;
    if (runtime->directActive)
    {
        runtime->suspended = true;
        runtime->suspendedFunction = runtime->directEntry;
        return m3Err_fuelExhausted;
    }
    return m3Err_none;
}

M3Result DirectStep (IM3Runtime runtime)
{
    return DirectExecute (runtime, 1, NULL);
}

M3Result DirectRun (IM3Runtime runtime)
{
    if (not runtime || not runtime->directActive)
        return m3Err_runtimeSuspended;

    runtime->suspended = false;
    runtime->fuelEnabled = false;
    while (runtime->directActive)
    {
        M3Result result = DirectExecuteInstruction (runtime);
        if (result)
        {
            DirectReset (runtime);
            runtime->lastCalled = NULL;
            return result;
        }
    }
    return m3Err_none;
}

// Snapshot v2 stores only byte offsets and explicit interpreter state. The
// format is process-local and does not contain native code addresses.
#define M3_DIRECT_SNAPSHOT_MAGIC   0x3253444du /* MDS2 */
#define M3_DIRECT_SNAPSHOT_VERSION 2u
#define M3_DIRECT_SNAPSHOT_NO_PC   UINT32_MAX
#define M3_DIRECT_SNAPSHOT_FLAGS   (d_m3HasFloat ? 1u : 0u)

typedef struct M3DirectSnapshotHeader
{
    u32 magic, version, headerSize, flags;
    u32 totalSize;
    u32 stackSize, valueTop, frameCount, controlCount;
    u32 memoryPages, memoryPageSize, memoryBytes;
    u32 moduleCount, globalCount;
    u32 entryModuleIndex, entryFunctionIndex;
    u64 fuel;
    u32 fuelEnabled, suspended;
}
M3DirectSnapshotHeader;

typedef struct M3DirectSnapshotFrame
{
    u32 moduleIndex, functionIndex, pcOffset;
    u32 localBase, stackBase, controlBase;
}
M3DirectSnapshotFrame;

typedef struct M3DirectSnapshotControl
{
    u32 frameIndex;
    u32 bodyOffset, elseOffset, endOffset;
    u32 stackHeight;
    u16 numParams, numResults;
    u8 opcode, reserved[3];
}
M3DirectSnapshotControl;

typedef struct M3DirectSnapshotGlobal
{
    u32 moduleIndex, globalIndex, type;
    u64 value;
}
M3DirectSnapshotGlobal;

static u32 DirectSnapshotModuleCount (IM3Runtime runtime)
{
    u32 count = 0;
    for (IM3Module module = runtime ? runtime->modules : NULL; module; module = module->next)
        ++count;
    return count;
}

static u32 DirectSnapshotGlobalCount (IM3Runtime runtime)
{
    u32 count = 0;
    for (IM3Module module = runtime ? runtime->modules : NULL; module; module = module->next)
        count += module->numGlobals;
    return count;
}

static IM3Module DirectSnapshotGetModule (IM3Runtime runtime, u32 index)
{
    for (IM3Module module = runtime ? runtime->modules : NULL; module; module = module->next)
    {
        if (index-- == 0)
            return module;
    }
    return NULL;
}

static bool DirectSnapshotFunctionId (IM3Runtime runtime, IM3Function function,
                                      u32 * moduleIndex, u32 * functionIndex)
{
    u32 mi = 0;
    for (IM3Module module = runtime ? runtime->modules : NULL; module; module = module->next, ++mi)
    {
        if (function >= module->functions && function < module->functions + module->numFunctions)
        {
            *moduleIndex = mi;
            *functionIndex = (u32)(function - module->functions);
            return true;
        }
    }
    return false;
}

static IM3Function DirectSnapshotGetFunction (IM3Runtime runtime, u32 moduleIndex, u32 functionIndex)
{
    IM3Module module = DirectSnapshotGetModule (runtime, moduleIndex);
    if (not module || functionIndex >= module->numFunctions)
        return NULL;
    return &module->functions[functionIndex];
}

static u32 DirectSnapshotControlFrame (IM3Runtime runtime, u32 controlIndex)
{
    for (u32 i = runtime->numDirectFrames; i > 0; --i)
    {
        if (controlIndex >= runtime->directFrames[i - 1].controlBase)
            return i - 1;
    }
    return UINT32_MAX;
}

static M3Result DirectSnapshotCalculatedSize (IM3Runtime runtime, u32 * size)
{
    if (not runtime || not size)
        return m3Err_snapshotInvalid;

    u64 memoryBytes = runtime->memory.mallocated ? runtime->memory.mallocated->length : 0;
    u64 total = sizeof (M3DirectSnapshotHeader);
    total += (u64)runtime->numDirectFrames * sizeof (M3DirectSnapshotFrame);
    total += (u64)runtime->numDirectControls * sizeof (M3DirectSnapshotControl);
    total += runtime->stackSize;
    total += memoryBytes;
    total += (u64)DirectSnapshotGlobalCount (runtime) * sizeof (M3DirectSnapshotGlobal);
    if (total > UINT32_MAX)
        return m3Err_snapshotInvalid;
    *size = (u32)total;
    return m3Err_none;
}

M3Result DirectGetSnapshotSize (IM3Runtime runtime, u32 * outSize)
{
    if (not runtime || not outSize)
        return m3Err_snapshotInvalid;
    if (not runtime->suspended || not runtime->directActive ||
        runtime->numDirectFrames == 0 || runtime->numDirectControls == 0)
        return m3Err_snapshotUnsupported;
    return DirectSnapshotCalculatedSize (runtime, outSize);
}

M3Result DirectSaveSnapshot (IM3Runtime runtime, u8 * buffer, u32 bufferSize, u32 * outSize)
{
    M3Result result = DirectGetSnapshotSize (runtime, outSize);
    if (result)
        return result;
    if (not buffer || bufferSize < *outSize)
        return m3Err_snapshotBufferTooSmall;

    u32 entryModule, entryFunction;
    if (not DirectSnapshotFunctionId (runtime, runtime->directEntry, & entryModule, & entryFunction))
        return m3Err_snapshotUnsupported;

    M3DirectSnapshotHeader header;
    M3_INIT (header);
    header.magic = M3_DIRECT_SNAPSHOT_MAGIC;
    header.version = M3_DIRECT_SNAPSHOT_VERSION;
    header.headerSize = sizeof header;
    header.flags = M3_DIRECT_SNAPSHOT_FLAGS;
    header.totalSize = *outSize;
    header.stackSize = runtime->stackSize;
    header.valueTop = runtime->directValueTop;
    header.frameCount = runtime->numDirectFrames;
    header.controlCount = runtime->numDirectControls;
    header.memoryPages = runtime->memory.numPages;
    header.memoryPageSize = runtime->memory.pageSize;
    header.memoryBytes = runtime->memory.mallocated ? (u32)runtime->memory.mallocated->length : 0;
    header.moduleCount = DirectSnapshotModuleCount (runtime);
    header.globalCount = DirectSnapshotGlobalCount (runtime);
    header.entryModuleIndex = entryModule;
    header.entryFunctionIndex = entryFunction;
    header.fuel = runtime->fuel;
    header.fuelEnabled = runtime->fuelEnabled;
    header.suspended = runtime->suspended;

    u8 * output = buffer;
    memcpy (output, & header, sizeof header);
    output += sizeof header;

    for (u32 i = 0; i < runtime->numDirectFrames; ++i)
    {
        M3DirectFrame * frame = &runtime->directFrames[i];
        M3DirectSnapshotFrame saved;
        if (not DirectSnapshotFunctionId (runtime, frame->function,
                                          & saved.moduleIndex, & saved.functionIndex))
            return m3Err_snapshotUnsupported;
        if (frame->pc < frame->function->wasm || frame->pc > frame->function->wasmEnd)
            return m3Err_snapshotUnsupported;
        saved.pcOffset = (u32)(frame->pc - frame->function->wasm);
        saved.localBase = frame->localBase;
        saved.stackBase = frame->stackBase;
        saved.controlBase = frame->controlBase;
        memcpy (output, & saved, sizeof saved);
        output += sizeof saved;
    }

    for (u32 i = 0; i < runtime->numDirectControls; ++i)
    {
        M3DirectControl * control = &runtime->directControls[i];
        u32 frameIndex = DirectSnapshotControlFrame (runtime, i);
        if (frameIndex == UINT32_MAX)
            return m3Err_snapshotUnsupported;
        IM3Function function = runtime->directFrames[frameIndex].function;
        if (control->bodyPc < function->wasm || control->bodyPc > function->wasmEnd ||
            control->endPc < function->wasm || control->endPc >= function->wasmEnd ||
            (control->elsePc && (control->elsePc < function->wasm || control->elsePc >= function->wasmEnd)))
            return m3Err_snapshotUnsupported;

        M3DirectSnapshotControl saved;
        M3_INIT (saved);
        saved.frameIndex = frameIndex;
        saved.bodyOffset = (u32)(control->bodyPc - function->wasm);
        saved.elseOffset = control->elsePc ? (u32)(control->elsePc - function->wasm)
                                           : M3_DIRECT_SNAPSHOT_NO_PC;
        saved.endOffset = (u32)(control->endPc - function->wasm);
        saved.stackHeight = control->stackHeight;
        saved.numParams = control->numParams;
        saved.numResults = control->numResults;
        saved.opcode = control->opcode;
        memcpy (output, & saved, sizeof saved);
        output += sizeof saved;
    }

    memcpy (output, runtime->stack, runtime->stackSize);
    output += runtime->stackSize;
    if (header.memoryBytes)
    {
        memcpy (output, m3MemData (runtime->memory.mallocated), header.memoryBytes);
        output += header.memoryBytes;
    }

    u32 moduleIndex = 0;
    for (IM3Module module = runtime->modules; module; module = module->next, ++moduleIndex)
    {
        for (u32 globalIndex = 0; globalIndex < module->numGlobals; ++globalIndex)
        {
            M3DirectSnapshotGlobal saved;
            saved.moduleIndex = moduleIndex;
            saved.globalIndex = globalIndex;
            saved.type = module->globals[globalIndex].type;
            saved.value = module->globals[globalIndex].i64Value;
            memcpy (output, & saved, sizeof saved);
            output += sizeof saved;
        }
    }
    return m3Err_none;
}

M3Result DirectLoadSnapshot (IM3Runtime runtime, const u8 * buffer, u32 bufferSize)
{
    if (not runtime || not buffer || bufferSize < sizeof (M3DirectSnapshotHeader))
        return m3Err_snapshotInvalid;

    M3DirectSnapshotHeader header;
    memcpy (& header, buffer, sizeof header);
    if (header.magic != M3_DIRECT_SNAPSHOT_MAGIC ||
        header.version != M3_DIRECT_SNAPSHOT_VERSION ||
        header.headerSize != sizeof header ||
        header.flags != M3_DIRECT_SNAPSHOT_FLAGS ||
        header.totalSize > bufferSize ||
        header.stackSize != runtime->stackSize ||
        header.valueTop > DirectValueCapacity (runtime) ||
        header.frameCount == 0 || header.controlCount == 0 ||
        header.frameCount > DirectValueCapacity (runtime) ||
        header.controlCount > DirectValueCapacity (runtime) ||
        header.moduleCount != DirectSnapshotModuleCount (runtime) ||
        header.globalCount != DirectSnapshotGlobalCount (runtime) ||
        header.memoryPageSize != runtime->memory.pageSize)
        return m3Err_snapshotInvalid;

    u64 expected = sizeof header;
    expected += (u64)header.frameCount * sizeof (M3DirectSnapshotFrame);
    expected += (u64)header.controlCount * sizeof (M3DirectSnapshotControl);
    expected += header.stackSize;
    expected += header.memoryBytes;
    expected += (u64)header.globalCount * sizeof (M3DirectSnapshotGlobal);
    if (expected != header.totalSize)
        return m3Err_snapshotInvalid;

    IM3Function entry = DirectSnapshotGetFunction (runtime, header.entryModuleIndex,
                                                    header.entryFunctionIndex);
    if (not entry)
        return m3Err_snapshotInvalid;

    M3Result result = DirectEnsureFrames (runtime, header.frameCount);
    if (result)
        return result;
    result = DirectEnsureControls (runtime, header.controlCount);
    if (result)
        return result;
    result = ResizeMemory (runtime, header.memoryPages);
    if (result ||
        (runtime->memory.mallocated ? (u32)runtime->memory.mallocated->length : 0) != header.memoryBytes)
        return m3Err_snapshotInvalid;

    DirectReset (runtime);
    const u8 * input = buffer + sizeof header;
    u32 previousControlBase = 0;
    for (u32 i = 0; i < header.frameCount; ++i)
    {
        M3DirectSnapshotFrame saved;
        memcpy (& saved, input, sizeof saved);
        input += sizeof saved;
        IM3Function function = DirectSnapshotGetFunction (runtime, saved.moduleIndex, saved.functionIndex);
        if (not function || not function->wasm ||
            saved.pcOffset > (u32)(function->wasmEnd - function->wasm) ||
            saved.localBase > header.valueTop || saved.stackBase > header.valueTop ||
            saved.controlBase >= header.controlCount ||
            (i && saved.controlBase <= previousControlBase))
        {
            DirectReset (runtime);
            return m3Err_snapshotInvalid;
        }

        bytes_t code = NULL;
        u32 numLocals = 0;
        result = DirectParseFunctionBody (function, & code, & numLocals);
        if (result || numLocals > UINT16_MAX)
        {
            DirectReset (runtime);
            return m3Err_snapshotInvalid;
        }
        function->numLocals = (u16)numLocals;

        M3DirectFrame * frame = &runtime->directFrames[i];
        frame->function = function;
        frame->pc = function->wasm + saved.pcOffset;
        frame->end = function->wasmEnd;
        frame->localBase = saved.localBase;
        frame->stackBase = saved.stackBase;
        frame->controlBase = saved.controlBase;
        previousControlBase = saved.controlBase;
    }

    runtime->numDirectFrames = header.frameCount;
    runtime->numDirectControls = header.controlCount;
    for (u32 i = 0; i < header.controlCount; ++i)
    {
        M3DirectSnapshotControl saved;
        memcpy (& saved, input, sizeof saved);
        input += sizeof saved;
        if (saved.frameIndex >= header.frameCount || saved.stackHeight > header.valueTop)
        {
            DirectReset (runtime);
            return m3Err_snapshotInvalid;
        }
        IM3Function function = runtime->directFrames[saved.frameIndex].function;
        u32 wasmSize = (u32)(function->wasmEnd - function->wasm);
        if (saved.bodyOffset > wasmSize || saved.endOffset >= wasmSize ||
            (saved.elseOffset != M3_DIRECT_SNAPSHOT_NO_PC && saved.elseOffset >= wasmSize) ||
            DirectSnapshotControlFrame (runtime, i) != saved.frameIndex)
        {
            DirectReset (runtime);
            return m3Err_snapshotInvalid;
        }

        M3DirectControl * control = &runtime->directControls[i];
        control->bodyPc = function->wasm + saved.bodyOffset;
        control->elsePc = saved.elseOffset == M3_DIRECT_SNAPSHOT_NO_PC
                        ? NULL : function->wasm + saved.elseOffset;
        control->endPc = function->wasm + saved.endOffset;
        control->stackHeight = saved.stackHeight;
        control->numParams = saved.numParams;
        control->numResults = saved.numResults;
        control->opcode = saved.opcode;
    }

    memcpy (runtime->stack, input, runtime->stackSize);
    input += runtime->stackSize;
    if (header.memoryBytes)
    {
        memcpy (m3MemData (runtime->memory.mallocated), input, header.memoryBytes);
        input += header.memoryBytes;
    }

    for (u32 i = 0; i < header.globalCount; ++i)
    {
        M3DirectSnapshotGlobal saved;
        memcpy (& saved, input, sizeof saved);
        input += sizeof saved;
        IM3Module module = DirectSnapshotGetModule (runtime, saved.moduleIndex);
        if (not module || saved.globalIndex >= module->numGlobals ||
            module->globals[saved.globalIndex].type != saved.type)
        {
            DirectReset (runtime);
            return m3Err_snapshotInvalid;
        }
        module->globals[saved.globalIndex].i64Value = saved.value;
    }

    runtime->numDirectFrames = header.frameCount;
    runtime->numDirectControls = header.controlCount;
    runtime->directValueTop = header.valueTop;
    runtime->directEntry = entry;
    runtime->directActive = true;
    runtime->fuel = header.fuel;
    runtime->fuelEnabled = header.fuelEnabled != 0;
    runtime->suspended = true;
    runtime->suspendedFunction = entry;
    runtime->lastCalled = NULL;
    return m3Err_none;
}
