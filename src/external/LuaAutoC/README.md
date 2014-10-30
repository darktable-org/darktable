LuaAutoC
========

Automagically use C Functions and Structs with the Lua API

Version 2.0.1

```c
#include "lautoc.h"

int fib(int n) {
  if (n == 0) { return 1; }
  if (n == 1) { return 1; }
  return fib(n-1) + fib(n-2);
}

int main(int argc, char** argv) {
  
  /* Init Lua & LuaAutoC */
  lua_State* L = luaL_newstate();
  luaA_open(L);
  
  /* Register `fib` function */
  luaA_function(L, fib, int, int);
  
  /* Push integer onto stack and call `fib` */
  lua_pushinteger(L, 25);
  luaA_call(L, fib);
  
  /* Print result & pop */
  printf("Result: %i\n", (int)lua_tointeger(L, -1));
  lua_pop(L, 1);
  
  luaA_close(L);
  lua_close(L);
  
  return 0;
}
```

Features
--------

* Friendly API.
* Easy to integrate (1 source file, 2 headers).
* Existing code is unaffected.
* Flexible, Extensible and Powerful.
* Provides dynamic runtime reflection.


Usage
-----

### Functions

At its most basic, LuaAutoC can be used to automatically call C functions from the Lua API. Lua stack arguments are automatically popped and converted to C types, the function is executed, and the return value is then converted back to a Lua type and placed on top the stack. First the function must be registered with `luaA_function`, and then at any point later it can be called with `luaA_call`.

```c
#include "lautoc.h"

float power(float val, int pow) {
  float x = 1.0;
  for(int i = 0; i < pow; i++) {
    x = x * val;
  }
  return x;
}

int main(int argc, char **argv) {
  
  lua_State* L = luaL_newstate();
  luaA_open(L);
  
  luaA_function(L, power, float, float, int);
  
  lua_pushnumber(L, 4.2);
  lua_pushinteger(L, 3);
  luaA_call(L, power);
  
  printf("Result: %f\n", lua_tonumber(L, -1));
  lua_pop(L, 1);
  
  luaA_close(L);
  lua_close(L);
  
  return 0;
}
```

### Structs

LuaAutoC also provides functions to deal with structs. Their members can be pushed onto or read from the stack, with automatic conversion of types between Lua and C provided.

```c
typedef struct {
  float x, y, z;
} vec3;
```

As with functions first they need to be registered.

```c
luaA_struct(L, vec3);
luaA_struct_member(L, vec3, x, float);
luaA_struct_member(L, vec3, y, float);
luaA_struct_member(L, vec3, z, float);
```

And then they can be used with the Lua API.

```c
vec3 pos = {1.0f, 2.11f, 3.16f};

luaA_struct_push_member(L, vec3, x, &pos);
printf("x: %f\n", lua_tonumber(L, -1));
lua_pop(L, 1);

lua_pushnumber(L, 0.0);
luaA_struct_to_member(L, vec3, x, &pos, -1);
lua_pop(L, 1);

luaA_struct_push_member(L, vec3, x, &pos);
printf("x: %f\n", lua_tonumber(L, -1));
lua_pop(L, 1);
```

### Conversion

To use LuaAutoC with non-native types it is possible to register your own conversion functions.

```c
typedef struct {
  int fst, snd;
} pair;

static int luaA_push_pair(lua_State* L, luaA_Type t, const void* c_in) {
  pair* p = (pair*)c_in;
  lua_pushinteger(L, p->fst);
  lua_pushinteger(L, p->snd);
  return 2;
}

static void luaA_to_pair(lua_State* L, luaA_Type t, void* c_out, int index) {
  pair* p = (pair*)c_out;
  p->snd = lua_tointeger(L, index);
  p->fst = lua_tointeger(L, index-1);
}
```

These are registered with `luaA_conversion`.

```c
luaA_conversion(L, pair, luaA_push_pair, luaA_to_pair);
```

And are then automatically used in the calling of functions, or in the conversion of structs and their members. Registered conversions are also available directly for manipulation of the Lua stack using `luaA_push` and `luaA_to`. For example to push a pair onto the stack...

```c
pair p = {1, 2};
luaA_push(L, pair, &p);
```

Essentially registering a conversion is all that is required to make that type available from the whole of LuaAutoC.

But who wants to write a bunch of conversion functions? When structs are registered with LuaAutoC, if no conversion functions are registered, automatic conversion to a Lua table is performed. So in reality writing conversions is a rare thing!

```c
typedef struct {
  int id;
  int legs;
  float height;
} table;
```

```c
luaA_struct(L, table);
luaA_struct_member(L, table, id, int);
luaA_struct_member(L, table, legs, int);
luaA_struct_member(L, table, height, float);
```

```c
table t = {0, 4, 0.72};

luaA_push(L, table, &t);

lua_getfield(L, -1, "legs");
printf("legs: %i\n", (int)lua_tointeger(L, -1));
lua_pop(L, 1);

lua_getfield(L, -1, "height");
printf("height: %f\n", lua_tonumber(L, -1));
lua_pop(L, 1);

lua_pop(L, 1);
```

