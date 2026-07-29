//
//  m3_info.c
//
//  Created by Steven Massey on 4/27/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#include "m3_env.h"
#include "m3_info.h"

#ifdef DEBUG

// a central function you can be breakpoint:
void ExceptionBreakpoint (cstr_t i_exception, cstr_t i_message)
{
    printf ("\nexception: '%s' @ %s\n", i_exception, i_message);
    return;
}


void  m3_PrintM3Info  ()
{
    printf ("\n-- m3 configuration --------------------------------------------\n");
    printf (" sizeof M3MemPage     : %u bytes              \n", d_m3DefaultMemPageSize);
    printf (" executor              : direct Wasm bytecode\n");
    printf (" sizeof M3Function    : %zu bytes             \n", sizeof (M3Function));
    printf ("----------------------------------------------------------------\n\n");
}


void *  v_PrintEnvModuleInfo  (IM3Module i_module, u32 * io_index)
{
    printf (" module [%u]  name: '%s'; funcs: %d  \n", * io_index++, i_module->name, i_module->numFunctions);

    return NULL;
}


void  m3_PrintRuntimeInfo  (IM3Runtime i_runtime)
{
    printf ("\n-- m3 runtime -------------------------------------------------\n");

    printf (" stack-size: %zu   \n\n", i_runtime->numStackSlots * sizeof (m3slot_t));

    u32 moduleIndex = 0;
    ForEachModule (i_runtime, (ModuleVisitor) v_PrintEnvModuleInfo, & moduleIndex);

    printf ("----------------------------------------------------------------\n\n");
}


cstr_t  GetTypeName  (u8 i_m3Type)
{
    if (i_m3Type < 5)
        return c_waTypes [i_m3Type];
    else
        return "?";
}


// TODO: these 'static char string []' aren't thread-friendly.  though these functions are
// mainly for simple diagnostics during development, it'd be nice if they were fully reliable.

cstr_t  SPrintFuncTypeSignature  (IM3FuncType i_funcType)
{
    static char string [256];

    sprintf (string, "(");

    for (u32 i = 0; i < i_funcType->numArgs; ++i)
    {
        if (i != 0)
            strcat (string, ", ");

        strcat (string, GetTypeName (d_FuncArgType(i_funcType, i)));
    }

    strcat (string, ") -> ");

    for (u32 i = 0; i < i_funcType->numRets; ++i)
    {
        if (i != 0)
            strcat (string, ", ");

        strcat (string, GetTypeName (d_FuncRetType(i_funcType, i)));
    }

    return string;
}


#endif // DEBUG

void  m3_PrintProfilerInfo  () {}
