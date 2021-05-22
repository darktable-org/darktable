Change Log
----------

### Changes from version 1 to 2

The major change in this version is due to a change the internal hashtable 
utility, which was removed and all the internal metadata which LuaAutoC uses 
was instead put into the Lua registry and accessed using Lua tables.

The two major advantages of this is that LuaAutoC no longer relies on
static variables, and that all the meta-data about the LuaAutoC types
can be found by looking into the registry either by C users or Lua users.

Here is a brief (incomplete) change log intended for people porting code.

* Removal of internal Hashtable. Instead all data is in the Lua registry in Lua tables
* Merged all source files into single source file. Simplified build process.
* Renamed header `lautofunc.h` to `lautocall.h`.
* All open and close functions merged into `luaA_open` and `luaA_close`
* All functions now take Lua state `L` as the first variable.
* Functions that ended in `typeid` to indicate they take `luaA_Type` inputs now end in just `type`
* Typedef of `luaA_Type` is now `lua_Integer`.
* Renamed `luaA_typeid` to `luaA_type`
* Renamed `luaA_type_name` to `luaA_typename`.
* Renamed `luaA_type_size` to `luaA_typesize`.
* Removed `luaA_struct_next`, this proved difficult to implement in the new system.
* Enum functions no longer take case sensitive argument. Aliases should be used instead (`luaA_enum_value_name`).
* Removed `luaA_function_void`. Use `luaA_function` and specify `void` as the return type. This is now dealt with automatically.
* Removed `luaA_function_decl_void`. Use `luaA_function_declare` and specify `void` as the return type. This is now dealt with automatically.
* Removed `luaA_function_reg_void`. Use `luaA_function_register` and specify `void` as the return type. This is now dealt with automatically.
* Renamed `luaA_function_decl` to `luaA_function_declare`.
* Renamed `luaA_function_reg` to `luaA_function_register`.
* Function `luaA_function` no longer takes argument count. This is now found automatically.  
* Function `luaA_function_declare` no longer takes argument count. This is now found automatically.  
* Function `luaA_function_register` no longer takes argument count. This is now found automatically.  
* Exposed argument and return stack sizes for fake stack.
* Renamed `demos` folder to `examples` and all the files within.
* Changed `luaA_call` and `luaA_call_name` to remove their arguments from stack, only leaving return values.
* Switched argument order to `luaA_struct_push_member` so that member comes before input.
* Switched argument order to `luaA_struct_push_member_name` so that member comes before input.
* Switched argument order to `luaA_struct_to_member` so that member comes before output.
* Switched argument order to `luaA_struct_to_member_name` so that member comes before output.

