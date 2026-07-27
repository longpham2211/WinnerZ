cmake_minimum_required(VERSION 3.18)
project(WinnerZ_WASM C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Nlohmann JSON (Header Only) ─────────────────────────────────────────────
# Tải nlohmann_json bằng Git để tránh lỗi bung file .tar.xz
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

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
)

target_include_directories(WinnerZ_WASM_Lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../redactor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../extract_text"
)

# KHÔNG build kèm PDFium vì WASM không hỗ trợ binaries của PDFium
target_compile_definitions(WinnerZ_WASM_Lib PUBLIC
    WINNERZ_USE_PDFIUM_PREVIEW=0
    WINNERZ_WASM_BUILD=1
    WINEXTRACT_USE_FREETYPE=1
)

# Sử dụng thư viện chuẩn của Emscripten cho WASM (không cần download)
target_compile_options(WinnerZ_WASM_Lib PRIVATE
    "-fexceptions"
    "-sUSE_FREETYPE=1"
    "-sUSE_HARFBUZZ=1"
    "-sUSE_ZLIB=1"
    "-sUSE_LIBPNG=1"
)

target_link_libraries(WinnerZ_WASM_Lib PUBLIC nlohmann_json::nlohmann_json)

# ─── WASM Executable ─────────────────────────────────────────────────────────
add_executable(winnerz_wasm winnerz_wasm_embind.cpp)

target_include_directories(winnerz_wasm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(winnerz_wasm PRIVATE WinnerZ_WASM_Lib)

set_target_properties(winnerz_wasm PROPERTIES
    OUTPUT_NAME "winnerz_wasm"
    SUFFIX ".js"
)

target_link_options(winnerz_wasm PRIVATE
    "-lembind"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createWinnerzModule"
    "-sWASM=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMAXIMUM_MEMORY=536870912"
    "-sINITIAL_MEMORY=16777216"
    "--no-entry"
    "-sEXPORTED_RUNTIME_METHODS=['FS','HEAP8','HEAPU8']"
    "-sFORCE_FILESYSTEM=0"
    "-fexceptions"
    "-sDISABLE_EXCEPTION_CATCHING=0"
    "-sUSE_FREETYPE=1"
    "-sUSE_HARFBUZZ=1"
    "-sUSE_ZLIB=1"
    "-sUSE_LIBPNG=1"
    "-sSINGLE_FILE=0"
    "-sEXPORT_ES6=1"
)

target_compile_options(winnerz_wasm PRIVATE "-fexceptions")

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
