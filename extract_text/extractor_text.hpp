#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "extractor_logic.hpp"

// Forwarding for new Mu-Logic structures
using WinChar = WinExtract::WinChar;
using WinLine = WinExtract::WinLine;
using WinBlock = WinExtract::WinBlock;
using WinPage = WinExtract::WinPage;

struct extract_text {
    float x1, y1, x2, y2;
    wchar_t ch;
    uint32_t color;
    std::string fontName;
    float fontSize;
    int object_id;
};

WinPage extract_structured_text(std::string pdf_path);

std::vector<extract_text> extract_all_chars(std::string pdf_path);