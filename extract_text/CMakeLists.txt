cmake_minimum_required(VERSION 3.10)
project(ExtractTextProject)

# 100% STANDALONE - ZERO DEPENDENCIES - WINNERZ ENGINE
set(CMAKE_CXX_STANDARD 17)

if (MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_executable(winnerz_extract 
    main.cpp
    extractor_logic.cpp
    pdf_engine.cpp
    parse.cpp
    xref.cpp
    cmap_parse.cpp
    cmap.cpp
    unicode.cpp
    type_3.cpp
    decoder_inflate.cpp
    bidi.cpp
    bidi_std.cpp
    ucdn.cpp
    # zlib-license puff inflater for FlateDecode streams
    puff.c
)

# Optional FreeType integration for more accurate glyph metrics fallback.
set(WINEXTRACT_FREETYPE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/free-type/freetype-2.14.3")
if (EXISTS "${WINEXTRACT_FREETYPE_DIR}/CMakeLists.txt")
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

    add_subdirectory("${WINEXTRACT_FREETYPE_DIR}" EXCLUDE_FROM_ALL)
    target_link_libraries(winnerz_extract PRIVATE freetype)
    target_compile_definitions(winnerz_extract PRIVATE WINEXTRACT_USE_FREETYPE=1)
endif()