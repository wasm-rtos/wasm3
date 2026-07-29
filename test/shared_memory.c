#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm3.h"

/* Generated from test/shared_memory_owner.wat. */
static const uint8_t c_owner_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0b, 0x02, 0x60,
    0x02, 0x7f, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x03, 0x03, 0x02,
    0x00, 0x01, 0x05, 0x04, 0x01, 0x01, 0x01, 0x03, 0x07, 0x19, 0x03, 0x06,
    0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x05, 0x77, 0x72, 0x69,
    0x74, 0x65, 0x00, 0x00, 0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x01, 0x0a,
    0x13, 0x02, 0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0x36, 0x02, 0x00, 0x0b,
    0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
};

/* Generated from test/shared_memory_borrower.wat. */
static const uint8_t c_borrower_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x03, 0x60,
    0x00, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01,
    0x7f, 0x02, 0x22, 0x02, 0x03, 0x65, 0x6e, 0x76, 0x06, 0x6d, 0x65, 0x6d,
    0x6f, 0x72, 0x79, 0x02, 0x01, 0x01, 0x03, 0x04, 0x68, 0x6f, 0x73, 0x74,
    0x0a, 0x72, 0x75, 0x6e, 0x74, 0x69, 0x6d, 0x65, 0x5f, 0x69, 0x64, 0x00,
    0x00, 0x03, 0x06, 0x05, 0x01, 0x02, 0x02, 0x00, 0x00, 0x07, 0x2b, 0x05,
    0x05, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x01, 0x04, 0x72, 0x65, 0x61,
    0x64, 0x00, 0x02, 0x04, 0x67, 0x72, 0x6f, 0x77, 0x00, 0x03, 0x04, 0x73,
    0x69, 0x7a, 0x65, 0x00, 0x04, 0x0a, 0x72, 0x75, 0x6e, 0x74, 0x69, 0x6d,
    0x65, 0x5f, 0x69, 0x64, 0x00, 0x05, 0x0a, 0x24, 0x05, 0x09, 0x00, 0x20,
    0x00, 0x20, 0x01, 0x36, 0x02, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x28,
    0x02, 0x00, 0x0b, 0x06, 0x00, 0x20, 0x00, 0x40, 0x00, 0x0b, 0x04, 0x00,
    0x3f, 0x00, 0x0b, 0x04, 0x00, 0x10, 0x00, 0x0b, 0x0b, 0x0e, 0x01, 0x00,
    0x41, 0x80, 0x08, 0x0b, 0x07, 0x6c, 0x69, 0x62, 0x72, 0x61, 0x72, 0x79,
};

/* Generated from test/shared_memory_incompatible.wat. */
static const uint8_t c_incompatible_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x01, 0x03,
    0x65, 0x6e, 0x76, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x01,
    0x04, 0x04,
};

