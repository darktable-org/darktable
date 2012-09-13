Lua AutoC
=========

Version 0.8

Introduction
------------

Lua AutoC automatically wraps C functions and structs at runtime so that they can be called from the Lua/C API.

* Don't fancy the idea of hand wrapping every function and struct in your codebase?
* Don't like the look of the monster that is SWIG?
* Want a way for developers to register extra functionality at runtime?

Lua AutoC is here to help.

Background
----------

Lua AutoC is based upon my library [PyAutoC](https://github.com/orangeduck/PyAutoC) which provides similar functionality but for the Python/C API. It is largely just a renaming of what I did there, but some of it has been adapated to better fit the semantics of the Lua API. Most notably rather than using PyObjects and reference counting the functions are designed for pushing to and inspecting the Lua stack.

Although I love Python this version of the library is probably more useful. Python already has a huge array of tools for interacting with C code. This library is also better suited to embedding, which Python somewhat frowns upon over extending, but Lua has a very strong culture of.


Basic Usage 1
-------------

```c
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

static float add_numbers(int first, float second) {
  return first + second;
}

int main(int argc, char **argv) {
  
  lua_State* L = luaL_newstate();
  luaA_open();
  
  luaA_function(L, add_numbers, float, 2, int, float);
  
  lua_pushnumber(L, 6.13);
  lua_pushinteger(L, 5);
  luaA_call(L, add_numbers);
  
  printf("Result: %f\n", lua_tonumber(L, -1));
  
  lua_settop(L, 0);
  
  luaA_close();
  lua_close(L);
  
  return 0;
}
```

Lua AutoC calls reside under the `luaA_*` namespace. This is to make it look seamless alongside Lua code, not because it is affiliated with official Lua development.

In this example Lua AutoC will call `add_numbers` with the Lua values on the stack. It will then push the return value as a Lua object back onto the stack. It will not pop the argument values off the stack. This is all done with no editing of the original function or codebase.

	
Basic Usage 2
-------------

```c
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

typedef struct {
  float x, y, z;
} vector3;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open();
	
  luaA_struct(L, vector3);
  luaA_struct_member(L, vector3, x, float);
  luaA_struct_member(L, vector3, y, float);
  luaA_struct_member(L, vector3, z, float);
  
  vector3 position = {1.0f, 2.11f, 3.16f};
  
  luaA_struct_push_member(L, vector3, &position, y);
  
  printf("Y: %f\n", lua_tonumber(L, -1));
  
  lua_pop(L, 1);
  
  luaA_close();
  lua_close(L);
	
  return 0;
}
```
	
Structs work similarly to their functional counterparts. They can be accessed at runtime and do automatic conversion of types. They provide the ability to push members onto the stack, and also to take objects off and store them in members.
 

Type Conversions
----------------

To call functions or access struct members which have non-primitive types it is possible to register your own conversion functions.

```c
typedef struct {
  int x, y;
} pair;

static int luaA_push_pair(lua_State* L, void* c_in) {
  pair p = *(pair*)c_in;
  lua_pushinteger(L, p.x);
  lua_pushinteger(L, p.y);
  return 2;
}

static void luaA_to_pair(lua_State* L, void* c_out, int index) {
  pair* p = (pair*)c_out;
  p->y = lua_tointeger(L, index);
  p->x = lua_tointeger(L, index-1);
}

luaA_conversion(pair, luaA_push_pair, luaA_to_pair);
```

Now it is possible to call any functions with `pair` as an argument or return type and Lua AutoC will handle any conversions automatically. You can also use the registered functions directly in your code in a fairly convenient and natural way using the `luaA_push` and `luaA_to` macros.

```c
pair p = {1, 2};
luaA_push(L, pair, &p);
```

Alternatively, when you register structs with Lua AutoC, if no conversion functions are known, it will attempt to automatically convert them. This is very useful but a few words of warning. Firstly be careful with circular references. The conversion is recursive and given the chance will happily run forever! Secondly be careful of pointer types such as `char*`. An automatic conversion will assign the struct a new pointer value to a string on the Lua Stack likely to be garbage collected once the call is over. The actual wanted behaviour - of copying the value of the string into the existing allocated memory - wont be performed by default!

```c
typedef struct {
  char* first_name;
  char* second_name;
  float coolness;
} person_details;

luaA_struct(L, person_details);
luaA_struct_member(L, person_details, first_name, char*);
luaA_struct_member(L, person_details, second_name, char*);
luaA_struct_member(L, person_details, coolness, float);

person_details my_details = {"Daniel", "Holden", 125212.213};

luaA_push(L, person_details, &my_details);

lua_getfield(L, -1, "first_name");
printf("First Name: %s\n", lua_tostring(L, -1));
lua_pop(L, 1);

lua_getfield(L, -1, "second_name");
printf("Second Name: %s\n", lua_tostring(L, -1));
lua_pop(L, 1);

lua_pop(L, 1);
```

Using C headers
---------------

I've included a basic Lua script which will autogenerate Lua AutoC code for structs and functions from C headers. Overall it gets the job done but it is fairly basic, not a C parser, and wont cover all situations, so expect to have to do some cleaning up for complicated headers.

```
$ lua autogen.lua ../Corange/include/assets/sound.h

luaA_struct(sound);
luaA_struct_member(sound, data, char*);
luaA_struct_member(sound, length, int);

luaA_function(wav_load_file, sound*, 1, char*);
luaA_function_void(sound_delete, 1, sound*);
```

Extended Usage 1
----------------

You can use Lua AutoC to very quickly and easily create Lua C modules for a bunch of functions such as might be done via SWIG or similar.

```c
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

static float add_numbers(int first, float second) {
  return first + second;
}

static void hello_world(char* person) {
  printf("Hello %s!", person);
}

static int autocall(lua_State* L) {
  return luaA_call_name(L, lua_tostring(L, 1));
}

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open();
  
  luaA_function(L, add_numbers, float, 2, int, float);
  luaA_function_void(L, hello_world, 1, char*);
  
  lua_pushcfunction(L, autocall);
  lua_setglobal(L, "autocall");
  
  luaL_dostring(L, "autocall(\"add_numbers\", 1, 5.2)");
  luaL_dostring(L, "autocall(\"hello_world\", \"Daniel\")");
  
  luaA_close();
  lua_close(L);
	
  return 0;
}
```

Once you have this basic interface of `autocall` it is easy to integrate more complicated and transparent APIs with some more complicated Lua using metaclasses and other tools.


Runtime?
--------

Many developers like to wrap their libraries externally before compile time using programs such as SWIG. This approach has many benefits but can be somewhat brittle and lacking in control. Lua AutoC takes a different approach by storing type information and doing conversions and anything else needed at runtime. As well as being a more controlled approach this also allows for some interesting options for dynamic behaviour.

When normally building a Lua/C extension it is typical to put all accessible functions statically declared in a methods table and then to compile. If a developer wants to add more functions to the Lua bindings he must add more methods to the table. Using Lua AutoC, users and developers can register new functions, structs and type conversions as the program is running, without going through the methods table and Lua API. This means developers can use and extend your Lua API using some really simple methods and without ever touching the Lua stack!

It also means that the job of wrapping is much easier - you can use strings and dynamic elements directly from Lua to do much of the job for you. For example...


Extended Usage 2
----------------

Lua AutoC is perfect for automatically wrapping existing C Structs as Lua classes. By overriding `__index` and `__newindex` using a metatable we can easily make a Lua object that behaves as if it were a C struct.

```lua
Birdie = {}
setmetatable(Birdie, Birdie)
Birdie.__index = birdie_index
Birdie.__newindex = birdie_newindex
function Birdie.__call()
  local self = {}
  setmetatable(self, Birdie)
  return self
end

bird = Birdie()
print(bird.name)
print(bird.num_wings)
```

Where `birdie_index` and `birdie_newindex` are functions defined using the C API as shown below. Or alternatively developers can define the whole metatable in C and hide the `birdie_newindex` and `birdie_index` functions altogether.

```c
typedef struct {
  char* name;
  int num_wings;
} birdie;

static int birdie_index(lua_State* L) {
  const char* membername = lua_tostring(L, -1);
  birdie* self = get_instance_ptr(L);
  return luaA_struct_push_member_name(L, birdie, self, membername);
}

static int birdie_newindex(lua_State* L) {
  const char* membername = lua_tostring(L, -2);
  birdie* self = get_instance_ptr(L);
  luaA_struct_to_member_name(L, birdie, self, membername, -1);
  return 0;
}
  
luaA_struct(L, birdie);
luaA_struct_member(L, birdie, name, char*);
luaA_struct_member(L, birdie, num_wings, int);

lua_pushcfunction(L, birdie_index);
lua_setglobal(L, "birdie_index");

lua_pushcfunction(L, birdie_newindex);
lua_setglobal(L, "birdie_newindex");
```

A lot less work than writing a bunch of getters and setters!

The `get_instance_ptr` function is left for the user to implement and there are lots of options. The idea is that somehow the lua table/instance should tell you how to get a pointer to the actual struct instance in C which it represents. One option is to store C pointers in the lua instance perhaps using a string identifier or even a stringification of the raw pointer value.

For fun why not try also making the lua metatable allocation and decallocation functions call some C functions which allocate and decallocate the structure you are emulating, storing some data to let you identify the instance later. It is also easy to extend the above technique so that, as well as members, the class is able to look up and execute methods!

The true power of Lua AutoC comes if you look a level deeper. If you use `luaA_struct_push_member_name_typeid` or `luaA_truct_to_member_name_typeid` you can even generalize the above code to work for arbritary structs/classes/types which can be added to.

For this to work you need to get a `luaA_Type` value. This can be found by feeding a string into `luaA_type_find` which will lookup a string and see if a type has been registered with the same name. This means that if you give it a string of a previously registered data type E.G `"birdie"`, it will return a matching id. One trick I like it to use is to feed into it the name of the instance's class. This means that I can create a new Lua class with overwritten `__index` and `__newindex` it will automatically act like the corrisponding C struct with the same name.

Managing Behaviour
------------------

Often in C, the same types can have different meanings. For example an `int*` could either mean that a function wants an array of integers or that it outputs some integer. We can change the behaviour of Lua AutoC without changing the function signature by using typedefs and new conversion functions. Then on function registration you just use the newly defined type rather than the old one. Providing the types are true aliases there wont be any problems with converting types or breaking the artificial stack.

```c
static void print_int_list(int* list, int num_ints) {
  for(int i = 0; i < num_ints; i++) {
    printf("Int %i: %i\n", i, list[i]);
  }
}

typedef int* int_list;

static int list_space[512];
static void luaA_to_int_list(lau_State* L, void* c_out, int index) {
  for(int i = 1; i <= luaL_getn(L, index); i++) {
    lua_pushinteger(L, i);
    lua_gettable(L, index-1);
	  list_space[i] = lua_tointeger(L, index); lua_pop(L, 1);
  }
  *(int_list*)c_out = list_space;
}

luaA_conversion_to(int_list, luaA_pop_int_list);

luaA_function_void(print_int_list, 2, int_list, int);
```

As you can probably see, automatic wrapping and type conversion becomes hard when memory management and pointers are involved. I'm looking at ways to improve this, perhaps with the ability to register 'before' and 'after' methods for certain functions or conversions.

FAQ
---

* How do unions work?

  They work the same way as structs. All the luaA_struct functions should be fine to use on them. Like in C though, accessing them "incorrectly" in Lua will result in the raw data being interpreted differently. Lua AutoC doesn't do any clever checking for you.
  
* How do enums work?

  Enums work like any other type and the best way to deal with them is to write an explicit conversion function. There is no real way to know what storage type compilers will pick for an enum, it could be a unsigned char, signed int, long or anything else. If though, you are sure what storage type the compiler is using for a particular enum, it might be easier to just use that as the registered type and get a conversion for free.

* Does this work on Linux/Mac/Windows?
  
  On Linux, yes. On Mac, probably but I don't have one to test on. On Windows, yes under MinGW or Cygwin. The binaries and headers will also link and compile under Visual Studio (in C++ mode).
  
  I've done some experiments getting Lua AutoC to _compile_ under Visual Studio and the port is fairly simple but there are a couple of annoying aspects. If someone is interested I'll be more than happy to share my developments but for now I would rather keep the code in the repo clean.

* Is Lua AutoC slow?
  
  For most uses Lua AutoC has to lookup runtime information in a hashtable. For calling functions it has to duplicate some of the process involved in managing the stack. Perhaps for a very large codebase there might be some overhead in performance and memory but for any normal sized one, this is minimal. If you are concerned about performance you can still wrap your functions manually but perhaps if you are using a scripting language like Lua it isn't much bother.

* Is this just macro hacks? Can I really use it with my production code?

  There are certainly some macro tricks going on, but most of them are pretty simple and nothing to gruesome - they are just there to save you typing. I use my similar library PyAutoC to wrap my game engine Corange (~10000 LOC, ~1000 functions) without any issues. If you are worried send me an email and I'll explain the internals so that you can decide for yourself. I've also written a short blog post on the nitty details [here](http://theorangeduck.com/page/autoc-tools).
  
  