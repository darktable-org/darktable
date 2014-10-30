/*
** macros to wrap function registration
**
** creates an inline function definition
** using a set calling convention
** which allows for automatic calling
*/

#ifndef lautocall_h
#define lautocall_h

#define LUAA_EVAL(...) __VA_ARGS__

/* Join Three Strings */
#define LUAA_JOIN2(X, Y) X ## Y
#define LUAA_JOIN3(X, Y, Z) X ## Y ## Z

/* workaround for MSVC VA_ARGS expansion */
#define LUAA_APPLY(FUNC, ARGS) LUAA_EVAL(FUNC ARGS)

/* Argument Counter */
#define LUAA_COUNT(...) LUAA_COUNT_COLLECT(_, ##__VA_ARGS__, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define LUAA_COUNT_COLLECT(...) LUAA_COUNT_SHIFT(__VA_ARGS__)
#define LUAA_COUNT_SHIFT(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _N, ...) _N

/* Detect Void */
#define LUAA_VOID(X) LUAA_JOIN2(LUAA_VOID_, X)
#define LUAA_VOID_void
#define LUAA_CHECK_N(X, N, ...) N
#define LUAA_CHECK(...) LUAA_CHECK_N(__VA_ARGS__, ,)
#define LUAA_SUFFIX(X) LUAA_SUFFIX_CHECK(LUAA_VOID(X))
#define LUAA_SUFFIX_CHECK(X) LUAA_CHECK(LUAA_JOIN2(LUAA_SUFFIX_, X))
#define LUAA_SUFFIX_ ~, _void,

/* Declaration and Register Macros */
#define LUAA_DECLARE(func, ret_t, count, suffix, ...) LUAA_APPLY(LUAA_JOIN3(luaA_function_declare, count, suffix), (func, ret_t, ##__VA_ARGS__))
//#define LUAA_DECLARE(func, ret_t, count, suffix, ...) LUAA_APPLY(LUAA_JOIN3(luaA_function_declare, count, suffix), (func, ret_t, ##__VA_ARGS__))
#define LUAA_REGISTER(L, func, ret_t, count, ...) LUAA_APPLY(LUAA_JOIN2(luaA_function_register, count), (L, func, ret_t, ##__VA_ARGS__))

/*
** MSVC does not allow nested functions
** so function is wrapped in nested struct
*/
#ifdef _MSC_VER

#define luaA_function_declare0(func, ret_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  *(ret_t*)out = func(); }; }

#define luaA_function_declare0_void(func, ret_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  func(); }; }

#define luaA_function_declare1(func, ret_t, arg0_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  *(ret_t*)out = func(a0); }; }

#define luaA_function_declare1_void(func, ret_t, arg0_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  func(a0); }; }

#define luaA_function_declare2(func, ret_t, arg0_t, arg1_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  *(ret_t*)out = func(a0, a1); }; }

#define luaA_function_declare2_void(func, ret_t, arg0_t, arg1_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  func(a0, a1); }; }

#define luaA_function_declare3(func, ret_t, arg0_t, arg1_t, arg2_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  *(ret_t*)out = func(a0, a1, a2); }; }

#define luaA_function_declare3_void(func, ret_t, arg0_t, arg1_t, arg2_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  func(a0, a1, a2); }; }

#define luaA_function_declare4(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3); }; }

#define luaA_function_declare4_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  func(a0, a1, a2, a3); }; }

#define luaA_function_declare5(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4); }; }

#define luaA_function_declare5_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  func(a0, a1, a2, a3, a4); }; }

#define luaA_function_declare6(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5); }; }

#define luaA_function_declare6_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  func(a0, a1, a2, a3, a4, a5); }; }

#define luaA_function_declare7(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6); }; }

#define luaA_function_declare7_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  func(a0, a1, a2, a3, a4, a5, a6); }; }

#define luaA_function_declare8(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7); }; }

#define luaA_function_declare8_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
struct __luaA_wrap_##func { static void __luaA_##func(char* out, char* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7); }; }

#define luaA_function_declare9(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
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
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }; }

#define luaA_function_declare9_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
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
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }; }

#define luaA_function_declare10(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
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
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }; }

#define luaA_function_declare10_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
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
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }; }

  
#define luaA_function_register0(L, func, ret_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 0)

#define luaA_function_register1(L, func, ret_t, arg0_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 1, luaA_type(L, arg0_t))

#define luaA_function_register2(L, func, ret_t, arg0_t, arg1_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 2, luaA_type(L, arg0_t), luaA_type(L, arg1_t))

#define luaA_function_register3(L, func, ret_t, arg0_t, arg1_t, arg2_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 3, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t))

#define luaA_function_register4(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 4, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t))
  
#define luaA_function_register5(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 5, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t))
  
#define luaA_function_register6(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 6, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t))

