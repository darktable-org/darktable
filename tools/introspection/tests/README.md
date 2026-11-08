# Introspection generator regression tests

These tests run the actual Perl parser on a small module fixture, compile the
generated C metadata against the real introspection API, and validate enum
symbols, numeric values, descriptions, lookup helpers and closing sentinels.
The fixture includes two anonymous enums with different values/counts, repeated
uses of an anonymous enum, and repeated uses of a named enum.

With a C compiler, CMake, Perl, pkg-config and GLib development files installed:

```sh
cmake -S tools/introspection/tests -B build/introspection-tests
cmake --build build/introspection-tests
ctest --test-dir build/introspection-tests --output-on-failure
```
