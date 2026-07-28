#include "wasm3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestVm
{
    IM3Environment environment;
    IM3Runtime runtime;
    IM3Module module;
    IM3Function function;
    unsigned char * wasm;
}
TestVm;

static int failures;

// A module whose start function increments memory three times before the
// exported get() function reads it. The custom name section is omitted.
static const unsigned char startModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x00,
    0x01, 0x7f, 0x03, 0x04, 0x03, 0x00, 0x01, 0x00,
    0x05, 0x04, 0x01, 0x01, 0x01, 0x01, 0x07, 0x0d,
    0x02, 0x03, 0x69, 0x6e, 0x63, 0x00, 0x00, 0x03,
    0x67, 0x65, 0x74, 0x00, 0x01, 0x08, 0x01, 0x02,
    0x0a, 0x23, 0x03, 0x0f, 0x00, 0x41, 0x00, 0x41,
    0x00, 0x2d, 0x00, 0x00, 0x41, 0x01, 0x6a, 0x3a,
    0x00, 0x00, 0x0b, 0x08, 0x00, 0x41, 0x00, 0x2d,
    0x00, 0x00, 0x0f, 0x0b, 0x08, 0x00, 0x10, 0x00,
    0x10, 0x00, 0x10, 0x00, 0x0b, 0x0b, 0x07, 0x01,
    0x00, 0x41, 0x00, 0x0b, 0x01, 0x41
};

// An exported function that calls env.host() -> i32 and then returns.
static const unsigned char importModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x02, 0x0c, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x04,
    0x68, 0x6f, 0x73, 0x74, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x0d, 0x01, 0x09, 0x63, 0x61, 0x6c, 0x6c,
    0x5f, 0x68, 0x6f, 0x73, 0x74, 0x00, 0x01,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b
};

#define CHECK(CONDITION, MESSAGE)                                      \
    do                                                                 \
    {                                                                  \
        if (CONDITION) printf ("PASS %s\n", MESSAGE);                  \
        else { printf ("FAIL %s\n", MESSAGE); ++failures; }            \
    } while (0)

static int hostCalls;

static const void * HostValue (IM3Runtime runtime, IM3ImportContext context,
                               uint64_t * stack, void * memory)
{
    (void)runtime;
    (void)context;
    (void)memory;
    ++hostCalls;
    *(int32_t *)stack = 42;
    return m3Err_none;
}

static int InitializeModule (TestVm * vm, uint32_t size)
{
    vm->environment = m3_NewEnvironment ();
    vm->runtime = m3_NewRuntime (vm->environment, 64 * 1024, NULL);
    IM3Module module = NULL;
    M3Result result = m3_ParseModule (vm->environment, & module, vm->wasm, size);
    if (!result)
        result = m3_LoadModule (vm->runtime, module);
    if (!result)
        vm->module = module;
    return result == m3Err_none;
}

static int InitializeVm (TestVm * vm, uint32_t size, const char * functionName)
{
    return InitializeModule (vm, size) &&
           m3_FindFunction (& vm->function, vm->runtime, functionName) == m3Err_none;
}

static int LoadVm (TestVm * vm, const char * path, const char * functionName)
{
    memset (vm, 0, sizeof *vm);
    FILE * file = fopen (path, "rb");
    if (!file)
        return 0;

    fseek (file, 0, SEEK_END);
    long size = ftell (file);
    rewind (file);
    if (size <= 0)
    {
        fclose (file);
        return 0;
    }

    vm->wasm = (unsigned char *)malloc ((size_t)size);
    if (!vm->wasm || fread (vm->wasm, 1, (size_t)size, file) != (size_t)size)
    {
        fclose (file);
        free (vm->wasm);
        vm->wasm = NULL;
        return 0;
    }
    fclose (file);

    return InitializeVm (vm, (uint32_t)size, functionName);
}

static int LoadVmBytes (TestVm * vm, const unsigned char * wasm, uint32_t size,
                        const char * functionName)
{
    memset (vm, 0, sizeof *vm);
    vm->wasm = (unsigned char *)malloc (size);
    if (!vm->wasm)
        return 0;
    memcpy (vm->wasm, wasm, size);
    return InitializeVm (vm, size, functionName);
}

