#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include "extractor_logic.hpp"
#include "pdf_engine.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

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
    std::cout << "Pipeline: Document -> Page -> Interpreter -> Device (SText)" << std::endl;
    std::cout << "Input PDF: " << path << std::endl;
    std::cout << "Detected pages/streams: " << page_count << std::endl;

    if (page_count <= 0) {
        std::cerr << "WARN: No page/content stream detected. PDF may use compressed object streams not fully supported yet." << std::endl;
        return;
    }

    for (int i = 0; i < page_count; i++) {
        // 2. PIPELINE Bước 2: Bóc Page Content Stream
        std::cout << "\n--- TRANG " << i + 1 << " ---" << std::endl;
        std::vector<uint8_t> stream = doc->get_page_content(i);

        WinFontUnicodeMap font_unicode_map = doc->get_page_font_unicode_map(i);
        WinFontWidthMap   font_width_map   = doc->get_page_font_width_map(i);
        WinFontCodeBytesMap font_code_bytes_map = doc->get_page_font_code_bytes_map(i);
        WinFontCodeSpaceMap font_codespace_map = doc->get_page_font_codespace_map(i);
        WinFontMatrixMap font_matrix_map = doc->get_page_font_matrix_map(i);
        WinFontVerticalMetricsMap font_vertical_metrics_map = doc->get_page_font_vertical_metrics_map(i);
        WinFontW2Map font_w2_map = doc->get_page_font_w2_map(i);
        WinColorSpaceMap color_space_map = doc->get_page_color_space_map(i);
        std::shared_ptr<const WinFormXObjectMap> form_xobject_map = doc->get_page_form_xobject_map(i);
        Rect              mediabox         = doc->get_page_geometry(i).mediabox;

        WinTextExtractor dev;
        dev.begin_page(mediabox.x1 - mediabox.x0, mediabox.y1 - mediabox.y0);

        WinPdfInterpreter::run(stream, dev, font_unicode_map, font_width_map, font_code_bytes_map, font_codespace_map, font_matrix_map, font_vertical_metrics_map, font_w2_map, color_space_map, form_xobject_map, nullptr, 0, &mediabox, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        WinPage structured_page = dev.finish_page();
        
        std::string page_text = dev.get_text(structured_page);
        std::cout << "Page " << i + 1 << " text:\n" << page_text << "\n";
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
