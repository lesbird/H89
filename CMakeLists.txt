cmake_minimum_required(VERSION 3.21)
project(H89 VERSION 1.1.0 LANGUAGES C CXX)

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
option(H89_DEBUGGER "Build the Dear ImGui debugger into the front ends" ON)

set(H89_VENDOR "Les Bird")
set(H89_COPYRIGHT "Copyright (C) 2026 Les Bird")

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
    core/devices/H37.cpp
    core/devices/Ins8250.cpp
    core/media/DiskFiles.cpp
    core/media/H17Disk.cpp
    core/media/SoftDisk.cpp
    core/terminal/Crtc6845.cpp
    core/terminal/H19.cpp
    core/DebugControl.cpp
    core/Machine.cpp
    core/RomSet.cpp
)
target_include_directories(h89core PUBLIC core)
target_compile_options(h89core PRIVATE ${H89_WARNINGS})

# --- Host links: serial ports to files and TCP (portable sockets) ------------
add_library(h89host STATIC
    host/AppFiles.cpp
    host/SerialHost.cpp
    host/FileTransfer.cpp
    host/SerialPort.cpp
    host/TerminalLink.cpp
)
target_include_directories(h89host PUBLIC host)
target_link_libraries(h89host PUBLIC h89core)
target_compile_options(h89host PRIVATE ${H89_WARNINGS})
if(WIN32)
    target_link_libraries(h89host PUBLIC ws2_32 shell32 advapi32)
endif()

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

