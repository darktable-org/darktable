/*
** macros to wrap function registration
**
** creates an inline function definition
** using a set calling convention
** which allows for automatic calling
*/

#ifndef lautocfunc_h
#define lautocfunc_h


/* workaround for MSVC VA_ARGS expansion */
#define __VA_ARGS_APPLY__(FUNC, ...) __VA_ARGS_APPLYED__(FUNC, (__VA_ARGS__))
#define __VA_ARGS_APPLYED__(FUNC, ARGS) FUNC ARGS


/*
** MSVC does not allow nested functions
** so function is wrapped in nested struct
*/
#ifdef _MSC_VER

#define luaA_function_args0_macro(L, func, ret_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  *(ret_t*)out = func(); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 0)

#define luaA_function_args0_void_macro(L, func, ret_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  func(); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 0)

#define luaA_function_args1_macro(L, func, ret_t, arg0_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  *(ret_t*)out = func(a0); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 1, luaA_type_id(arg0_t))

#define luaA_function_args1_void_macro(L, func, ret_t, arg0_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  func(a0); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 1, luaA_type_id(arg0_t))

#define luaA_function_args2_macro(L, func, ret_t, arg0_t, arg1_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  *(ret_t*)out = func(a0, a1); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 2, luaA_type_id(arg0_t), luaA_type_id(arg1_t))

#define luaA_function_args2_void_macro(L, func, ret_t, arg0_t, arg1_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  func(a0, a1); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 2, luaA_type_id(arg0_t), luaA_type_id(arg1_t))

#define luaA_function_args3_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  *(ret_t*)out = func(a0, a1, a2); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 3, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t))

#define luaA_function_args3_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  func(a0, a1, a2); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 3, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t))

#define luaA_function_args4_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 4, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t))

#define luaA_function_args4_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  func(a0, a1, a2, a3); }; }; \
luaA_function_typeid(L, func,(luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 4, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t))

#define luaA_function_args5_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 5, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t))

#define luaA_function_args5_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  func(a0, a1, a2, a3, a4); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 5, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t))

#define luaA_function_args6_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 6, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t))

#define luaA_function_args6_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  func(a0, a1, a2, a3, a4, a5); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 6, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t))

#define luaA_function_args7_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 7, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t))

#define luaA_function_args7_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  func(a0, a1, a2, a3, a4, a5, a6); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 7, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t))

#define luaA_function_args8_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 8, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t))

#define luaA_function_args8_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 8, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t))

#define luaA_function_args9_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 9, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t))

#define luaA_function_args9_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 9, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t))

#define luaA_function_args10_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  arg9_t a9 = *(arg9_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)+sizeof(arg8_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_id(ret_t), 10, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t), luaA_type_id(arg9_t))

#define luaA_function_args10_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  arg9_t a9 = *(arg9_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)+sizeof(arg8_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }; }; \
luaA_function_typeid(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type_find("void"), 10, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t), luaA_type_id(arg9_t))


#else


#define luaA_function_args0_macro(L, func, ret_t) \
void __luaA_##func(void* out, void* args) { \
  *(ret_t*)out = func(); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 0)

#define luaA_function_args0_void_macro(L, func, ret_t) \
void __luaA_##func(void* out, void* args) { \
  func(); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 0)

#define luaA_function_args1_macro(L, func, ret_t, arg0_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  *(ret_t*)out = func(a0); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 1, luaA_type_id(arg0_t))

#define luaA_function_args1_void_macro(L, func, ret_t, arg0_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  func(a0); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 1, luaA_type_id(arg0_t))

#define luaA_function_args2_macro(L, func, ret_t, arg0_t, arg1_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  *(ret_t*)out = func(a0, a1); };  \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 2, luaA_type_id(arg0_t), luaA_type_id(arg1_t))

#define luaA_function_args2_void_macro(L, func, ret_t, arg0_t, arg1_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  func(a0, a1); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 2, luaA_type_id(arg0_t), luaA_type_id(arg1_t))

#define luaA_function_args3_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  *(ret_t*)out = func(a0, a1, a2); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 3, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t))

#define luaA_function_args3_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  func(a0, a1, a2); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 3, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t))

#define luaA_function_args4_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 4, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t))

#define luaA_function_args4_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  func(a0, a1, a2, a3); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 4, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t))

#define luaA_function_args5_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 5, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t))

#define luaA_function_args5_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  func(a0, a1, a2, a3, a4); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 5, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t))

#define luaA_function_args6_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 6, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t))

#define luaA_function_args6_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  func(a0, a1, a2, a3, a4, a5); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 6, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t))

#define luaA_function_args7_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 7, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t))

#define luaA_function_args7_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  func(a0, a1, a2, a3, a4, a5, a6); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 7, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t))

#define luaA_function_args8_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 8, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t))

#define luaA_function_args8_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 8, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t))

#define luaA_function_args9_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 9, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t))

#define luaA_function_args9_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 9, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t))

#define luaA_function_args10_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  arg9_t a9 = *(arg9_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)+sizeof(arg8_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 10, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t), luaA_type_id(arg9_t))

#define luaA_function_args10_void_macro(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  arg8_t a8 = *(arg8_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)); \
  arg9_t a9 = *(arg9_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)+sizeof(arg7_t)+sizeof(arg8_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }; \
luaA_function_typeid(L, func, __luaA_##func, #func, luaA_type_id(ret_t), 10, luaA_type_id(arg0_t), luaA_type_id(arg1_t), luaA_type_id(arg2_t), luaA_type_id(arg3_t), luaA_type_id(arg4_t), luaA_type_id(arg5_t), luaA_type_id(arg6_t), luaA_type_id(arg7_t), luaA_type_id(arg8_t), luaA_type_id(arg9_t))

#endif

#endif