#define luaA_function_register7(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 7, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t))
  
#define luaA_function_register8(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 8, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t), luaA_type(L, arg7_t))
  
#define luaA_function_register9(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 9, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t), luaA_type(L, arg7_t), luaA_type(L, arg8_t))
  
#define luaA_function_register10(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
  luaA_function_register_type(L, func, (luaA_Func)__luaA_wrap_##func::__luaA_##func, #func, luaA_type(L, ret_t), 10, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t), luaA_type(L, arg7_t), luaA_type(L, arg8_t), luaA_type(L, arg9_t))

  
#else


#define luaA_function_declare0(func, ret_t) \
void __luaA_##func(void* out, void* args) { \
  *(ret_t*)out = func(); }

#define luaA_function_declare0_void(func, ret_t) \
void __luaA_##func(void* out, void* args) { \
  func(); }

#define luaA_function_declare1(func, ret_t, arg0_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  *(ret_t*)out = func(a0); }

#define luaA_function_declare1_void(func, ret_t, arg0_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  func(a0); }

#define luaA_function_declare2(func, ret_t, arg0_t, arg1_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  *(ret_t*)out = func(a0, a1); }

#define luaA_function_declare2_void(func, ret_t, arg0_t, arg1_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  func(a0, a1); }

#define luaA_function_declare3(func, ret_t, arg0_t, arg1_t, arg2_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  *(ret_t*)out = func(a0, a1, a2); }

#define luaA_function_declare3_void(func, ret_t, arg0_t, arg1_t, arg2_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  func(a0, a1, a2); }

#define luaA_function_declare4(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3); }

#define luaA_function_declare4_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  func(a0, a1, a2, a3); }

#define luaA_function_declare5(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4); }

#define luaA_function_declare5_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  func(a0, a1, a2, a3, a4); }

#define luaA_function_declare6(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5); }

#define luaA_function_declare6_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  func(a0, a1, a2, a3, a4, a5); }

#define luaA_function_declare7(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6); }

#define luaA_function_declare7_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  func(a0, a1, a2, a3, a4, a5, a6); }

#define luaA_function_declare8(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7); }

#define luaA_function_declare8_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
void __luaA_##func(void* out, void* args) { \
  arg0_t a0 = *(arg0_t*)args; \
  arg1_t a1 = *(arg1_t*)(args+sizeof(arg0_t)); \
  arg2_t a2 = *(arg2_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)); \
  arg3_t a3 = *(arg3_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)); \
  arg4_t a4 = *(arg4_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)); \
  arg5_t a5 = *(arg5_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)); \
  arg6_t a6 = *(arg6_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)); \
  arg7_t a7 = *(arg7_t*)(args+sizeof(arg0_t)+sizeof(arg1_t)+sizeof(arg2_t)+sizeof(arg3_t)+sizeof(arg4_t)+sizeof(arg5_t)+sizeof(arg6_t)); \
  func(a0, a1, a2, a3, a4, a5, a6, a7); }

#define luaA_function_declare9(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
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
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }

#define luaA_function_declare9_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
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
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8); }

#define luaA_function_declare10(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
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
  *(ret_t*)out = func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }

#define luaA_function_declare10_void(func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
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
  func(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }

  
#define luaA_function_register0(L, func, ret_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 0)

#define luaA_function_register1(L, func, ret_t, arg0_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 1, luaA_type(L, arg0_t))

#define luaA_function_register2(L, func, ret_t, arg0_t, arg1_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 2, luaA_type(L, arg0_t), luaA_type(L, arg1_t))

#define luaA_function_register3(L, func, ret_t, arg0_t, arg1_t, arg2_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 3, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t))

#define luaA_function_register4(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 4, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t))
  
#define luaA_function_register5(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 5, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t))
  
#define luaA_function_register6(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 6, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t))

#define luaA_function_register7(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 7, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t))
  
#define luaA_function_register8(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 8, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t), luaA_type(L, arg7_t))
  
#define luaA_function_register9(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 9, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t), luaA_type(L, arg7_t), luaA_type(L, arg8_t))
  
#define luaA_function_register10(L, func, ret_t, arg0_t, arg1_t, arg2_t, arg3_t, arg4_t, arg5_t, arg6_t, arg7_t, arg8_t, arg9_t) \
  luaA_function_register_type(L, func, __luaA_##func, #func, luaA_type(L, ret_t), 10, luaA_type(L, arg0_t), luaA_type(L, arg1_t), luaA_type(L, arg2_t), luaA_type(L, arg3_t), luaA_type(L, arg4_t), luaA_type(L, arg5_t), luaA_type(L, arg6_t), luaA_type(L, arg7_t), luaA_type(L, arg8_t), luaA_type(L, arg9_t))

  
#endif

#endif
