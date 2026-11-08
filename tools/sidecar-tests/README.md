# Android sidecar discovery regression tests

Android API 26 has no `glob()` or `globfree()`. The Android image importer uses
the same GLib directory implementation compiled by these host tests. They check
literal metacharacters and Unicode filenames, sidecar ordering and suffixes,
relative path spelling, extensionless inputs, empty files, directories, symlinks
and FIFOs. The desktop importer keeps its existing implementation.

```sh
cmake -S tools/sidecar-tests -B build/sidecar-tests
cmake --build build/sidecar-tests
ctest --test-dir build/sidecar-tests --output-on-failure
```

The host test is a discovery test; it does not establish Android runtime or RAW
decoding support. Cross-compile `src/common/sidecar.c` with the API 26 NDK and
target GLib headers to verify the Android ABI separately.
