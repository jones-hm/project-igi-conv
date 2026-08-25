cmake_minimum_required(VERSION 3.16)

project(igi1conv
  VERSION 1.11.0
  DESCRIPTION "Project IGI 1 Game Convertor"
  LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release")
endif ()
message("CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-D_DEBUG)
endif ()

if (WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
endif()

if (MSVC)
    add_compile_options("/FS")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ─── shared engine subset (parsers + headers reused from the IGI editor) ──────
file(GLOB SOURCES_PARSERS "source/parsers/*.cpp" "source/parsers/*.h")

# ─── igi1conv CLI ───────────────────────────────────────────────────────────────
file(GLOB IGI1CONV_SOURCES "igi1conv/*.cpp" "igi1conv/*.h")
list(APPEND IGI1CONV_SOURCES
    "igi1conv/command_dispatch.cpp"
    "igi1conv/mcp_operations.cpp"
    "igi1conv/mcp_protocol.cpp"
    "igi1conv/mcp_execution.cpp"
    "igi1conv/mcp_transport.cpp"
)
list(REMOVE_DUPLICATES IGI1CONV_SOURCES)
if (WIN32)
    list(APPEND IGI1CONV_SOURCES "assets/igi1conv.rc")
    list(APPEND IGI1CONV_SOURCES "assets/resources.qrc")
endif()

add_executable(igi1conv ${IGI1CONV_SOURCES} ${SOURCES_PARSERS})

set_target_properties(igi1conv PROPERTIES
    OUTPUT_NAME "igi1conv"
)
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(igi1conv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release32"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${PROJECT_SOURCE_DIR}/bin/Debug32"
    )
else()
    set_target_properties(igi1conv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${PROJECT_SOURCE_DIR}/bin/Debug"
    )
endif()

# --- GUI Dependencies (Qt5/Qt6) ---
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    message("Configuring 32-bit (Win32) build using local Qt 5.15.2")
    set(CMAKE_PREFIX_PATH "${PROJECT_SOURCE_DIR}/5.15.2/msvc2019")
    find_package(Qt5 COMPONENTS Core Gui Widgets Network OpenGL REQUIRED)
    target_link_libraries(igi1conv PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::OpenGL)
else()
    message("Configuring 64-bit (x64) build using system Qt 6.5.3")
    set(CMAKE_PREFIX_PATH "D:/Qt/6.5.3/msvc2019_64")
    find_package(Qt6 COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets REQUIRED)
    target_link_libraries(igi1conv PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets)
endif()
# ---------------------------------------

# Single source of truth for the version string (also stamped in assets/igi1conv.rc).
target_compile_definitions(igi1conv PRIVATE IGI1CONV_VERSION="${PROJECT_VERSION}")

target_include_directories(igi1conv PRIVATE
    ${PROJECT_SOURCE_DIR}/igi1conv          # igi1conv/pch.h, cmd_*.h
    ${PROJECT_SOURCE_DIR}/source         # common.h, logger.h, utils.h, config.h, level/, renderer/
    ${PROJECT_SOURCE_DIR}/source/parsers
    ${PROJECT_SOURCE_DIR}/third_party/glm-1.0.1
    ${PROJECT_SOURCE_DIR}/third_party
)

if (MSVC)
    # igi1conv uses no PCH; parsers #include "pch.h" expecting it force-included.
    # /FI force-includes igi1conv/pch.h (no GL/app deps) into every TU.
    target_compile_options(igi1conv PRIVATE
        "/bigobj" "/FS"
        "/FI${PROJECT_SOURCE_DIR}/igi1conv/pch.h"
    )
    set_target_properties(igi1conv PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()

# Copy required runtime assets (JSON databases and autocomplete text) to target directory
add_custom_command(TARGET igi1conv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/IGIModels.json"
        "${PROJECT_SOURCE_DIR}/assets/IGIModelsAllLevel.json"
        "${PROJECT_SOURCE_DIR}/assets/IGIAutoComplete.txt"
        $<TARGET_FILE_DIR:igi1conv>
)

# Keep Release/Debug binaries runnable when launched directly from the build
# output (Explorer, CTest, or a copied test bundle).  Qt DLLs are not found
# from the executable's link metadata, so relying on the caller's PATH makes
# the otherwise valid test executable fail with 0xc0000135.
if (WIN32)
    option(IGI1CONV_REQUIRE_QT_DEPLOYMENT
        "Fail configuration when a standalone Qt runtime cannot be deployed" ON)
    find_program(IGI1CONV_WINDEPLOYQT
        NAMES windeployqt windeployqt.exe
        HINTS "${CMAKE_PREFIX_PATH}/bin" "${CMAKE_PREFIX_PATH}"
    )
    if (IGI1CONV_WINDEPLOYQT)
        add_custom_command(TARGET igi1conv POST_BUILD
            COMMAND "${IGI1CONV_WINDEPLOYQT}"
                "$<IF:$<CONFIG:Debug>,--debug,--release>"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:igi1conv>"
            COMMENT "Deploying Qt runtime beside igi1conv"
            VERBATIM
        )
    elseif (IGI1CONV_REQUIRE_QT_DEPLOYMENT)
        message(FATAL_ERROR
            "windeployqt is required for standalone Windows binaries; "
            "set IGI1CONV_REQUIRE_QT_DEPLOYMENT=OFF only when Qt is guaranteed on PATH")
    else()
        message(WARNING "windeployqt not found; built executables require Qt DLLs on PATH")
    endif()
endif()

# ─── tests ───────────────────────────────────────────────────────────────────
option(IGI1CONV_BUILD_TESTS "Build igi1conv test suite" ON)

if (IGI1CONV_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()

    file(GLOB IGI1CONV_TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
    add_executable(igi1conv_tests ${IGI1CONV_TEST_SOURCES})

    # Keep structural source-contract tests independent of the process
    # working directory.  The test executable is also deployed beside a live
    # game corpus for validation, so its cwd is not necessarily the checkout.
    target_compile_definitions(igi1conv_tests PRIVATE
        IGI1CONV_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )

    add_dependencies(igi1conv_tests igi1conv)

    if (MSVC)
        set_target_properties(igi1conv_tests PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
    endif()

    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set_target_properties(igi1conv_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release32"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${PROJECT_SOURCE_DIR}/bin/Debug32"
        )
    else()
        set_target_properties(igi1conv_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${PROJECT_SOURCE_DIR}/bin/Debug"
        )
    endif()

    target_link_libraries(igi1conv_tests PRIVATE gtest)

    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_libraries(igi1conv_tests PRIVATE Qt5::Core Qt5::Network)
    else()
        target_link_libraries(igi1conv_tests PRIVATE Qt6::Core Qt6::Network)
    endif()

    target_include_directories(igi1conv_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/igi1conv
        ${PROJECT_SOURCE_DIR}/source
        ${PROJECT_SOURCE_DIR}/source/parsers
        ${PROJECT_SOURCE_DIR}/third_party/glm-1.0.1
        ${PROJECT_SOURCE_DIR}/third_party
    )

    target_sources(igi1conv_tests PRIVATE
        igi1conv/mcp_operations.cpp
        igi1conv/mcp_protocol.cpp
        igi1conv/mcp_transport.cpp
        source/parsers/qsc_object_editor.cpp
    )

    include(GoogleTest)
    gtest_discover_tests(igi1conv_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    find_program(IGI1CONV_POWERSHELL
        NAMES pwsh powershell
    )
    if (IGI1CONV_POWERSHELL)
        add_test(NAME McpDocumentation
            COMMAND "${IGI1CONV_POWERSHELL}" -NoProfile -File
                    "${PROJECT_SOURCE_DIR}/tests/mcp_docs_check.ps1"
        )
        set_tests_properties(McpDocumentation PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )

        set(IGI1CONV_GAME_PATH "$ENV{IGI_GAME_PATH}" CACHE PATH
            "Optional game corpus path for the live MCP/CLI matrix")
        if (IGI1CONV_GAME_PATH
            AND EXISTS "${PROJECT_SOURCE_DIR}/tests/live_igi1_matrix.ps1")
            add_test(NAME McpLiveIgi1Matrix
                COMMAND "${IGI1CONV_POWERSHELL}" -NoProfile -File
                        "${PROJECT_SOURCE_DIR}/tests/live_igi1_matrix.ps1"
                        -GamePath "${IGI1CONV_GAME_PATH}"
                        -Executable "$<TARGET_FILE:igi1conv>"
            )
            set_tests_properties(McpLiveIgi1Matrix PROPERTIES
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                TIMEOUT 900
            )
        endif()
    endif()
endif()
