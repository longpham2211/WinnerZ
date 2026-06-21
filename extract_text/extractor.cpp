#include "extractor_text.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

// File trung gian kết nối PDFium với WinExtract Pipeline
WinPage extract_structured_text(std::string pdf_path) {
    WinExtract::WinTextExtractor extractor;

    // TODO: Tích hợp PDFium của bạn để lặp và add_char ở đây
    
    return extractor.finish_page();
}

// Hàm bóc tách danh sách phẳng (Flat List) cho tương thích ngược
std::vector<extract_text> extract_all_chars(std::string pdf_path) {
    WinPage page = extract_structured_text(pdf_path);
    std::vector<extract_text> flat_list;
    
    for (auto& block : page.blocks) {
        if (block.type != WinExtract::BlockType::TEXT) continue;
        for (auto& line : block.lines) {
            for (auto& span : line.spans) {
                for (auto& ch : span.chars) {
                    extract_text et;
                    et.x1 = ch.quad.ll.x;
                    et.y1 = ch.quad.ll.y;
                    et.x2 = ch.quad.ur.x;
                    et.y2 = ch.quad.ur.y;
                    et.ch = (wchar_t)ch.c;
                    et.fontName = span.font_name; // Lấy từ Span
                    et.fontSize = span.font_size; // Lấy từ Span
                    et.color = span.color;       // Lấy từ Span
                    et.object_id = 0;
                    flat_list.push_back(et);
                }
            }
        }
    }
    return flat_list;
}
