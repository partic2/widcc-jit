# widcc — tiny C compiler + JIT runtime
# -------------------------------------
# Targets:
#
#   widcc_jit   — static library exposing the JIT Runtime API (symbol map,
#                 generated code/data ownership) and Compiler API (public
#                 header: include/widcc_jit.h). Built in single-file mode
#                 from src/all.c; all internal symbols are static, so only
#                 the widcc_jit.h API is exported.
#   jit_test    — functional test suite (examples/jit_test.c), built by
#                 default; disable with -DWIDCC_BUILD_TESTS=OFF.
#   stress_svalue — SValue backend stress tests (examples/stress_svalue.c):
#                 spill-heavy expressions plus algorithm-level programs
#                 (Huffman coding, quicksort, sieve, matmul, linked list).
#   mem_test    — memory lifecycle test (examples/mem_test.c). Build with
#                 -DWIDCC_SANITIZE=ON to check for leaks under LSan.
#
# Build:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/jit_test      # runs the functional test suite
cmake_minimum_required(VERSION 3.14)
project(widcc C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

# GNUInstallDirs must be included before any reference to
# CMAKE_INSTALL_INCLUDEDIR (e.g. inside $<INSTALL_INTERFACE:...>), otherwise
# the variable is empty at generator-expression-evaluation time.
include(GNUInstallDirs)


# ---------- common compile definitions / options ----------

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(WIDCC_PLATFORM_CFLAGS "")
  set(WIDCC_PLATFORM_LIBS psapi)
else()
  set(WIDCC_PLATFORM_CFLAGS -D_GNU_SOURCE)
  set(WIDCC_PLATFORM_LIBS dl)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(WIDCC_WARNINGS -Wall -Wextra -Wno-switch)
else()
  set(WIDCC_WARNINGS "")
endif()

# ---------- optional 32-bit (ILP32) host build ----------
#
# On a 32-bit host widcc follows the host data model: int/long/pointers are
# 4 bytes, long long stays 8. 64-bit integer codegen is rejected at compile
# time by the sljit backend in that configuration.
option(WIDCC_M32 "Build for a 32-bit (ILP32) host" OFF)
if(WIDCC_M32)
  add_compile_options(-m32)
  add_link_options(-m32)
endif()

# ---------- optional sanitizer build ----------
#
# Configure a separate build dir with -DWIDCC_SANITIZE=ON to run the test
# targets under AddressSanitizer + LeakSanitizer; a leak then fails ctest.
option(WIDCC_SANITIZE "Instrument with AddressSanitizer/LeakSanitizer" OFF)
if(WIDCC_SANITIZE)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

# ---------- sljit backend (object library, no install) ----------

add_library(sljit OBJECT sljit/sljit_src/sljitLir.c)
target_include_directories(sljit PRIVATE sljit/sljit_src)

# ---------- JIT runtime (public static library) ----------
#
# Single-file mode: src/all.c includes every implementation file, so the
# whole frontend/backend builds as one translation unit and all internal
# symbols are static. Only the widcc_jit.h API is exported.

add_library(widcc_jit STATIC src/all.c $<TARGET_OBJECTS:sljit>)
target_compile_options(widcc_jit PRIVATE ${WIDCC_WARNINGS})
target_compile_definitions(widcc_jit PRIVATE ${WIDCC_PLATFORM_CFLAGS})
target_include_directories(widcc_jit
  PRIVATE sljit/sljit_src ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(widcc_jit PUBLIC ${WIDCC_PLATFORM_LIBS})

# pkg-config-style alias for consumers that prefer the camelCase name.
add_library(widcc::jit ALIAS widcc_jit)


# ---------- tests (opt-out with -DWIDCC_BUILD_TESTS=OFF) ----------

option(WIDCC_BUILD_TESTS "Build the test suites (links widcc_jit)" ON)
if(WIDCC_BUILD_TESTS)
  enable_testing()

  add_executable(jit_test examples/jit_test.c)
  target_link_libraries(jit_test PRIVATE widcc_jit)
  target_compile_options(jit_test PRIVATE ${WIDCC_WARNINGS})
  add_test(NAME jit_test COMMAND jit_test)

  add_executable(stress_svalue examples/stress_svalue.c)
  target_link_libraries(stress_svalue PRIVATE widcc_jit)
  target_compile_options(stress_svalue PRIVATE ${WIDCC_WARNINGS})
  if(UNIX)
    target_link_libraries(stress_svalue PRIVATE m)
  endif()
  add_test(NAME stress_svalue COMMAND stress_svalue)

  add_executable(mem_test examples/mem_test.c)
  target_link_libraries(mem_test PRIVATE widcc_jit)
  target_compile_options(mem_test PRIVATE ${WIDCC_WARNINGS})
  add_test(NAME mem_test COMMAND mem_test)

  add_executable(run_c_file examples/run_c_file.c)
  target_link_libraries(run_c_file PRIVATE widcc_jit)
  target_compile_options(run_c_file PRIVATE ${WIDCC_WARNINGS})
endif()