#define check(CONDITION)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(CONDITION))                                                       \
        {                                                                       \
            fprintf(stderr, "shared memory test failed at line %d: %s\n",       \
                    __LINE__, #CONDITION);                                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const void * runtime_id(
    IM3Runtime runtime,
    IM3ImportContext context,
    uint64_t * stack,
    void * memory)
{
    (void) context;
    (void) memory;
    stack[0] = (uint32_t) (uintptr_t) m3_GetUserData(runtime);
    return m3Err_none;
}

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

int main(void)
{
    IM3Environment environment = m3_NewEnvironment();
    check(environment);

    IM3Runtime owner = m3_NewRuntime(
        environment, 2048, (void *) (uintptr_t) 11);
    IM3Runtime borrower = m3_NewRuntime(
        environment, 1024, (void *) (uintptr_t) 22);
    IM3Runtime empty_source = m3_NewRuntime(environment, 1024, NULL);
    check(owner && borrower && empty_source);

    check(m3_ShareRuntimeMemory(borrower, empty_source) ==
          m3Err_sharedMemoryUnavailable);

    IM3Module owner_module = parse_module(
        environment, c_owner_module, sizeof(c_owner_module));
    check(owner_module);
    check(m3_LoadModule(owner, owner_module) == m3Err_none);

    check(m3_ShareRuntimeMemory(borrower, owner) == m3Err_none);
    check(m3_ShareRuntimeMemory(borrower, owner) == m3Err_none);

    IM3Module borrower_module = parse_module(
        environment, c_borrower_module, sizeof(c_borrower_module));
    check(borrower_module);
    check(m3_LoadModule(borrower, borrower_module) == m3Err_none);
    M3Result link_result = m3_LinkRawFunction(
        borrower_module, "host", "runtime_id", "i()", runtime_id);
    if (link_result)
        fprintf(stderr, "link failed: %s\n", link_result);
    check(link_result == m3Err_none);

    uint32_t owner_size = 0;
    uint32_t borrower_size = 0;
    uint8_t * owner_memory = m3_GetMemory(owner, &owner_size, 0);
    uint8_t * borrower_memory = m3_GetMemory(borrower, &borrower_size, 0);
    check(owner_memory && owner_memory == borrower_memory);
    check(owner_size == 65536 && borrower_size == owner_size);
    check(memcmp(owner_memory + 1024, "library", 7) == 0);

    IM3Function owner_write = NULL;
    IM3Function owner_read = NULL;
    IM3Function borrower_write = NULL;
    IM3Function borrower_read = NULL;
    IM3Function borrower_grow = NULL;
    IM3Function borrower_size_function = NULL;
    IM3Function borrower_runtime_id = NULL;
    check(m3_FindFunction(&owner_write, owner, "write") == m3Err_none);
    check(m3_FindFunction(&owner_read, owner, "read") == m3Err_none);
    check(m3_FindFunction(&borrower_write, borrower, "write") == m3Err_none);
    check(m3_FindFunction(&borrower_read, borrower, "read") == m3Err_none);
    check(m3_FindFunction(&borrower_grow, borrower, "grow") == m3Err_none);
    check(m3_FindFunction(
        &borrower_size_function, borrower, "size") == m3Err_none);
    check(m3_FindFunction(
        &borrower_runtime_id, borrower, "runtime_id") == m3Err_none);

    uint32_t value = 0;
    check(m3_CallV(owner_write, 64, 0x12345678) == m3Err_none);
    check(m3_CallV(borrower_read, 64) == m3Err_none);
    check(m3_GetResultsV(borrower_read, &value) == m3Err_none);
    check(value == 0x12345678);

    check(m3_CallV(borrower_write, 68, 0x55667788) == m3Err_none);
    check(m3_CallV(owner_read, 68) == m3Err_none);
    check(m3_GetResultsV(owner_read, &value) == m3Err_none);
    check(value == 0x55667788);

    check(m3_CallV(borrower_runtime_id) == m3Err_none);
    check(m3_GetResultsV(borrower_runtime_id, &value) == m3Err_none);
    check(value == 22);

    m3_SetFuel(owner, 0);
    check(m3_CallV(owner_write, 72, 0x11223344) == m3Err_fuelExhausted);
    check(m3_IsSuspended(owner));

    uint32_t snapshot_size = 0;
    check(m3_GetRuntimeSnapshotSize(owner, &snapshot_size) ==
          m3Err_snapshotUnsupported);
    uint8_t snapshot_buffer[512] = {0};
    check(m3_SaveRuntimeSnapshot(
              owner, snapshot_buffer, sizeof(snapshot_buffer), &snapshot_size) ==
          m3Err_snapshotUnsupported);
    check(m3_LoadRuntimeSnapshot(
              owner, snapshot_buffer, sizeof(snapshot_buffer)) ==
          m3Err_snapshotUnsupported);

    check(m3_CallV(borrower_grow, 1) == m3Err_none);
    check(m3_GetResultsV(borrower_grow, &value) == m3Err_none);
    check(value == 1);

    owner_memory = m3_GetMemory(owner, &owner_size, 0);
    borrower_memory = m3_GetMemory(borrower, &borrower_size, 0);
    check(owner_memory && owner_memory == borrower_memory);
    check(owner_size == 2 * 65536 && borrower_size == owner_size);
    check(memcmp(owner_memory + 1024, "library", 7) == 0);

    m3_AddFuel(owner, 1000);
    check(m3_Resume(owner) == m3Err_none);
    check(m3_CallV(borrower_read, 72) == m3Err_none);
    check(m3_GetResultsV(borrower_read, &value) == m3Err_none);
    check(value == 0x11223344);

    check(m3_CallV(borrower_size_function) == m3Err_none);
    check(m3_GetResultsV(borrower_size_function, &value) == m3Err_none);
    check(value == 2);

    IM3Runtime occupied = m3_NewRuntime(environment, 1024, NULL);
    check(occupied);
    IM3Module occupied_module = parse_module(
        environment, c_owner_module, sizeof(c_owner_module));
    check(occupied_module);
    check(m3_LoadModule(occupied, occupied_module) == m3Err_none);
    check(m3_ShareRuntimeMemory(occupied, owner) == m3Err_sharedMemoryInUse);

    IM3Runtime defined_target = m3_NewRuntime(environment, 1024, NULL);
    check(defined_target);
    check(m3_ShareRuntimeMemory(defined_target, owner) == m3Err_none);
    IM3Module defined_module = parse_module(
        environment, c_owner_module, sizeof(c_owner_module));
    check(defined_module);
    check(m3_LoadModule(defined_target, defined_module) ==
          m3Err_sharedMemoryIncompatible);
    m3_FreeModule(defined_module);
    m3_FreeRuntime(defined_target);

    IM3Runtime incompatible_target = m3_NewRuntime(environment, 1024, NULL);
    check(incompatible_target);
    check(m3_ShareRuntimeMemory(incompatible_target, owner) == m3Err_none);
    IM3Module incompatible_module = parse_module(
        environment, c_incompatible_module, sizeof(c_incompatible_module));
    check(incompatible_module);
    check(m3_LoadModule(incompatible_target, incompatible_module) ==
          m3Err_sharedMemoryIncompatible);
    m3_FreeModule(incompatible_module);
    m3_FreeRuntime(incompatible_target);

    m3_FreeRuntime(owner);
    owner = NULL;

    check(m3_CallV(borrower_read, 64) == m3Err_none);
    check(m3_GetResultsV(borrower_read, &value) == m3Err_none);
    check(value == 0x12345678);

    check(m3_CallV(borrower_grow, 1) == m3Err_none);
    check(m3_GetResultsV(borrower_grow, &value) == m3Err_none);
    check(value == 2);
    check(m3_GetMemorySize(borrower) == 3 * 65536);

    m3_FreeRuntime(occupied);
    m3_FreeRuntime(empty_source);
    m3_FreeRuntime(borrower);
    m3_FreeEnvironment(environment);

    puts("shared memory test passed");
    return 0;
}
