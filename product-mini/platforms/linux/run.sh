# Copy this file to the build folder.

CC="aarch64-linux-gnu-gcc" CXX="aarch64-linux-gnu-g++" cmake -DWAMR_BUILD_TARGET=AARCH64 -DWAMR_BUILD_LIBC_BUILTIN=0 -DWAMR_BUILD_LIBC_WASI=0 -DWAMR_BUILD_FAST_INTERP=0 ..
