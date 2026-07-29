//
//  m3_test.c
//
//  Created by Steven Massey on 2/27/20.
//  Copyright © 2020 Steven Massey. All rights reserved.
//

#include <stdio.h>

#include "wasm3_ext.h"
#include "m3_bind.h"

#define Test(NAME) if (RunTest (argc, argv, #NAME) != 0)
#define DisabledTest(NAME) printf ("\ndisabled: %s\n", #NAME); if (false)
#define expect(TEST) if (not (TEST)) { printf ("failed: (%s) on line: %d\n", #TEST, __LINE__); }


bool RunTest (int i_argc, const char * i_argv [], cstr_t i_name)
{
	cstr_t option = (i_argc == 2) ? i_argv [1] : NULL;
	
	bool runningTest = option ? strcmp (option, i_name) == 0 : true;
	
	if (runningTest)
		printf ("\n    test: %s\n", i_name);
	
	return runningTest;
}


int  main  (int argc, const char  * argv [])
{
    Test (signatures)
    {
        M3Result result;
        
        IM3FuncType ftype = NULL;
        
        result = SignatureToFuncType (& ftype, "");                     expect (result == m3Err_malformedFunctionSignature)
        m3_Free (ftype);
        
          // implicit void return
        result = SignatureToFuncType (& ftype, "()");                   expect (result == m3Err_none)
        m3_Free (ftype);

        result = SignatureToFuncType (& ftype, " v () ");               expect (result == m3Err_none)
                                                                        expect (ftype->numRets == 0)
                                                                        expect (ftype->numArgs == 0)
        m3_Free (ftype);

        result = SignatureToFuncType (& ftype, "f(IiF)");               expect (result == m3Err_none)
                                                                        expect (ftype->numRets == 1)
                                                                        expect (ftype->types [0] == c_m3Type_f32)
                                                                        expect (ftype->numArgs == 3)
                                                                        expect (ftype->types [1] == c_m3Type_i64)
                                                                        expect (ftype->types [2] == c_m3Type_i32)
                                                                        expect (ftype->types [3] == c_m3Type_f64)
        
        IM3FuncType ftype2 = NULL;
        result = SignatureToFuncType (& ftype2, "f(I i F)");            expect (result == m3Err_none);
                                                                        expect (AreFuncTypesEqual (ftype, ftype2));
        m3_Free (ftype);
        m3_Free (ftype2);
    }
    Test (extensions)
    {
        M3Result result;
        
        IM3Environment env = m3_NewEnvironment ();

        IM3Runtime runtime = m3_NewRuntime (env, 1024, NULL);

        IM3Module module = m3_NewModule (env);

        
        i32 functionIndex = -1;
        
        u8 wasm [5] = { 0x04,       // size
                        0x00,       // num local defs
                        0x41, 0x37, // i32.const= 55
                        0x0b        // end block
        };
        
        // Validation requires the module to be attached to a runtime.
        result = m3_InjectFunction (module, & functionIndex, "i()", wasm, true);        expect (result != m3Err_none)
                                                                                        expect (functionIndex >= 0)

        result = m3_LoadModule (runtime, module);                                       expect (result == m3Err_none)

        // try again
        result = m3_InjectFunction (module, & functionIndex, "i()", wasm, true);        expect (result == m3Err_none)

        IM3Function function = m3_GetFunctionByIndex (module, functionIndex);           expect (function)
        
        if (function)
        {
            result = m3_CallV (function);                                               expect (result == m3Err_none)
            u32 ret = 0;
            m3_GetResultsV (function, & ret);                                           expect (ret == 55);
        }
        
        m3_FreeRuntime (runtime);
    }
    
	IM3Environment env = m3_NewEnvironment ();


	Test (multireturn.a)
	{
		M3Result result;

        IM3Runtime runtime = m3_NewRuntime (env, 1024, NULL);

#		if 0
		(module
			(func (result i32 f32)

				i32.const 1234
				f32.const 5678.9
			)

			(export "main" (func 0))
		)
#		endif
		
		u8 wasm [44] = {
		  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x00, 0x02, 0x7f, 0x7d, 0x03, 0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04,
		  0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x0a, 0x0c, 0x01, 0x0a, 0x00, 0x41, 0xd2, 0x09, 0x43, 0x33, 0x77, 0xb1, 0x45, 0x0b
		};
		  
		IM3Module module;
		result = m3_ParseModule  (env, & module, wasm, 44);		     				    expect (result == m3Err_none)
	
		result = m3_LoadModule (runtime, module);                                       expect (result == m3Err_none)

		IM3Function function = NULL;
		result = m3_FindFunction (& function, runtime, "main");							expect (result == m3Err_none)
																						expect (function)
		printf ("\n%s\n", result);

		if (function)
		{
			result = m3_CallV (function);                                               expect (result == m3Err_none)

			i32 ret0 = 0;
			f32 ret1 = 0.;
			m3_GetResultsV (function, & ret0, & ret1);
			
			printf ("%d %f\n", ret0, ret1);
		}
	}

		
	Test (multireturn.branch)
	{
#			if 0
			(module
			  (func (param i32) (result i32 i32)

				i32.const 123
				i32.const 456
				i32.const 789

				block (param i32 i32) (result i32 i32 i32)

					local.get 0
					local.get 0

					local.get 0
					br_if 0

					drop

				end

				drop
				drop
			  )

			(export "main" (func 0))
			)
#			endif
	}
    
    return 0;
}
