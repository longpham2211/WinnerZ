#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include "extractor_logic.hpp"
#include "pdf_engine.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

// WINNERZ
using namespace WinExtract;

void process_winnerz_engine(const std::string& path) {
    // 1. PIPELINE Bước 1: Mở Document (Pure C++ / xref / objects)
    auto doc = WinPdfDocument::open(path);
    if (!doc) {
        std::cerr << "ERROR: Cannot open PDF file or xref not found: " << path << std::endl;
        return;
    }

    int page_count = doc->count_pages();
    std::cout << "--- WINNERZ PROJECT: STANDALONE ENGINE CLONE ---" << std::endl;
    std::cout << "Fitz Pipeline: Document -> Page -> Interpreter -> Device (SText)" << std::endl;
    std::cout << "Input PDF: " << path << std::endl;
    std::cout << "Detected pages/streams: " << page_count << std::endl;

    if (page_count <= 0) {
        std::cerr << "WARN: No page/content stream detected. PDF may use compressed object streams not fully supported yet." << std::endl;
        return;
    }

    for (int i = 0; i < page_count; i++) {
        // 2. PIPELINE Bước 2: Bóc Page Content Stream
        std::vector<uint8_t> stream = doc->get_page_content(i);
        WinFontUnicodeMap font_unicode_map = doc->get_page_font_unicode_map(i);
        WinFontWidthMap   font_width_map   = doc->get_page_font_width_map(i);
        WinFontCodeBytesMap font_code_bytes_map = doc->get_page_font_code_bytes_map(i);
        WinFontCodeSpaceMap font_codespace_map = doc->get_page_font_codespace_map(i);
        WinFontMatrixMap font_matrix_map = doc->get_page_font_matrix_map(i);
        WinFontVerticalMetricsMap font_vertical_metrics_map = doc->get_page_font_vertical_metrics_map(i);
        WinColorSpaceMap color_space_map = doc->get_page_color_space_map(i);
        WinFormXObjectMap form_xobject_map = doc->get_page_form_xobject_map(i);
        Rect              mediabox         = doc->get_page_geometry(i).mediabox;

        WinTextExtractor dev;
        dev.begin_page(mediabox.x1 - mediabox.x0, mediabox.y1 - mediabox.y0);

        if (std::getenv("WINEXTRACT_DEBUG_STREAM") != nullptr && i == 2) {
            std::string s(reinterpret_cast<const char*>(stream.data()), stream.size());
            size_t p = s.find("87,800");
            if (p != std::string::npos) {
                size_t from = (p > 120) ? (p - 120) : 0;
                size_t to = (p + 120 < s.size()) ? (p + 120) : s.size();
                std::cout << "\n[DEBUG_STREAM_CONTEXT]\n";
                for (size_t k = from; k < to; ++k) {
                    unsigned char c = static_cast<unsigned char>(s[k]);
                    if (c == '\r' || c == '\n') {
                        std::cout << '\\' << ((c == '\r') ? 'r' : 'n');
                    } else if (c >= 32 && c <= 126) {
                        std::cout << static_cast<char>(c);
                    } else {
                        std::cout << "\\x";
                        const char* hex = "0123456789ABCDEF";
                        std::cout << hex[(c >> 4) & 0x0F] << hex[c & 0x0F];
                    }
                }
                std::cout << "\n[END_DEBUG_STREAM_CONTEXT]\n";
            }
        }

        WinPdfInterpreter::run(stream, dev, font_unicode_map, font_width_map, font_code_bytes_map, font_codespace_map, font_matrix_map, font_vertical_metrics_map, color_space_map, form_xobject_map, nullptr, 0, &mediabox, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

        // KẾT QUẢ ĐÃ ĐƯỢC NHÓM LẠI THEO HỆ THỐNG PHÂN CẤP (Fitz SText)
        WinPage structured_page = dev.finish_page();
        std::cout << "\n--- TRANG " << i + 1 << " (Pipeline Finished) ---" << std::endl;
        WinTextExtractor::print_to_terminal(structured_page);
    }
}

#include <fstream> // Cần để check file tồn tại

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string path = "data/ieee-format.pdf";
    
    if (argc > 1) {
        path = argv[1];
    } else {
        // WinnerZ Smart Path: Tự tìm file nếu đang chạy trong build/Debug
        std::ifstream f(path.c_str());
        if (!f.good()) {
            // Thử lùi lại 2 cấp (để thoát khỏi build/Debug)
            path = "../../data/ieee-format.pdf";
        }
    }
    
    process_winnerz_engine(path);
    return 0;
}