static int LoadImportVm (TestVm * vm)
{
    memset (vm, 0, sizeof *vm);
    vm->wasm = (unsigned char *)malloc (sizeof importModule);
    if (!vm->wasm)
        return 0;
    memcpy (vm->wasm, importModule, sizeof importModule);
    if (!InitializeModule (vm, sizeof importModule))
        return 0;
    if (m3_LinkRawFunction (vm->module, "env", "host", "i()", HostValue) !=
        m3Err_none)
        return 0;
    return m3_FindFunction (& vm->function, vm->runtime, "call_host") == m3Err_none;
}

static void FreeVm (TestVm * vm)
{
    m3_FreeRuntime (vm->runtime);
    m3_FreeEnvironment (vm->environment);
    free (vm->wasm);
    memset (vm, 0, sizeof *vm);
}

static M3Result StartFib (TestVm * vm)
{
    int32_t input = 10;
    const void * arguments[] = { & input };
    return m3_Start (vm->function, 1, arguments);
}

static void CheckResult (TestVm * vm)
{
    int32_t output = 0;
    M3Result result = m3_GetResultsV (vm->function, & output);
    CHECK (result == m3Err_none, "read result");
    CHECK (output == 55, "fib(10) is 55");
}

int main (int argc, char ** argv)
{
    if (argc != 2)
    {
        fprintf (stderr, "usage: %s test/lang/fib32.wasm\n", argv[0]);
        return 2;
    }

    TestVm stepVm;
    CHECK (LoadVm (& stepVm, argv[1], "fib"), "load step VM");
    CHECK (StartFib (& stepVm) == m3Err_none, "start without executing");

    uint64_t instructionCount = 0;
    int everyStepSuspended = 1;
    M3Result result;
    do
    {
        result = m3_Step (stepVm.runtime);
        ++instructionCount;
        if (result == m3Err_fuelExhausted && !m3_IsSuspended (stepVm.runtime))
            everyStepSuspended = 0;
    }
    while (result == m3Err_fuelExhausted);
    CHECK (everyStepSuspended, "each incomplete step leaves the call suspended");
    CHECK (result == m3Err_none, "step execution completes");
    CHECK (instructionCount > 1, "step counted instructions");
    CheckResult (& stepVm);
    FreeVm (& stepVm);

    TestVm splitVm;
    CHECK (LoadVm (& splitVm, argv[1], "fib"), "load split execute VM");
    CHECK (StartFib (& splitVm) == m3Err_none, "start split execute");
    uint64_t consumed = 0;
    result = m3_Execute (splitVm.runtime, instructionCount - 1, & consumed);
    CHECK (result == m3Err_fuelExhausted, "N-1 fuel suspends");
    CHECK (consumed == instructionCount - 1, "N-1 instructions consumed exactly");
    int32_t replacementInput = 1;
    const void * replacementArguments[] = { & replacementInput };
    CHECK (m3_Start (splitVm.function, 1, replacementArguments) == m3Err_runtimeSuspended,
           "start rejects an active suspended call");
    result = m3_Execute (splitVm.runtime, 1, & consumed);
    CHECK (result == m3Err_none, "one final fuel completes");
    CHECK (consumed == 1, "final execute consumes one instruction");
    CheckResult (& splitVm);
    FreeVm (& splitVm);

    TestVm executeVm;
    CHECK (LoadVm (& executeVm, argv[1], "fib"), "load execute VM");
    CHECK (StartFib (& executeVm) == m3Err_none, "start exact execute");
    result = m3_Execute (executeVm.runtime, instructionCount, & consumed);
    CHECK (result == m3Err_none, "N fuel completes");
    CHECK (consumed == instructionCount, "N fuel equals N instructions");
    CheckResult (& executeVm);
    FreeVm (& executeVm);

    TestVm runVm;
    CHECK (LoadVm (& runVm, argv[1], "fib"), "load run VM");
    CHECK (StartFib (& runVm) == m3Err_none, "start unlimited run");
    CHECK (m3_Run (runVm.runtime) == m3Err_none, "unlimited run completes");
    CHECK (!m3_IsFuelEnabled (runVm.runtime), "run disables fuel");
    CheckResult (& runVm);
    FreeVm (& runVm);

    TestVm zeroVm;
    CHECK (LoadVm (& zeroVm, argv[1], "fib"), "load zero-fuel VM");
    CHECK (StartFib (& zeroVm) == m3Err_none, "start zero-fuel execute");
    result = m3_Execute (zeroVm.runtime, 0, & consumed);
    CHECK (result == m3Err_fuelExhausted, "zero fuel suspends without execution");
    CHECK (consumed == 0, "zero fuel consumes no instructions");
    result = m3_Execute (zeroVm.runtime, instructionCount, & consumed);
    CHECK (result == m3Err_none, "execute continues after zero-fuel suspension");
    CheckResult (& zeroVm);
    FreeVm (& zeroVm);

    TestVm startVm;
    CHECK (LoadVmBytes (& startVm, startModule, sizeof startModule, "get"),
           "load module with start function");
    CHECK (m3_Start (startVm.function, 0, NULL) == m3Err_none,
           "start entry with pending module initializer");
    uint64_t startInstructions = 0;
    do
    {
        result = m3_Execute (startVm.runtime, 1, & consumed);
        startInstructions += consumed;
    }
    while (result == m3Err_fuelExhausted);
    CHECK (result == m3Err_none, "resume through module start into requested entry");
    CHECK (startInstructions > 1, "module start and entry are instruction-counted");
    int32_t startOutput = 0;
    CHECK (m3_GetResultsV (startVm.function, & startOutput) == m3Err_none,
           "read result after module start");
    CHECK (startOutput == 68, "module start executes exactly once before entry");
    FreeVm (& startVm);

    TestVm runStartVm;
    CHECK (LoadVmBytes (& runStartVm, startModule, sizeof startModule, "get"),
           "load module for explicit m3_RunStart");
    m3_SetFuel (runStartVm.runtime, 1);
    result = m3_RunStart (runStartVm.module);
    while (result == m3Err_fuelExhausted)
    {
        m3_AddFuel (runStartVm.runtime, 1);
        result = m3_Resume (runStartVm.runtime);
    }
    CHECK (result == m3Err_none, "resume an explicit module start with fuel");
    CHECK (m3_Start (runStartVm.function, 0, NULL) == m3Err_none,
           "start entry after explicit module start");
    CHECK (m3_Run (runStartVm.runtime) == m3Err_none,
           "run entry after explicit module start");
    startOutput = 0;
    CHECK (m3_GetResultsV (runStartVm.function, & startOutput) == m3Err_none,
           "read result after explicit module start");
    CHECK (startOutput == 68, "explicit module start is not repeated after resume");
    FreeVm (& runStartVm);

    TestVm importVm;
    int importReady = LoadImportVm (& importVm);
    CHECK (importReady, "load and link raw-import caller");
    if (importReady)
    {
        hostCalls = 0;
        CHECK (m3_Start (importVm.function, 0, NULL) == m3Err_none,
               "start raw-import caller");
        result = m3_Execute (importVm.runtime, 1, & consumed);
        CHECK (result == m3Err_fuelExhausted, "raw import call costs one fuel");
        CHECK (consumed == 1, "raw import call consumes exactly one instruction");
        CHECK (hostCalls == 1, "raw import is called exactly once");
        result = m3_Execute (importVm.runtime, 1, & consumed);
        CHECK (result == m3Err_none, "function end completes with the next fuel");
        int32_t importOutput = 0;
        CHECK (m3_GetResultsV (importVm.function, & importOutput) == m3Err_none,
               "read raw-import result");
        CHECK (importOutput == 42, "raw-import result reaches WebAssembly caller");
        CHECK (hostCalls == 1, "resuming does not repeat the raw import");
    }
    FreeVm (& importVm);

    return failures ? 1 : 0;
}
