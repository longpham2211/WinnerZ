cmake_minimum_required(VERSION 3.18)
project(WinnerZ_WASM C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Nlohmann JSON (Header Only) ─────────────────────────────────────────────
# Tải nlohmann_json bằng Git để tránh lỗi bung file .tar.xz
include(FetchContent)
FetchContent_Declare(
    lcms2
    URL "file://${CMAKE_CURRENT_SOURCE_DIR}/../Little-CMS-lcms2.19.1.tar.gz"
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
FetchContent_MakeAvailable(lcms2)


include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    libjpeg-turbo
    URL "file://${CMAKE_CURRENT_SOURCE_DIR}/../libjpeg-turbo-3.0.3.tar.gz"
    PATCH_COMMAND sed -i "s/message(FATAL_ERROR \\\"The libjpeg-turbo build system cannot be integrated into another build system using add_subdirectory().  Use ExternalProject_Add() instead.\\\")/message(STATUS \\\"Skipped fatal error\\\")/g" CMakeLists.txt
)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(WITH_SIMD OFF CACHE BOOL "" FORCE)
set(WITH_TURBOJPEG ON CACHE BOOL "" FORCE)
set(CMAKE_INSTALL_DOCDIR "doc" CACHE STRING "" FORCE)
set(CMAKE_INSTALL_DEFAULT_DATAROOTDIR "share" CACHE STRING "" FORCE)
set(CMAKE_INSTALL_DEFAULT_MANDIR "share/man" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(libjpeg-turbo)

# ─── HarfBuzz (For Font Subsetting) ──────────────────────────────────────────
FetchContent_Declare(
    harfbuzz
    URL "file://${CMAKE_CURRENT_SOURCE_DIR}/../harfbuzz-8.3.0.tar.gz"
)
FetchContent_Populate(harfbuzz)
file(GLOB HB_SUBSET_SOURCES
    "${harfbuzz_SOURCE_DIR}/src/hb-subset*.cc"
    "${harfbuzz_SOURCE_DIR}/src/hb-repacker*.cc"
    "${harfbuzz_SOURCE_DIR}/src/hb-map*.cc"
    "${harfbuzz_SOURCE_DIR}/src/hb-set*.cc"
    "${harfbuzz_SOURCE_DIR}/src/hb-number*.cc"
)
add_library(harfbuzz-subset STATIC ${HB_SUBSET_SOURCES})
target_include_directories(harfbuzz-subset PRIVATE "${harfbuzz_SOURCE_DIR}/src")
target_compile_definitions(harfbuzz-subset PRIVATE HB_NO_MT)

# ─── WinnerZ Library ────────────────────────────────────────────────────────
add_library(WinnerZ_WASM_Lib STATIC
    ../redactor/redactor.cpp
    ../extract_text/extractor_logic.cpp
    ../extract_text/pdf_engine.cpp
    ../extract_text/parse.cpp
    ../extract_text/xref.cpp
    ../extract_text/cmap_parse.cpp
    ../extract_text/cmap.cpp
    ../extract_text/cmap_table.cpp
    ../extract_text/unicode.cpp
    ../extract_text/type_3.cpp
    ../extract_text/decoder_inflate.cpp
    ../extract_text/bidi.cpp
    ../extract_text/bidi_std.cpp
    ../extract_text/ucdn.cpp
    ../extract_text/miniz.c
    ../extract_text/micro_ocr.cpp
    ../extract_text/pdf_font_metrics.cpp
    ../insert_text/insert.cpp
    ../src/preview_pdf.cpp
    ../src/drawing.cpp
)

target_include_directories(WinnerZ_WASM_Lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../redactor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extract_text"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third-party/pdfium/include"
    "${libjpeg-turbo_SOURCE_DIR}"
    "${harfbuzz_SOURCE_DIR}/src"
)

# Bật PDFium Preview
if(NOT DEFINED WINNERZ_USE_PDFIUM_PREVIEW)
    set(WINNERZ_USE_PDFIUM_PREVIEW 1)
endif()
target_compile_definitions(WinnerZ_WASM_Lib PUBLIC
    WINNERZ_WASM_BUILD=1
    WINEXTRACT_USE_FREETYPE=1
    WINNERZ_USE_PDFIUM_PREVIEW=${WINNERZ_USE_PDFIUM_PREVIEW}
)

# Sử dụng thư viện chuẩn của Emscripten cho WASM (không cần download)
target_compile_options(WinnerZ_WASM_Lib PRIVATE
    "-fexceptions"
    "-sUSE_FREETYPE=1"
    "-sUSE_ZLIB=1"
    "-sUSE_LIBPNG=1"
    "-sDISABLE_EXCEPTION_CATCHING=0"
)

target_link_libraries(WinnerZ_WASM_Lib PUBLIC nlohmann_json::nlohmann_json turbojpeg-static harfbuzz-subset harfbuzz lcms2)

# ─── WASM Executable ─────────────────────────────────────────────────────────
add_executable(winnerz_wasm winnerz_wasm_embind.cpp)

target_include_directories(winnerz_wasm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(winnerz_wasm PRIVATE 
    WinnerZ_WASM_Lib
    "${CMAKE_CURRENT_SOURCE_DIR}/../winnerz/js/libpdfium-wasm.a"
)

set_target_properties(winnerz_wasm PROPERTIES
    OUTPUT_NAME "winnerz_wasm"
    SUFFIX ".js"
)

target_link_options(winnerz_wasm PRIVATE
    "-Wl,--allow-multiple-definition"
    "-lembind"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createWinnerzModule"
    "-sWASM=1"
    "-sSTACK_SIZE=5242880"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMAXIMUM_MEMORY=4294967296"
    "-sINITIAL_MEMORY=16777216"
    "--no-entry"
    "-sEXPORTED_RUNTIME_METHODS=['FS','HEAP8','HEAPU8']"
    "-sFORCE_FILESYSTEM=0"
    "-fexceptions"
    "-sDISABLE_EXCEPTION_CATCHING=0"
    "-sUSE_FREETYPE=1"
    "-sUSE_ZLIB=1"
    "-sUSE_LIBPNG=1"
    "-sSINGLE_FILE=0"
    "-sEXPORT_ES6=1"
    "-sDISABLE_EXCEPTION_CATCHING=0"
    "-s USE_ZLIB=1"
)

target_compile_options(winnerz_wasm PRIVATE "-fexceptions" "-sDISABLE_EXCEPTION_CATCHING=0")

# Copy outputs
add_custom_command(TARGET winnerz_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../winnerz/js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:winnerz_wasm>/winnerz_wasm.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/../winnerz/js/winnerz_wasm.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:winnerz_wasm>/winnerz_wasm.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../winnerz/js/winnerz_wasm.wasm"
    VERBATIM
)
