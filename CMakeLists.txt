cmake_minimum_required(VERSION 3.10)
project(WinnerZ)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

if(MSVC)
    add_compile_options(/bigobj)
    
    # Disable Whole Program Optimization (LTCG) to prevent "Generating Code..." crashes and OOM on 32-bit GitHub Actions
    string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/GL" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/LTCG" "" CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
    string(REPLACE "/LTCG" "" CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}")
    string(REPLACE "/LTCG" "" CMAKE_MODULE_LINKER_FLAGS_RELEASE "${CMAKE_MODULE_LINKER_FLAGS_RELEASE}")
    
    # Also for MinSizeRel and RelWithDebInfo just in case
    string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
    string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

if(WIN32)
    option(WINNERZ_BUILD_PYTHON_WRAPPER "Build Python module (Pybind11)" ON)
else()
    option(WINNERZ_BUILD_PYTHON_WRAPPER "Build Python module (Pybind11)" ON)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

set(WINNERZ_FREETYPE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extract_text/free-type/freetype-2.14.3")
if(EXISTS "${WINNERZ_FREETYPE_DIR}/CMakeLists.txt")
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

    add_subdirectory("${WINNERZ_FREETYPE_DIR}" EXCLUDE_FROM_ALL)
    if(TARGET freetype)
        set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

include(FetchContent)
FetchContent_Declare(
    harfbuzz
    URL https://github.com/harfbuzz/harfbuzz/releases/download/8.3.0/harfbuzz-8.3.0.tar.xz
)
FetchContent_MakeAvailable(harfbuzz)

add_library(WinnerZ STATIC
    redactor/redactor.cpp
    src/preview_pdf.cpp
    extract_text/extractor_logic.cpp
    extract_text/pdf_engine.cpp
    extract_text/parse.cpp
    extract_text/xref.cpp
    extract_text/cmap_parse.cpp
    extract_text/cmap.cpp
    extract_text/cmap_table.cpp
    extract_text/unicode.cpp
    extract_text/type_3.cpp
    extract_text/decoder_inflate.cpp
    extract_text/bidi.cpp
    extract_text/bidi_std.cpp
    extract_text/ucdn.cpp
    extract_text/miniz.c
    extract_text/micro_ocr.cpp
    extract_text/pdf_font_metrics.cpp
)
set_target_properties(WinnerZ PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(WinnerZ PUBLIC cxx_std_17)
if(WIN32)
    target_compile_definitions(WinnerZ PUBLIC NOMINMAX)
endif()
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        ZLIB
        URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.zip
    )
    FetchContent_MakeAvailable(ZLIB)
    if(TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        set(ZLIB_FOUND TRUE)
        target_include_directories(WinnerZ PUBLIC "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    elseif(TARGET zlib)
        add_library(ZLIB::ZLIB ALIAS zlib)
        set(ZLIB_FOUND TRUE)
        target_include_directories(WinnerZ PUBLIC "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    endif()
endif()
target_include_directories(WinnerZ PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/redactor"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/extract_text"
)

if(UNIX AND NOT APPLE)
    find_library(
        WINNERZ_LCMS2_LIB
        NAMES lcms2 lcms2.so.2 liblcms2.so.2
        PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
    )
endif()

if(WINNERZ_LCMS2_LIB)
    target_link_libraries(WinnerZ PUBLIC "${WINNERZ_LCMS2_LIB}")
    target_compile_definitions(WinnerZ PRIVATE WINNERZ_USE_LCMS2=1)
else()
    target_compile_definitions(WinnerZ PRIVATE WINNERZ_USE_LCMS2=0)
endif()

set(PDFIUM_VERSION "7891")

if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-win-arm64.tgz")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-win-x86.tgz")
    else()
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-win-x64.tgz")
    endif()
    set(PDFIUM_LINK_LIB "pdfium.dll.lib")
    set(PDFIUM_SHARED_LIB "bin/pdfium.dll")
elseif(APPLE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-mac-arm64.tgz")
    else()
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-mac-x64.tgz")
    endif()
    set(PDFIUM_LINK_LIB "libpdfium.dylib")
    set(PDFIUM_SHARED_LIB "lib/libpdfium.dylib")
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-linux-arm64.tgz")
    else()
        set(PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_VERSION}/pdfium-linux-x64.tgz")
    endif()
    set(PDFIUM_LINK_LIB "libpdfium.so")
    set(PDFIUM_SHARED_LIB "lib/libpdfium.so")
endif()

include(FetchContent)
FetchContent_Declare(
    pdfium
    URL ${PDFIUM_URL}
)
FetchContent_MakeAvailable(pdfium)

if(APPLE)
    execute_process(COMMAND chmod u+w "${pdfium_SOURCE_DIR}/${PDFIUM_SHARED_LIB}")
    execute_process(COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${pdfium_SOURCE_DIR}/${PDFIUM_SHARED_LIB}")
endif()

set(WINNERZ_PDFIUM_INCLUDE_DIR "${pdfium_SOURCE_DIR}/include")
if(WIN32)
    set(WINNERZ_PDFIUM_LIB "${pdfium_SOURCE_DIR}/lib/${PDFIUM_LINK_LIB}")
else()
    set(WINNERZ_PDFIUM_LIB "${pdfium_SOURCE_DIR}/${PDFIUM_SHARED_LIB}")
endif()
set(WINNERZ_PDFIUM_SHARED_FILE "${pdfium_SOURCE_DIR}/${PDFIUM_SHARED_LIB}")

target_sources(WinnerZ PRIVATE src/drawing.cpp insert_text/insert.cpp)
target_include_directories(WinnerZ PUBLIC "${WINNERZ_PDFIUM_INCLUDE_DIR}")
target_link_libraries(WinnerZ PUBLIC "${WINNERZ_PDFIUM_LIB}")
target_compile_definitions(WinnerZ PUBLIC WINNERZ_USE_PDFIUM_PREVIEW=1)

if(TARGET freetype)
    target_link_libraries(WinnerZ PUBLIC nlohmann_json::nlohmann_json freetype harfbuzz harfbuzz-subset)
    target_compile_definitions(WinnerZ PRIVATE WINEXTRACT_USE_FREETYPE=1)
else()
    target_link_libraries(WinnerZ PUBLIC nlohmann_json::nlohmann_json harfbuzz)
endif()

if(ZLIB_FOUND)
    target_link_libraries(WinnerZ PUBLIC ZLIB::ZLIB)
endif()


if(WINNERZ_BUILD_PYTHON_WRAPPER)
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            pybind11
            URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.zip
        )
        FetchContent_MakeAvailable(pybind11)
    endif()

    pybind11_add_module(winnerz_core wrapper_python/winnerz_python_wrapper.cpp)
    target_compile_features(winnerz_core PRIVATE cxx_std_17)
    

    
    target_include_directories(winnerz_core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(winnerz_core PRIVATE WinnerZ)
    
    add_custom_command(TARGET winnerz_core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINNERZ_PDFIUM_SHARED_FILE}"
            "$<TARGET_FILE_DIR:winnerz_core>"
    )
    
    if(UNIX AND NOT APPLE)
        target_link_options(winnerz_core PRIVATE "-Wl,--allow-multiple-definition")
    endif()
    
    if(APPLE)
        set_target_properties(winnerz_core PROPERTIES
            INSTALL_RPATH "@loader_path"
            BUILD_RPATH "@loader_path"
        )
        # Avoid build failure if the library name doesn't exactly match
        add_custom_command(TARGET winnerz_core POST_BUILD
            COMMAND install_name_tool -change "./libpdfium.dylib" "@loader_path/libpdfium.dylib" "$<TARGET_FILE:winnerz_core>" || true
            COMMAND install_name_tool -change "libpdfium.dylib" "@loader_path/libpdfium.dylib" "$<TARGET_FILE:winnerz_core>" || true
            COMMAND install_name_tool -change "@rpath/libpdfium.dylib" "@loader_path/libpdfium.dylib" "$<TARGET_FILE:winnerz_core>" || true
        )
    endif()

    if(UNIX AND NOT APPLE)
        set_target_properties(winnerz_core PROPERTIES
            PREFIX ""
            OUTPUT_NAME "winnerz_core"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python"
            BUILD_RPATH "\$ORIGIN;\$ORIGIN/../third-party/pdfium/lib"
            INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../third-party/pdfium/lib"
            BUILD_RPATH_USE_ORIGIN ON
        )

        file(GLOB WINNERZ_STALE_WINDOWS_ARTIFACTS
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python/winnerz_core*.pyd"
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/python/winnerz_core*.pdb"
        )

        if(WINNERZ_STALE_WINDOWS_ARTIFACTS)
            add_custom_command(TARGET winnerz_core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rm -f ${WINNERZ_STALE_WINDOWS_ARTIFACTS}
                VERBATIM
            )
        endif()
    endif()

    install(TARGETS winnerz_core DESTINATION winnerz)
    install(FILES winnerz/python/__init__.py DESTINATION winnerz)
    install(FILES winnerz/python/winnerz_core.pyi DESTINATION winnerz)
    install(FILES "${WINNERZ_PDFIUM_SHARED_FILE}" DESTINATION winnerz)
endif()

if(EMSCRIPTEN)
    # ─── WASM build using Emscripten Embind ───────────────────────────────────
    # Build command (run in WSL after sourcing emsdk_env.sh):
    #   mkdir -p build-wasm && cd build-wasm
    #   emcmake cmake .. -DWINNERZ_BUILD_PYTHON_WRAPPER=OFF
    #   emmake make winnerz_wasm -j4
    # Output goes to winnerz/js/winnerz_wasm.{js,wasm}
    # ─────────────────────────────────────────────────────────────────────────

    add_executable(winnerz_wasm wrapper_wasm/winnerz_wasm_embind.cpp)

    target_compile_features(winnerz_wasm PRIVATE cxx_std_17)

    target_include_directories(winnerz_wasm PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(winnerz_wasm PRIVATE WinnerZ)

    # Output: winnerz/js/winnerz_wasm.js  +  winnerz/js/winnerz_wasm.wasm
    set_target_properties(winnerz_wasm PROPERTIES
        OUTPUT_NAME "winnerz_wasm"
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/js"
    )

    # Emscripten linker flags
    target_link_options(winnerz_wasm PRIVATE
        # Embind – exposes C++ classes to JS
        "-lembind"

        # Modularize output so it can be imported as an ES module or CommonJS
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createWinnerzModule"

        # WASM output
        "-sWASM=1"

        # Allow heap to grow for large PDFs (up to 512 MB)
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=536870912"  # 512 MB

        # Initial memory (16 MB)
        "-sINITIAL_MEMORY=16777216"

        # No main() entry point – this is a library
        "--no-entry"

        # Export the module factory
        "-sEXPORTED_RUNTIME_METHODS=['FS','HEAP8','HEAPU8']"

        # Disable filesystem (we load PDFs from memory)
        "-sFORCE_FILESYSTEM=0"

        # Enable exception handling (needed for std::runtime_error)
        "-fexceptions"
        "-sDISABLE_EXCEPTION_CATCHING=0"

        # Emscripten provided libraries (ports)
        "-sUSE_FREETYPE=1"
        "-sUSE_HARFBUZZ=1"
        "-sUSE_ZLIB=1"
        "-sUSE_LIBPNG=1"

        # Generate separate .wasm file (better browser caching)
        "-sSINGLE_FILE=0"
    )

    # Compile with exceptions enabled and Emscripten ports
    target_compile_options(winnerz_wasm PRIVATE 
        "-fexceptions"
        "-sUSE_FREETYPE=1"
        "-sUSE_HARFBUZZ=1"
        "-sUSE_ZLIB=1"
        "-sUSE_LIBPNG=1"
    )

    # In WASM, we do NOT use PDFium (no binaries), so skip pdfium features
    target_compile_definitions(winnerz_wasm PRIVATE
        WINNERZ_USE_PDFIUM_PREVIEW=0
        WINNERZ_WASM_BUILD=1
    )

    # Copy outputs to winnerz/js for distribution
    add_custom_command(TARGET winnerz_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:winnerz_wasm>/winnerz_wasm.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/js/winnerz_wasm.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:winnerz_wasm>/winnerz_wasm.wasm"
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/js/winnerz_wasm.wasm"
        VERBATIM
    )
endif()