This is very useful, but a few words of warning.

* Be careful with circular references. The conversion is recursive and given the chance will happily run forever!
* Be careful of pointer types such as `char*`. An automatic conversion may just assign a new pointer and might not do the intended behaviour, of copying the contents pointed to. This can sometimes screw up the garbage collector.

### Enums

Enums can be used too. They are transformed into string, value mappings.

Again they need to be registered. Multiple strings can be registered for a single value, and any matching Lua string will be transformed into the C value. But if a value has multiple strings, the last registered string will be used when transforming from C to Lua.

```c
typedef enum {
  DIAMONDS,
  HEARTS,
  CLUBS,
  SPADES,
  INVALID = -1
} cards;
```

```c
luaA_enum(L, cards);
luaA_enum_value(L, cards, DIAMONDS);
luaA_enum_value(L, cards, HEARTS);
luaA_enum_value(L, cards, CLUBS);
luaA_enum_value(L, cards, SPADES);
luaA_enum_value(L, cards, INVALID);
```

```c
cards cval = SPADES;
const char* lval = "SPADES";

luaA_push(L, cards, &cval);
printf("%i pushed as %s\n", cval, lua_tostring(L, -1));
lua_pop(L, 1);

lua_pushstring(L, lval);
luaA_to(L, cards, &cval, -1);
printf("%s read back as %i\n", lval, cval); 
lua_pop(L, 1);
```

### Quick & Dirty Interface

Because LuaAutoC stores meta-information and registered functions and types, this lets you call C functions just by providing a string of their name. Using this it is really easy to make a quick and dirty interface to your C library, which can later be extended and wrapped nicely on the Lua side of things.

Given some set of functions.

```c
/* Hello Module Begin */

void hello_world(void) {
  puts("Hello World!");
}

void hello_repeat(int times) {
  for (int i = 0; i < times; i++) {
    hello_world();
  }
}

void hello_person(const char* person) {
  printf("Hello %s!\n", person);
}

int hello_subcount(const char* greeting) {
  int count = 0;
  const char *tmp = greeting;
  while((tmp = strstr(tmp, "hello"))) {
    count++; tmp++;
  }
  return count;
}

/* Hello Module End */
```

We can create a Lua function that takes some function name as a string, and some other arguments, and calls the matching C function with that name.

```c
int C(lua_State* L) {
  return luaA_call_name(L, lua_tostring(L, 1));
}
```

We then just need to register those functions, and also register the `C` function to be avaliable from Lua.

```c
luaA_function(L, hello_world, void);
luaA_function(L, hello_repeat, void, int);
luaA_function(L, hello_person, void, const char*);
luaA_function(L, hello_subcount, int, const char*);

lua_register(L, "C", C);
```

And now we can call our C functions from Lua easily!

```c
luaL_dostring(L,
  "C('hello_world')\n"
  "C('hello_person', 'Daniel')\n"
  "C('hello_repeat', C('hello_subcount', 'hello hello'))\n"
);
```

### Advanced Interface

Using LuaAutoC it is easy to wrap pointers to structs so that they act like object instances. All we need to do is set `__index` and `__newindex` in the metatable.

```lua
Birdie = {}
setmetatable(Birdie, Birdie)

function Birdie.__call()
  local self = {}
  setmetatable(self, Birdie)
  return self
end

Birdie.__index = birdie_index
Birdie.__newindex = birdie_newindex

bird = Birdie()
print(bird.name)
print(bird.num_wings)
bird.num_wings = 3
print(bird.num_wings)
```

Now we just define `birdie_index` and `birdie_newindex` in the C API as shown below. Alternatively developers can define the whole metatable in C and hide the `birdie_newindex` and `birdie_index` functions altogether.

```c
typedef struct {
  char* name;
  int num_wings;
} birdie;

int birdie_index(lua_State* L) {
  const char* membername = lua_tostring(L, -1);
  birdie* self = get_instance_ptr(L);
  return luaA_struct_push_member_name(L, birdie, membername, self);
}

int birdie_newindex(lua_State* L) {
  const char* membername = lua_tostring(L, -2);
  birdie* self = get_instance_ptr(L);
  luaA_struct_to_member_name(L, birdie, membername, self, -1);
  return 0;
}
```

```c
luaA_struct(L, birdie);
luaA_struct_member(L, birdie, name, char*);
luaA_struct_member(L, birdie, num_wings, int);

lua_register(L, "birdie_index", birdie_index);
lua_register(L, "birdie_newindex", birdie_newindex);
```

This is a great way to avoid having to write a bunch of getters and setters!

The `get_instance_ptr` function is left for the user to implement and there are lots of options. The idea is that somehow the Lua table/instance should tell you how to get a pointer to the actual struct instance in C which it represents. A good option is to store C pointers in the Lua table.

