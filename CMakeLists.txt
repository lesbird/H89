cmake_minimum_required(VERSION 3.21)
project(H89 VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)

if(APPLE)
    set(H89_GLFW_DEFAULT OFF)
else()
    set(H89_GLFW_DEFAULT ON)
endif()
option(H89_BUILD_TESTS "Build unit tests and the ZEX harness" ON)
option(H89_GLFW_FRONTEND "Build the GLFW front end (default on Windows)" ${H89_GLFW_DEFAULT})

if(MSVC)
    set(H89_WARNINGS /W4 /permissive- /wd4201) # 4201: anonymous struct in RegPair
else()
    set(H89_WARNINGS -Wall -Wextra -Wno-gnu-anonymous-struct -Wno-nested-anon-types)
endif()

# --- Emulation core: portable C++, no platform headers -----------------------
add_library(h89core STATIC
    core/cpu/Z80.cpp
    core/cpu/Z80Disasm.cpp
    core/devices/H17.cpp
    core/devices/Ins8250.cpp
    core/media/DiskFiles.cpp
    core/media/H17Disk.cpp
    core/terminal/Crtc6845.cpp
    core/terminal/H19.cpp
    core/Machine.cpp
    core/RomSet.cpp
)
target_include_directories(h89core PUBLIC core)
target_compile_options(h89core PRIVATE ${H89_WARNINGS})

