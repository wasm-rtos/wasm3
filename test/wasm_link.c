#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm3.h"

/* Generated from test/wasm_link_app.wat. */
static const uint8_t c_app_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x02, 0x18, 0x01, 0x05, 0x6d, 0x79, 0x6c,
    0x69, 0x62, 0x0e, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x5f, 0x62,
    0x75, 0x66, 0x66, 0x65, 0x72, 0x00, 0x00, 0x03, 0x03, 0x02, 0x00, 0x00,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x01, 0x05, 0x04, 0x01, 0x01, 0x01, 0x02,
    0x07, 0x2a, 0x04, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00,
    0x08, 0x72, 0x65, 0x65, 0x78, 0x70, 0x6f, 0x72, 0x74, 0x00, 0x00, 0x03,
    0x72, 0x75, 0x6e, 0x00, 0x01, 0x0c, 0x72, 0x75, 0x6e, 0x5f, 0x69, 0x6e,
    0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x00, 0x02, 0x09, 0x07, 0x01, 0x00,
    0x41, 0x00, 0x0b, 0x01, 0x00, 0x0a, 0x16, 0x02, 0x08, 0x00, 0x20, 0x00,
    0x20, 0x01, 0x10, 0x00, 0x0b, 0x0b, 0x00, 0x20, 0x00, 0x20, 0x01, 0x41,
    0x00, 0x11, 0x00, 0x00, 0x0b,
};

/* Generated from test/wasm_link_library.wat. */
static const uint8_t c_library_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x02, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02, 0x10,
    0x01, 0x03, 0x65, 0x6e, 0x76, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79,
    0x02, 0x01, 0x01, 0x02, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07, 0x24, 0x02,
    0x0e, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x5f, 0x62, 0x75, 0x66,
    0x66, 0x65, 0x72, 0x00, 0x00, 0x0f, 0x77, 0x72, 0x6f, 0x6e, 0x67, 0x5f,
    0x73, 0x69, 0x67, 0x6e, 0x61, 0x74, 0x75, 0x72, 0x65, 0x00, 0x01, 0x0a,
    0x41, 0x02, 0x3a, 0x01, 0x03, 0x7f, 0x02, 0x40, 0x03, 0x40, 0x20, 0x02,
    0x20, 0x01, 0x4f, 0x0d, 0x01, 0x20, 0x00, 0x20, 0x02, 0x6a, 0x2d, 0x00,
    0x00, 0x41, 0x01, 0x6a, 0x21, 0x04, 0x20, 0x00, 0x20, 0x02, 0x6a, 0x20,
    0x04, 0x3a, 0x00, 0x00, 0x20, 0x03, 0x20, 0x04, 0x6a, 0x21, 0x03, 0x20,
    0x02, 0x41, 0x01, 0x6a, 0x21, 0x02, 0x0c, 0x00, 0x0b, 0x0b, 0x20, 0x03,
    0x0b, 0x04, 0x00, 0x20, 0x00, 0x0b,
};

