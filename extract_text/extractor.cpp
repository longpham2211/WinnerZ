#include "extractor_text.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

WinPage extract_structured_text(std::string pdf_path) {
    WinExtract::WinTextExtractor extractor;

    
    return extractor.finish_page();
}

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
                    et.fontName = span.font_name; 
                    et.fontSize = span.font_size; 
                    et.color = span.color;       
                    et.object_id = 0;
                    flat_list.push_back(et);
                }
            }
        }
    }
    return flat_list;
}
