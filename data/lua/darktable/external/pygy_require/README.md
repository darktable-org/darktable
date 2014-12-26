#require.lua

`require()` rewritten in plain Lua, as close as possible to the original.

It allows to circumvent "yield across C boundary" issues related to mixing coroutines and `require()`, and supports both Lua 5.1 and Lua 5.2 semantics.

## Usage

```Lua
require = require"require".require
```
:o)

## Rerequire

An amnesiac version of require. It removes the corresponding entry from `package.loaded` before running `require` again.

```Lua
rerequire = require"require".rerequire
```

## Full API:

- `require.require51`: replacement for Lua 5.1
- `require.require52`: replacement for Lua 5.2
- `require.require`: one of the above, depending on your Lua version

- `require.rerequire51`: rerequire using the replacement for Lua 5.1
- `require.rerequire52`: rerequire using the replacement for Lua 5.2
- `require.rerequire`: rerequire using one of the above depending on your Lua version
- `require.rerequiredefault`: rerequire using the default Lua `require`

## License: MIT

Copyright © 2014 Pierre-Yves Gérardy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the “Software”), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
