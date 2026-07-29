#include <stdint.h>
#include <stdio.h>

#include "wasm3.h"

static const uint8_t c_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x00,
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x41, 0x28, 0x41, 0x02, 0x6a, 0x0b,
};

#define check(CONDITION)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(CONDITION))                                                       \
        {                                                                       \
            fprintf(stderr, "fuel test failed at line %d: %s\n",               \
                    __LINE__, #CONDITION);                                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(void)
{
    IM3Environment environment = m3_NewEnvironment();
    check(environment);

    IM3Runtime runtime = m3_NewRuntime(environment, 1024, NULL);
    check(runtime);

    IM3Module module = NULL;
    M3Result result = m3_ParseModule(
        environment, &module, c_module, sizeof(c_module));
    check(result == m3Err_none);
    check(m3_LoadModule(runtime, module) == m3Err_none);

    IM3Function function = NULL;
    check(m3_FindFunction(&function, runtime, "run") == m3Err_none);
    check(function);

    m3_SetFuel(runtime, 0);
    result = m3_CallV(function);
    check(result == m3Err_fuelExhausted);
    check(m3_IsSuspended(runtime));
    check(m3_GetFuel(runtime) == 0);
    check(m3_CallV(function) == m3Err_runtimeSuspended);

    uint64_t resumes = 0;
    while (result == m3Err_fuelExhausted)
    {
        m3_AddFuel(runtime, 1);
        check(m3_GetFuel(runtime) == 1);
        result = m3_Resume(runtime);
        resumes++;
    }

    check(result == m3Err_none);
    check(!m3_IsSuspended(runtime));
    check(m3_GetFuel(runtime) == 0);
    check(resumes > 0);

    uint32_t value = 0;
    check(m3_GetResultsV(function, &value) == m3Err_none);
    check(value == 42);

    m3_SetFuel(runtime, resumes - 1);
    result = m3_CallV(function);
    check(result == m3Err_fuelExhausted);
    check(m3_GetFuel(runtime) == 0);

    m3_AddFuel(runtime, 1);
    check(m3_Resume(runtime) == m3Err_none);
    check(m3_GetFuel(runtime) == 0);

    m3_SetFuel(runtime, 0);
    check(m3_CallV(function) == m3Err_fuelExhausted);
    m3_DisableFuel(runtime);
    check(m3_Resume(runtime) == m3Err_none);

    puts("fuel test passed");

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(environment);
    return 0;
}
