//
//  m3_info.h
//
//  Created by Steven Massey on 12/6/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#ifndef m3_info_h
#define m3_info_h

#include "m3_function.h"

d_m3BeginExternC

#ifdef DEBUG

cstr_t          SPrintFuncTypeSignature (IM3FuncType i_funcType);

#else // DEBUG

#define         SPrintFuncTypeSignature(...) ""

#endif // DEBUG

d_m3EndExternC

#endif // m3_info_h