# --- OpenGL renderer ----------------------------------------------------------
add_library(h89render STATIC
    render/GL.cpp
    render/GLRenderer.cpp
)
target_include_directories(h89render PUBLIC render)
target_compile_options(h89render PRIVATE ${H89_WARNINGS})
if(APPLE)
    target_link_libraries(h89render PUBLIC "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(h89render PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(h89render PUBLIC OpenGL::GL)
endif()

# --- macOS app (Objective-C++) ------------------------------------------------
if(APPLE)
    add_executable(H89 MACOSX_BUNDLE
        platform/mac/main.mm
        platform/mac/AppDelegate.mm
        platform/mac/H89View.mm
        platform/mac/H89Emulator.mm
        platform/mac/H89DiskBrowser.mm
    )
    set_target_properties(H89 PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/platform/mac/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.h89emu.H89
        MACOSX_BUNDLE_BUNDLE_NAME H89
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    )
    target_compile_options(H89 PRIVATE -fobjc-arc ${H89_WARNINGS})
    # NSOpenGLView is deprecated but still supported; Metal is a later option.
    target_compile_definitions(H89 PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(H89 PRIVATE h89core h89render "-framework Cocoa")
endif()

# --- GLFW app (Windows; optional elsewhere) ------------------------------------
if(H89_GLFW_FRONTEND)
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)

    add_executable(H89-glfw platform/glfw/main.cpp)
    target_compile_options(H89-glfw PRIVATE ${H89_WARNINGS})
    target_link_libraries(H89-glfw PRIVATE h89core h89render glfw)
    if(WIN32)
        set_target_properties(H89-glfw PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(H89-glfw PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
endif()

# --- Developer tools ------------------------------------------------------------
add_executable(z80dis tools/z80dis.cpp)
target_compile_options(z80dis PRIVATE ${H89_WARNINGS})
target_link_libraries(z80dis PRIVATE h89core)

add_executable(h89run tools/h89run.cpp)
target_compile_options(h89run PRIVATE ${H89_WARNINGS})
target_link_libraries(h89run PRIVATE h89core)

add_executable(h8dtool tools/h8dtool.cpp)
target_compile_options(h8dtool PRIVATE ${H89_WARNINGS})
target_link_libraries(h8dtool PRIVATE h89core)

# --- Tests --------------------------------------------------------------------
if(H89_BUILD_TESTS)
    enable_testing()

    add_executable(z80_tests tests/z80_tests.cpp)
    target_compile_options(z80_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(z80_tests PRIVATE h89core)
    add_test(NAME z80_tests COMMAND z80_tests)

    add_executable(machine_tests tests/machine_tests.cpp)
    target_compile_options(machine_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(machine_tests PRIVATE h89core)
    add_test(NAME machine_tests COMMAND machine_tests)

    add_executable(diskfiles_tests tests/diskfiles_tests.cpp)
    target_compile_options(diskfiles_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(diskfiles_tests PRIVATE h89core)
    add_test(NAME diskfiles_tests COMMAND diskfiles_tests)

    # Boot tests against real ROMs, registered only when they are present in
    # roms/ (they are copyrighted and not distributed).
    # Finds one raw ROM image (not an Intel HEX copy) matching `pattern`.
    function(h89_find_rom var pattern)
        file(GLOB matches ${CMAKE_SOURCE_DIR}/roms/${pattern})
        list(FILTER matches EXCLUDE REGEX "\\.[hH][eE][xX]$")
        list(SORT matches)
        list(LENGTH matches n)
        if(n GREATER 0)
            list(GET matches 0 first)
            set(${var} ${first} PARENT_SCOPE)
        else()
            set(${var} "" PARENT_SCOPE)
        endif()
    endfunction()
    h89_find_rom(H89_MTR90_ROM *444-142*)
    h89_find_rom(H89_MTR89_ROM *444-62*)
    h89_find_rom(H89_H17_ROM *444-19*)
    if(H89_MTR90_ROM AND H89_H17_ROM)
        add_test(NAME boot_mtr90 COMMAND h89run --rom ${H89_MTR90_ROM} --h17rom ${H89_H17_ROM}
                 --seconds 2 --expect "H: ")
        add_test(NAME mtr90_substitute COMMAND h89run --rom ${H89_MTR90_ROM} --h17rom ${H89_H17_ROM}
                 --seconds 4 --type "S0000\\r" --expect "000000 303")
    endif()
    add_executable(h19_tests tests/h19_tests.cpp)
    target_compile_options(h19_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(h19_tests PRIVATE h89core)
    h89_find_rom(H89_H19_ROM *444-46*)
    h89_find_rom(H89_H19_FONT *444-29*)
    if(H89_H19_ROM AND H89_H19_FONT)
        add_test(NAME h19_tests COMMAND h19_tests ${CMAKE_SOURCE_DIR}/roms)
        if(H89_MTR90_ROM)
            add_test(NAME boot_h89_with_h19 COMMAND h89run --rom ${H89_MTR90_ROM} --h17rom ${H89_H17_ROM}
                     --h19rom ${H89_H19_ROM} --font ${H89_H19_FONT}
                     --seconds 5 --type "S0000\\r" --expect "000000 303")
        endif()
    endif()
    # Disk boot tests: images in disks/ are mounted read-only.
    file(GLOB H89_HDOS_DISK ${CMAKE_SOURCE_DIR}/disks/*HDOS*.[hH]8[dD])
    file(GLOB H89_CPM_DISK ${CMAKE_SOURCE_DIR}/disks/*CPM*.[hH]8[dD])
    if(H89_MTR90_ROM AND H89_H17_ROM AND H89_H19_ROM AND H89_H19_FONT)
        set(H89_FULL_MACHINE --rom ${H89_MTR90_ROM} --h17rom ${H89_H17_ROM}
            --h19rom ${H89_H19_ROM} --font ${H89_H19_FONT})
        if(H89_HDOS_DISK)
            list(GET H89_HDOS_DISK 0 disk)
            add_test(NAME boot_hdos COMMAND h89run ${H89_FULL_MACHINE} --ro --disk0 ${disk}
                     --seconds 120 --step "H: |B\\r" --step "ACTION?|\\r"
                     --step ")?|14-Oct-86\\r" --step ">|CAT\\r" --expect "Files, Using")
        endif()
        if(H89_CPM_DISK)
            list(GET H89_CPM_DISK 0 disk)
            add_test(NAME boot_cpm COMMAND h89run ${H89_FULL_MACHINE} --ro --disk0 ${disk}
                     --seconds 60 --step "H: |B\\r" --step "A>|DIR\\r" --expect "PIP      COM")
        endif()
    endif()
    if(H89_MTR89_ROM AND H89_H17_ROM)
        add_test(NAME boot_mtr89 COMMAND h89run --rom ${H89_MTR89_ROM} --h17rom ${H89_H17_ROM}
                 --seconds 2 --expect "H: ")
    endif()

    add_executable(zexrun tests/zexrun.cpp)
    target_compile_options(zexrun PRIVATE ${H89_WARNINGS})
    target_link_libraries(zexrun PRIVATE h89core)

    # Instruction exercisers: fetch with tools/fetch_zex.sh. Slow (~minutes),
    # so they carry the "slow" label: ctest -LE slow skips them.
    foreach(zex zexdoc zexall)
        set(zexfile ${CMAKE_SOURCE_DIR}/tests/data/${zex}.com)
        if(EXISTS ${zexfile})
            add_test(NAME ${zex} COMMAND zexrun ${zexfile})
            set_tests_properties(${zex} PROPERTIES LABELS slow TIMEOUT 1800)
        endif()
    endforeach()
endif()
