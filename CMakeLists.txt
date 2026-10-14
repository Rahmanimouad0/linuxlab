cmake_minimum_required(VERSION 3.16)
project(LinuxLab VERSION 1.0.0 DESCRIPTION "Systems programming & Linux internals" LANGUAGES C)

# ---------------------------------------------------------------- options
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clang-tidy
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LINUXLAB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wformat=2
                    -Wstrict-prototypes -Wmissing-prototypes -Wvla)
if(LINUXLAB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ------------------------------------------------------------------ shell
file(GLOB LSH_SOURCES CONFIGURE_DEPENDS shell/src/*.c)
add_executable(lsh ${LSH_SOURCES})

# ------------------------------------------- labs, tools and benchmarks
add_library(linuxlab_common STATIC common/util.c)
target_include_directories(linuxlab_common PUBLIC common)
target_link_libraries(linuxlab_common PUBLIC Threads::Threads rt)

file(GLOB PROGRAM_SOURCES CONFIGURE_DEPENDS labs/*/*.c tools/*.c benchmarks/*.c)
foreach(src IN LISTS PROGRAM_SOURCES)
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE linuxlab_common)
endforeach()

# ------------------------------------------------------------------ tests
add_executable(test_parser tests/unit/test_parser.c shell/src/lexer.c shell/src/parser.c)

enable_testing()
add_test(NAME unit COMMAND test_parser)
add_test(NAME shell
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/shell/test_shell.sh $<TARGET_FILE:lsh>)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME jobctl
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/shell/test_job_control.py
                     $<TARGET_FILE:lsh>)
endif()
add_test(NAME labs
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/labs/test_labs.sh ${CMAKE_BINARY_DIR}/bin)
set_tests_properties(labs PROPERTIES TIMEOUT 600)