#define check(CONDITION)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(CONDITION))                                                       \
        {                                                                       \
            fprintf(stderr, "Wasm link test failed at line %d: %s\n",           \
                    __LINE__, #CONDITION);                                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

typedef struct Fixture
{
    IM3Environment environment;
    IM3Runtime runtime;
    IM3Module app;
    IM3Module library;
    IM3Function process;
    IM3Function wrong_signature;
    IM3Function run;
    IM3Function run_indirect;
    IM3Function reexport;
}
Fixture;

static IM3Module parse_module(
    IM3Environment environment,
    const uint8_t * bytes,
    uint32_t size)
{
    IM3Module module = NULL;
    if (m3_ParseModule(environment, &module, bytes, size) != m3Err_none)
        return NULL;
    return module;
}

static int load_fixture(Fixture * fixture, bool link_app)
{
    memset(fixture, 0, sizeof(*fixture));

    fixture->environment = m3_NewEnvironment();
    check(fixture->environment);

    fixture->runtime = m3_NewRuntime(fixture->environment, 4096, NULL);
    check(fixture->runtime);

    fixture->app = parse_module(
        fixture->environment, c_app_module, sizeof(c_app_module));
    check(fixture->app);
    m3_SetModuleName(fixture->app, "app");
    check(m3_LoadModule(fixture->runtime, fixture->app) == m3Err_none);

    fixture->library = parse_module(
        fixture->environment, c_library_module, sizeof(c_library_module));
    check(fixture->library);
    m3_SetModuleName(fixture->library, "mylib");
    check(m3_LoadModule(fixture->runtime, fixture->library) == m3Err_none);

    check(m3_FindFunctionInModule(
              &fixture->process, fixture->library, "process_buffer") ==
          m3Err_none);
    check(m3_FindFunctionInModule(
              &fixture->wrong_signature,
              fixture->library,
              "wrong_signature") == m3Err_none);

    if (link_app)
    {
        check(m3_LinkWasmFunction(
                  fixture->app,
                  "mylib",
                  "process_buffer",
                  fixture->process) == m3Err_none);
        check(m3_FindFunctionInModule(&fixture->run, fixture->app, "run") ==
              m3Err_none);
        check(m3_FindFunctionInModule(
                  &fixture->run_indirect,
                  fixture->app,
                  "run_indirect") == m3Err_none);
        check(m3_FindFunctionInModule(
                  &fixture->reexport,
                  fixture->app,
                  "reexport") == m3Err_none);
        check(fixture->reexport == fixture->process);
    }

    return 0;
}

static void free_fixture(Fixture * fixture)
{
    m3_FreeRuntime(fixture->runtime);
    m3_FreeEnvironment(fixture->environment);
    memset(fixture, 0, sizeof(*fixture));
}

static const void * dummy_raw(
    IM3Runtime runtime,
    IM3ImportContext context,
    uint64_t * stack,
    void * memory)
{
    (void) runtime;
    (void) context;
    (void) stack;
    (void) memory;
    return m3Err_none;
}

static int check_finished_buffer(Fixture * fixture)
{
    uint32_t memory_size = 0;
    uint8_t * memory = m3_GetMemory(fixture->runtime, &memory_size, 0);
    check(memory && memory_size >= 96);

    for (uint32_t i = 0; i < 64; ++i)
        check(memory[32 + i] == (uint8_t)(i + 2));

    uint32_t sum = 0;
    check(m3_GetResultsV(fixture->run, &sum) == m3Err_none);
    check(sum == 2144);
    return 0;
}

static int test_link_validation_and_call(void)
{
    Fixture fixture;
    Fixture foreign;
    Fixture raw_first;
    check(load_fixture(&fixture, false) == 0);
    check(load_fixture(&foreign, true) == 0);
    check(load_fixture(&raw_first, false) == 0);

    check(m3_LinkRawFunction(
              raw_first.app,
              "mylib",
              "process_buffer",
              "i(ii)",
              dummy_raw) == m3Err_none);
    check(m3_LinkWasmFunction(
              raw_first.app,
              "mylib",
              "process_buffer",
              raw_first.process) == m3Err_functionAlreadyLinked);

    check(m3_LinkWasmFunction(
              fixture.app,
              "mylib",
              "missing",
              fixture.process) == m3Err_functionLookupFailed);
    check(m3_LinkWasmFunction(
              fixture.app,
              "mylib",
              "process_buffer",
              fixture.wrong_signature) == m3Err_functionTypeMismatch);
    check(m3_LinkWasmFunction(
              fixture.app,
              "mylib",
              "process_buffer",
              foreign.process) == m3Err_functionRuntimeMismatch);
    check(m3_LinkWasmFunction(
              fixture.app,
              "mylib",
              "process_buffer",
              fixture.process) == m3Err_none);
    check(m3_LinkWasmFunction(
              fixture.app,
              "mylib",
              "process_buffer",
              fixture.process) == m3Err_none);
    check(m3_LinkRawFunction(
              fixture.app,
              "mylib",
              "process_buffer",
              "i(ii)",
              dummy_raw) == m3Err_functionAlreadyLinked);

    check(m3_FindFunctionInModule(&fixture.run, fixture.app, "run") ==
          m3Err_none);
    check(m3_FindFunctionInModule(
              &fixture.run_indirect,
              fixture.app,
              "run_indirect") == m3Err_none);
    check(m3_FindFunctionInModule(
              &fixture.reexport,
              fixture.app,
              "reexport") == m3Err_none);
    check(fixture.reexport == fixture.process);

    uint32_t memory_size = 0;
    uint8_t * memory = m3_GetMemory(fixture.runtime, &memory_size, 0);
    check(memory && memory_size >= 132);
    memory[128] = 1;
    memory[129] = 2;
    memory[130] = 3;
    memory[131] = 4;

    check(m3_CallV(fixture.run, 128, 4) == m3Err_none);

    uint32_t sum = 0;
    check(m3_GetResultsV(fixture.run, &sum) == m3Err_none);
    check(sum == 14);
    check(memory[128] == 2);
    check(memory[129] == 3);
    check(memory[130] == 4);
    check(memory[131] == 5);

    memory[136] = 5;
    memory[137] = 6;
    memory[138] = 7;
    memory[139] = 8;
    check(m3_CallV(fixture.run_indirect, 136, 4) == m3Err_none);
    check(m3_GetResultsV(fixture.run_indirect, &sum) == m3Err_none);
    check(sum == 30);
    check(memory[136] == 6);
    check(memory[137] == 7);
    check(memory[138] == 8);
    check(memory[139] == 9);

    memory[144] = 9;
    check(m3_CallV(fixture.reexport, 144, 1) == m3Err_none);
    check(m3_GetResultsV(fixture.reexport, &sum) == m3Err_none);
    check(sum == 10);
    check(memory[144] == 10);

    free_fixture(&raw_first);
    free_fixture(&foreign);
    free_fixture(&fixture);
    return 0;
}

static int test_fuel_snapshot_and_resume(void)
{
    Fixture original;
    Fixture restored;
    check(load_fixture(&original, true) == 0);

    uint32_t memory_size = 0;
    uint8_t * memory = m3_GetMemory(original.runtime, &memory_size, 0);
    check(memory && memory_size >= 96);
    for (uint32_t i = 0; i < 64; ++i)
        memory[32 + i] = (uint8_t)(i + 1);

    m3_SetFuel(original.runtime, 40);
    check(m3_CallV(original.run, 32, 64) == m3Err_fuelExhausted);
    check(m3_IsSuspended(original.runtime));

    uint32_t changed = 0;
    for (uint32_t i = 0; i < 64; ++i)
    {
        if (memory[32 + i] == (uint8_t)(i + 2))
            ++changed;
        else
            check(memory[32 + i] == (uint8_t)(i + 1));
    }
    check(changed > 0 && changed < 64);

    uint8_t mid_call_memory[64];
    memcpy(mid_call_memory, memory + 32, sizeof(mid_call_memory));

    uint32_t snapshot_size = 0;
    check(m3_GetRuntimeSnapshotSize(original.runtime, &snapshot_size) ==
          m3Err_none);
    uint8_t * snapshot = (uint8_t *)malloc(snapshot_size);
    check(snapshot);
    check(m3_SaveRuntimeSnapshot(
              original.runtime,
              snapshot,
              snapshot_size,
              &snapshot_size) == m3Err_none);

    m3_AddFuel(original.runtime, 100000);
    check(m3_Resume(original.runtime) == m3Err_none);
    check(check_finished_buffer(&original) == 0);

    check(load_fixture(&restored, true) == 0);
    check(m3_LoadRuntimeSnapshot(
              restored.runtime, snapshot, snapshot_size) == m3Err_none);

    memory = m3_GetMemory(restored.runtime, &memory_size, 0);
    check(memory && memory_size >= 96);
    check(memcmp(memory + 32, mid_call_memory, sizeof(mid_call_memory)) == 0);

    m3_AddFuel(restored.runtime, 100000);
    check(m3_Resume(restored.runtime) == m3Err_none);
    check(check_finished_buffer(&restored) == 0);

    free(snapshot);
    free_fixture(&restored);
    free_fixture(&original);
    return 0;
}

int main(void)
{
    check(test_link_validation_and_call() == 0);
    check(test_fuel_snapshot_and_resume() == 0);
    puts("Wasm link test passed");
    return 0;
}
