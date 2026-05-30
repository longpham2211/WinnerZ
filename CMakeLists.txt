cmake_minimum_required(VERSION 3.10)
project(WinnerZ)

set(CMAKE_CXX_STANDARD 17)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

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

# 2. Định nghĩa thư viện chính (self-contained, no PDFium)
add_library(WinnerZ STATIC
    src/redactor.cpp
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
    extract_text/puff.c
)
set_target_properties(WinnerZ PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(WinnerZ PUBLIC cxx_std_17)
target_include_directories(WinnerZ PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/extract_text"
)

find_library(
    WINNERZ_LCMS2_LIB
    NAMES lcms2 lcms2.so.2 liblcms2.so.2
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
)
if(WINNERZ_LCMS2_LIB)
    target_link_libraries(WinnerZ PUBLIC "${WINNERZ_LCMS2_LIB}")
    target_compile_definitions(WinnerZ PRIVATE WINNERZ_USE_LCMS2=1)
else()
    target_compile_definitions(WinnerZ PRIVATE WINNERZ_USE_LCMS2=0)
endif()

set(WINNERZ_PDFIUM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/pdfium/include")
set(WINNERZ_PDFIUM_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third-party/pdfium/lib/libpdfium.so")

if(UNIX AND NOT APPLE AND EXISTS "${WINNERZ_PDFIUM_INCLUDE_DIR}/fpdfview.h" AND EXISTS "${WINNERZ_PDFIUM_LIB}")
    target_sources(WinnerZ PRIVATE src/drawing.cpp)
    target_include_directories(WinnerZ PUBLIC "${WINNERZ_PDFIUM_INCLUDE_DIR}")
    target_link_libraries(WinnerZ PUBLIC "${WINNERZ_PDFIUM_LIB}")
    target_compile_definitions(WinnerZ PUBLIC WINNERZ_USE_PDFIUM_PREVIEW=1)
else()
    target_compile_definitions(WinnerZ PUBLIC WINNERZ_USE_PDFIUM_PREVIEW=0)
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
    target_link_libraries(WinnerZ PUBLIC nlohmann_json::nlohmann_json freetype)
    target_compile_definitions(WinnerZ PRIVATE WINEXTRACT_USE_FREETYPE=1)
else()
    target_link_libraries(WinnerZ PUBLIC nlohmann_json::nlohmann_json)
endif()

# 3. Tạo Python wrapper

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

    pybind11_add_module(winnerz_core src/winnerz_python_wrapper.cpp)
    target_compile_features(winnerz_core PRIVATE cxx_std_17)
    target_include_directories(winnerz_core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(winnerz_core PRIVATE WinnerZ)

    if(UNIX AND NOT APPLE)
        set_target_properties(winnerz_core PROPERTIES
            PREFIX ""
            OUTPUT_NAME "winnerz_core"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/winnerz"
            BUILD_RPATH "\$ORIGIN;\$ORIGIN/../third-party/pdfium/lib"
            INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../third-party/pdfium/lib"
            BUILD_RPATH_USE_ORIGIN ON
        )

        file(GLOB WINNERZ_STALE_WINDOWS_ARTIFACTS
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/winnerz_core*.pyd"
            "${CMAKE_CURRENT_SOURCE_DIR}/winnerz/winnerz_core*.pdb"
        )

        if(WINNERZ_STALE_WINDOWS_ARTIFACTS)
            add_custom_command(TARGET winnerz_core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rm -f ${WINNERZ_STALE_WINDOWS_ARTIFACTS}
                VERBATIM
            )
        endif()
    endif()

    install(TARGETS winnerz_core DESTINATION winnerz)
    install(FILES winnerz/__init__.py DESTINATION winnerz)
    install(FILES winnerz/winnerz_core.pyi DESTINATION winnerz)
endif()
