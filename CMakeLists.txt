cmake_minimum_required(VERSION 3.16)
project(malloc_jitter_lab VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -O2 -g -fno-omit-frame-pointer)
endif()

option(MJL_BUILD_TEST   "Build all jitter experiments" ON)
option(MJL_BUILD_SCRIPTS "Generate run_all script target" ON)
option(MJL_RUN_TESTS     "Run experiments under CTest" ON)

find_package(Threads REQUIRED)

add_subdirectory(src)

if(MJL_BUILD_SCRIPTS)
  add_subdirectory(scripts)
endif()

if(MJL_RUN_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