# --- Debugger: Dear ImGui panels shared by the front ends --------------------
if(H89_DEBUGGER)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    add_library(h89debug STATIC
        debug/DebuggerUI.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(h89debug PUBLIC debug ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(h89debug PUBLIC h89core)
    target_compile_definitions(h89debug PUBLIC H89_DEBUGGER=1)
    if(APPLE)
        target_compile_definitions(h89debug PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endif()

# --- Bundled ROMs and disks -----------------------------------------------------
# The apps ship with the ROMs (raw images only) and the starter disks in roms/
# and disks/.
file(GLOB H89_BUNDLED_ROMS ${CMAKE_SOURCE_DIR}/roms/*.[bB][iI][nN] ${CMAKE_SOURCE_DIR}/roms/*.[rR][oO][mM])
file(GLOB H89_BUNDLED_DISKS ${CMAKE_SOURCE_DIR}/disks/*.[hH]8[dD] ${CMAKE_SOURCE_DIR}/disks/*.[hH]37
     ${CMAKE_SOURCE_DIR}/disks/*.[iI][mM][dD])

# --- macOS app (Objective-C++) ------------------------------------------------
if(APPLE)
    add_executable(H89 MACOSX_BUNDLE
        platform/mac/main.mm
        platform/mac/AppDelegate.mm
        platform/mac/H89View.mm
        platform/mac/H89Emulator.mm
        platform/mac/H89DiskBrowser.mm
        platform/mac/H89Preferences.mm
        platform/mac/H89TransferPanel.mm
        platform/mac/H89.icns
        ${H89_BUNDLED_ROMS}
        ${H89_BUNDLED_DISKS}
    )
    set_source_files_properties(platform/mac/H89.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(${H89_BUNDLED_ROMS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/roms)
    set_source_files_properties(${H89_BUNDLED_DISKS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/disks)
    set_target_properties(H89 PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/platform/mac/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.h89emu.H89
        MACOSX_BUNDLE_ICON_FILE H89.icns
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 ${H89_VENDOR}"
        MACOSX_BUNDLE_BUNDLE_NAME H89
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    )
    target_compile_options(H89 PRIVATE -fobjc-arc ${H89_WARNINGS})
    # NSOpenGLView is deprecated but still supported; Metal is a later option.
    target_compile_definitions(H89 PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(H89 PRIVATE h89host h89render "-framework Cocoa")
    if(H89_DEBUGGER)
        target_sources(H89 PRIVATE platform/mac/H89DebuggerView.mm)
        target_link_libraries(H89 PRIVATE h89debug "-framework QuartzCore")
    endif()
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
    target_link_libraries(H89-glfw PRIVATE h89host h89render glfw)
    if(H89_DEBUGGER)
        target_sources(H89-glfw PRIVATE
            platform/glfw/SettingsPanel.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
        target_link_libraries(H89-glfw PRIVATE h89debug)
    endif()
    if(WIN32)
        configure_file(platform/glfw/H89.rc.in ${CMAKE_BINARY_DIR}/H89.rc @ONLY)
        target_sources(H89-glfw PRIVATE ${CMAKE_BINARY_DIR}/H89.rc)
        set_target_properties(H89-glfw PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME H89)
        target_link_libraries(H89-glfw PRIVATE winmm comdlg32 ole32 shell32)
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
target_link_libraries(h89run PRIVATE h89host)

add_executable(h8dtool tools/h8dtool.cpp)
target_compile_options(h8dtool PRIVATE ${H89_WARNINGS})
target_link_libraries(h8dtool PRIVATE h89core)

# --- Packaging ------------------------------------------------------------------
# macOS: tools/package_mac.sh signs, notarizes and wraps H89.app in a .dmg.
# Windows: cpack -C Release builds an installer (NSIS) and a .zip holding
# H89.exe, the command-line tools, roms\ and disks\.
if(APPLE)
    install(TARGETS H89 BUNDLE DESTINATION .)
elseif(H89_GLFW_FRONTEND)
    install(TARGETS H89-glfw h89run h8dtool RUNTIME DESTINATION .)
    install(FILES ${H89_BUNDLED_ROMS} DESTINATION roms)
    install(FILES ${H89_BUNDLED_DISKS} DESTINATION disks)
    install(FILES README.md DESTINATION .)
endif()

set(CPACK_PACKAGE_NAME "H89")
set(CPACK_PACKAGE_VENDOR "${H89_VENDOR}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Heathkit H89 emulator")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/lesbird/H89")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "H89")
set(CPACK_PACKAGE_EXECUTABLES "H89;H89 Emulator")
set(CPACK_PACKAGE_FILE_NAME "H89-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_DISPLAY_NAME "H89 Emulator")
    set(CPACK_NSIS_PACKAGE_NAME "H89 Emulator ${PROJECT_VERSION}")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/platform/glfw/H89.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/platform/glfw/H89.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "H89.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "H89.exe")
    set(CPACK_CREATE_DESKTOP_LINKS "H89")
else()
    set(CPACK_GENERATOR "ZIP")
endif()
include(CPack)

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

    add_executable(appfiles_tests tests/appfiles_tests.cpp)
    target_compile_options(appfiles_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(appfiles_tests PRIVATE h89host)
    add_test(NAME appfiles_tests COMMAND appfiles_tests)

    add_executable(transfer_tests tests/transfer_tests.cpp)
    target_compile_options(transfer_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(transfer_tests PRIVATE h89host)
    add_test(NAME transfer_tests COMMAND transfer_tests)

    add_executable(h37_tests tests/h37_tests.cpp)
    target_compile_options(h37_tests PRIVATE ${H89_WARNINGS})
    target_link_libraries(h37_tests PRIVATE h89core)
    add_test(NAME h37_tests COMMAND h37_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    # Boot tests against the ROMs in roms/, registered only when they are
    # there.
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
        if(NOT WIN32)
            add_executable(terminal_link_tests tests/terminal_link_tests.cpp)
            target_compile_options(terminal_link_tests PRIVATE ${H89_WARNINGS})
            target_link_libraries(terminal_link_tests PRIVATE h89host)
            if(NOT APPLE)
                target_link_libraries(terminal_link_tests PRIVATE util)
            endif()
            add_test(NAME terminal_link_tests COMMAND terminal_link_tests ${CMAKE_SOURCE_DIR}/roms)
        endif()
        add_executable(state_tests tests/state_tests.cpp)
        target_compile_options(state_tests PRIVATE ${H89_WARNINGS})
        target_link_libraries(state_tests PRIVATE h89core)
        add_test(NAME state_tests COMMAND state_tests ${CMAKE_SOURCE_DIR}/roms ${CMAKE_SOURCE_DIR}/disks)
        if(H89_MTR90_ROM)
            add_test(NAME boot_h89_with_h19 COMMAND h89run --rom ${H89_MTR90_ROM} --h17rom ${H89_H17_ROM}
                     --h19rom ${H89_H19_ROM} --font ${H89_H19_FONT}
                     --seconds 5 --type "S0000\\r" --expect "000000 303")
        endif()
    endif()
    # Disk boot tests: images in disks/ are mounted read-only.
    file(GLOB H89_HDOS_DISK ${CMAKE_SOURCE_DIR}/disks/*HDOS*.[hH]8[dD])
    file(GLOB H89_CPM_DISK ${CMAKE_SOURCE_DIR}/disks/*CPM*.[hH]8[dD])
    file(GLOB H89_H37_HDOS_DISK ${CMAKE_SOURCE_DIR}/disks/*HDOS*.[hH]37)
    file(GLOB H89_H37_CPM_DISK ${CMAKE_SOURCE_DIR}/disks/*CPM*.[hH]37)
    if(H89_MTR90_ROM AND H89_H17_ROM AND H89_H19_ROM AND H89_H19_FONT)
        set(H89_FULL_MACHINE --rom ${H89_MTR90_ROM} --h17rom ${H89_H17_ROM}
            --h19rom ${H89_H19_ROM} --font ${H89_H19_FONT})
        if(H89_HDOS_DISK)
            list(GET H89_HDOS_DISK 0 disk)
            add_test(NAME boot_hdos COMMAND h89run ${H89_FULL_MACHINE} --ro --disk0 ${disk}
                     --seconds 120 --step "H: |B\\r" --step "ACTION?|\\r"
                     --step ")?|14-Oct-86\\r" --step ">|CAT\\r" --expect "Files, Using")
        endif()
        # H37 boots: SW501 30 = boot from the device at 170Q (78h).
        if(H89_H37_HDOS_DISK)
            list(GET H89_H37_HDOS_DISK 0 disk)
            add_test(NAME boot_h37_hdos COMMAND h89run ${H89_FULL_MACHINE} --sw501 30 --ro --dk0 ${disk}
                     --seconds 120 --step "H: |B\\r" --step "ACTION?|\\r"
                     --step ")?|14-Oct-86\\r" --step ">|CAT\\r" --expect "Files, Using")
        endif()
        if(H89_H37_CPM_DISK)
            list(GET H89_H37_CPM_DISK 0 disk)
            add_test(NAME boot_h37_cpm COMMAND h89run ${H89_FULL_MACHINE} --sw501 30 --ro --dk0 ${disk}
                     --seconds 60 --step "H: |B\\r" --step "A>|DIR\\r" --expect "PIP      COM")
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