It is also possible to make the Lua metatable allocation and deallocation functions call some C functions which allocate and decallocate the structure you are emulating, storing the instance pointer to let you identify it later! Not only that. It isn't difficult to make methods available too!

The true power of Lua AutoC comes if you look a level deeper. If you use `luaA_struct_push_member_name_type` or `luaA_truct_to_member_name_type` you can even generalize the above code to work for arbitrary structs/classes/types. Then when other developers create new structs and types, they can register them with your system to make them trivial to access from Lua.

For this to work you need to get a `luaA_Type` value. This can be found by feeding a string into `luaA_type_find` which will lookup a string and see if a type has been registered with the same name. This means that if you give it a string of a previously registered data type E.G `"birdie"`, it will return a matching id. One trick is to feed it with the name of the instance's metatable. This means it is possible to create a new Lua object with defined `__index` and `__newindex`, it will automatically act like the corresponding C struct of the same name.

### Managing Behaviour

Often in C, the same types can have different meanings. For example an `int*` could either mean that a function wants an array of integers or that it outputs some integer. We can change the behaviour of Lua AutoC without changing the function signature by using typedefs and new conversion functions. Then on function registration you just use the newly defined type rather than the old one. Providing the types are true aliases there wont be any problems with converting types.

```c
static void print_int_list(int* list, int num_ints) {
  for(int i = 0; i < num_ints; i++) {
    printf("Int %i: %i\n", i, list[i]);
  }
}

typedef int* int_list;

static int list_space[512];
static void luaA_to_int_list(lau_State* L, luaA_Type t, void* c_out, int index) {
  for(int i = 1; i <= luaL_getn(L, index); i++) {
    lua_pushinteger(L, i);
    lua_gettable(L, index-1);
	  list_space[i] = lua_tointeger(L, index); lua_pop(L, 1);
  }
  *(int_list*)c_out = list_space;
}

luaA_conversion_to(int_list, luaA_to_int_list);

luaA_function(print_int_list, void, int_list, int);
```

As you can probably see, automatic wrapping and type conversion becomes hard when memory management and pointers are involved.


Headers
-------

It isn't any fun writing out all the registration functions, so I've included a basic Lua script which will auto-generate LuaAutoC code for structs and functions from C header files. In general it works pretty well, but it is fairly basic, not a C parser, and wont cover all situations, so expect to have to do some cleaning up for complicated headers.

```
$ lua lautoc.lua ../Corange/include/assets/sound.h

luaA_struct(sound);
luaA_struct_member(sound, data, char*);
luaA_struct_member(sound, length, int);

luaA_function(wav_load_file, sound*, char*);
luaA_function(sound_delete, void, sound*);
```

Internals
---------

LuaAutoC works by storing meta-information about C types, structs, and functions in the Lua registry.

This allows it to automatically do conversions at runtime. In my opinion this is a far better approach than compile-time wrapping such as is available from programs such as SWIG. Because all the type information is stored in the Lua registry this allows for the use reflection techniques to ensure you build a Lua API that really does match the concepts of your C program. In essence LuaAutoC lets you build a powerful and complex API on the C side of things.

It also means that LuaAutoC is completely un-intrusive, and can be used without touching your original codebase. There is no marking it up or wrapping to be done. Finally, because it is a runtime system, it can be changed, adapted and extended by other developers. You can actually provide functions for developers to use that _generate_ new bindings for their functions, types, or structs, without them having to know a thing about Lua!


FAQ
---

* How do unions work?

  They work the same way as structs. All the `luaA_struct` functions should be fine to use on them. Like in C though, accessing them "incorrectly" in Lua will result in the raw data being interpreted differently. Lua AutoC doesn't do any clever checking for you.  

* Function Pointer Casts?

  LuaAutoC casts function pointers to `void*` so that they can be used in the Lua ecosystem. For this reason it may not work on a platform that disallows these sorts of casts.
  
* Nested Functions?
  
  By standard LuaAutoC makes uses of nested functions. Almost all compilers support these, but if not LuaAutoC can still be used. Instead functions must be "declared" outside of the program, and then "registered" in the runtime. See `example_unnested.c` for a clear example.
  
* Is LuaAutoC slow?
  
  The short answer is no. For most uses LuaAutoC has to lookup runtime information in the Lua registry, and for calling functions it has to duplicate some of the process involved in managing the stack, but for any normal codebase this overhead is minimal. If you are concerned about performance you can still wrap your functions manually, but perhaps if you are using a scripting language like Lua it wont be worth it anyway.

* Is this just macro hacks? Can I really use it in production code?

  There are certainly some macro tricks going on, but the backbone code is very simple. The macros just save typing. I know that it has been used successfully in production in [darktable](https://github.com/darktable-org/darktable) and probably elsewhere. Even so, if you are worried send me an email and I'll explain the internals so that you can decide for yourself. I've also written a short blog post about it [here](http://theorangeduck.com/page/autoc-tools).
  
  
