LibRaw-cmake
============

[![Build Status](https://travis-ci.org/LibRaw/LibRaw-cmake.svg?branch=master)](https://travis-ci.org/LibRaw/LibRaw-cmake)

This is a separate repository for LibRaw CMake support scripts.
It is [unmaintained](https://github.com/LibRaw/LibRaw/issues/44#issuecomment-60344793) by the authors of LibRaw and relies solely on user contributions.
The current community-maintainer of this repository is [Maik Riechert](https://github.com/neothemachine).

If you wish to contribute to it, please open an issue or submit a pull request in this repository. Do *not* submit issues or pull requests regarding CMake to the main [LibRaw repository](https://github.com/LibRaw/LibRaw). Also, try to keep CMake related discussions out of the [main forum](http://www.libraw.org/forum), instead use the issues for that.

If you like to become a direct contributor with write permissions to this repository, please contact the [LibRaw authors](https://github.com/LibRaw).

How to use
----------
Just copy the contents of this repository into the root LibRaw folder and run cmake as usual.

### Add as a submodule

Add this repo and libraw as git submodules:

`git submodule add https://github.com/LibRaw/LibRaw-cmake.git`

`git submodule add https://github.com/LibRaw/LibRaw.git`

In your CMakeLists.txt add 

```cmake
add_subdirectory(LibRaw-cmake)
target_link_libraries(ProjectName PRIVATE libraw::libraw)
```

Set the `LIBRAW_PATH` CMake variable to point to the **LibRaw** directory:

`cmake -DLIBRAW_PATH=./LibRaw/`