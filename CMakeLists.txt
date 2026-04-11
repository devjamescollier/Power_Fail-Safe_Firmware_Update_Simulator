cmake_minimum_required(VERSION 3.16)
project(failsafe_fwupdate VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(FWU_WERROR "Treat warnings as errors" ON)
option(FWU_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "fwupdate-sim expects GCC or Clang (got ${CMAKE_C_COMPILER_ID})")
endif()

add_library(fwu_warnings INTERFACE)
target_compile_options(fwu_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef)
if(FWU_WERROR)
    target_compile_options(fwu_warnings INTERFACE -Werror)
endif()

if(FWU_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(fwu_core STATIC
    src/core/crc32.c
    src/core/image.c
    src/core/meta.c
    src/core/status.c
    src/core/update.c
)
target_include_directories(fwu_core PUBLIC include PRIVATE src/core)
target_link_libraries(fwu_core PRIVATE fwu_warnings)

add_library(fwu_flashsim STATIC src/sim/flash_sim.c)
target_include_directories(fwu_flashsim PUBLIC src/sim)
target_link_libraries(fwu_flashsim PUBLIC fwu_core PRIVATE fwu_warnings)

add_executable(fwsim src/app/fwsim_main.c)
target_link_libraries(fwsim PRIVATE fwu_flashsim fwu_core fwu_warnings)

include(CTest)
if(BUILD_TESTING)
    foreach(t crc32 image flash_sim meta update)
        add_executable(test_${t} tests/unit/test_${t}.c)
        target_include_directories(test_${t} PRIVATE tests/unit)
        target_link_libraries(test_${t} PRIVATE fwu_flashsim fwu_core fwu_warnings)
        add_test(NAME unit_${t} COMMAND test_${t} ${CMAKE_CURRENT_BINARY_DIR}/unit_${t}.img)
        set_tests_properties(unit_${t} PROPERTIES LABELS unit)
    endforeach()

    add_test(NAME core_portability
             COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
                     -P ${PROJECT_SOURCE_DIR}/cmake/check_core_portability.cmake)
    set_tests_properties(core_portability PROPERTIES LABELS static)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME py_mkimage
             COMMAND Python3::Interpreter -B ${PROJECT_SOURCE_DIR}/tests/system/test_mkimage.py)
    set_tests_properties(py_mkimage PROPERTIES LABELS python)

    add_test(NAME py_cli_e2e
             COMMAND Python3::Interpreter -B ${PROJECT_SOURCE_DIR}/tests/system/test_cli.py)
    set_tests_properties(py_cli_e2e PROPERTIES LABELS "python;e2e"
                         ENVIRONMENT "FWSIM=$<TARGET_FILE:fwsim>")

    add_test(NAME fault_sweep
             COMMAND Python3::Interpreter -B ${PROJECT_SOURCE_DIR}/tests/system/fault_sweep.py
                     --fwsim $<TARGET_FILE:fwsim>)
    set_tests_properties(fault_sweep PROPERTIES LABELS "python;fault" TIMEOUT 600)
endif()
