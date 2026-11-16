# Native Raspberry Pi C++ runtime

The `aros-raspi` helper statically links LLVM libc++, libc++abi and libunwind
20.1.0. These components use Apache-2.0 WITH LLVM-exception; the complete
licences are in `LLVM-LICENSE.TXT`, `libcxxabi-LICENSE.TXT` and
`libunwind-LICENSE.TXT`. Preserve these files with binary distributions.
The AROS changes are supplied here as unmodified upstream patches; the AROS
Public License 1.1 is in the project's `LICENSES/AROS-APL-1.1.txt`.

Source inputs are recorded by SHA-256 in `PINS`:

- LLVM release: <https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.0>,
  asset `llvm-project-20.1.0.src.tar.xz`.
- Patches: <https://github.com/aros-development-team/AROS/tree/7654818bf1acd25b11cd840a713dc97692bfff55/tools/crosstools/llvm>.
  These files are unchanged at AROS commit
  `1fef6c415e12523a2c868e4dd1597f5d7b4ba710`.

The maintained runtime reused for the initial port was built on 2026-11-03
with Clang 20.1.0 and the native Raspberry Pi nightly 20261029 SDK. The helper
itself is compiled and linked against the selected `UNARC_SDK_RASPI`; the
2026-11-16 build used the 20261110 Developer SDK. Runtime archive hashes in
`PINS` identify the reused inputs rather than claiming those libraries were
rebuilt against the newer SDK.

## Rebuilding

Extract the LLVM release into a temporary source directory. Apply each patch
with `patch -p1` in its corresponding `libcxx`, `libcxxabi` or `libunwind`
directory. Configure the source's `runtimes` directory using CMake/Ninja with
`CMAKE_SYSTEM_NAME=Generic`, `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`,
`CMAKE_BUILD_TYPE=Release`, and `CMAKE_POSITION_INDEPENDENT_CODE=OFF`.
Use the native `aarch64-aros-clang`, `aarch64-aros-clang++`, `aarch64-aros-ar`,
`aarch64-aros-ranlib` and `aarch64-aros-nm` wrappers; set `RPI_SDK` and
`CMAKE_SYSROOT` to the matching native SDK and each compiler target to
`aarch64-unknown-aros`. Set C/C++ flags to
`-D_GNU_SOURCE -fno-PIC -fno-PIE -O2`, and ASM flags to `-fno-PIC -fno-PIE`.
The maintained build additionally maps its source directory to
`llvm-project-20.1.0.src/` with `-ffile-prefix-map`.

Configure these runtime options (`ON`/`OFF` are CMake booleans):

```
LLVM_ENABLE_RUNTIMES=libunwind;libcxxabi;libcxx
LLVM_INCLUDE_TESTS=OFF
LIBCXX_INCLUDE_BENCHMARKS=OFF
LIBCXX_INCLUDE_TESTS=OFF
LIBCXXABI_INCLUDE_TESTS=OFF
LIBUNWIND_INCLUDE_TESTS=OFF
LIBUNWIND_ENABLE_SHARED=OFF
LIBUNWIND_ENABLE_STATIC=ON
LIBUNWIND_USE_COMPILER_RT=ON
LIBUNWIND_IS_BAREMETAL=ON
LIBUNWIND_SHARED_OUTPUT_NAME=unwind_sh
LIBCXXABI_SHARED_OUTPUT_NAME=c++abi_sh
LIBCXX_SHARED_OUTPUT_NAME=c++_sh
LIBCXXABI_ENABLE_SHARED=OFF
LIBCXXABI_ENABLE_STATIC=ON
LIBCXXABI_USE_LLVM_UNWINDER=ON
LIBCXXABI_ENABLE_THREADS=ON
LIBCXXABI_HAS_PTHREAD_API=ON
LIBCXXABI_USE_COMPILER_RT=ON
LIBCXX_CXX_ABI=libcxxabi
LIBCXX_USE_COMPILER_RT=ON
LIBCXX_ENABLE_STATIC=ON
LIBCXX_ENABLE_SHARED=OFF
LIBCXX_ENABLE_THREADS=ON
LIBCXX_HAS_PTHREAD_API=ON
LIBCXX_ENABLE_LOCALIZATION=ON
LIBCXX_ENABLE_WIDE_CHARACTERS=ON
LIBCXX_ENABLE_FILESYSTEM=ON
LIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
LIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON
```

Set `CMAKE_INSTALL_PREFIX` to a private output directory and run Ninja targets
`install-unwind install-cxxabi install-cxx`. The output provides
`include/c++/v1` and `lib/{libc++,libc++abi,libunwind}.a`. Set
`UNARC_CXXRT_RASPI` in `local.env` to that output directory. Do not install
these libraries into a shared SDK. The supplied archive pins describe the
initial maintained build; changing compiler, SDK or flags requires recording
new archive identities and validating exception handling on the target.
