#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
// fpdfview.h is only needed when building the Python wrapper with PDFium support.
// In WASM builds (WINNERZ_WASM_BUILD=1) or when PDFium is disabled, skip it.
#if !defined(WINNERZ_WASM_BUILD) && defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
#include <fpdfview.h>
#endif
#include <functional>
#include <memory>
#include "../extract_text/pdf_engine.hpp"

namespace WinExtract {
    class WinPdfDocument;
}

namespace Winnerz {

struct WinInsertTextTask {
    std::string text;
    float x0;
    float y0;
    float x1;
    float y1;
    float font_size;
    int r;
    int g;
    int b;
    bool bold;
    bool italic;
    bool multiline;
    std::string font_family;
};
struct WinInsertRectTask {
    float x0;
    float y0;
    float x1;
    float y1;
    int r;
    int g;
    int b;
};

void PreloadFonts(const std::string& fonts_dir);
float MeasureTextWidth(const std::string& text, const std::string& font_path, float font_size, bool is_bold = false, bool is_italic = false);
std::vector<uint8_t> InsertTextToMultiplePages(WinExtract::WinPdfDocument* doc, const std::map<int, std::vector<WinInsertTextTask>>& pages_tasks, const std::string& fonts_dir, std::function<void(int, int)> progress_cb = nullptr, int num_threads_opt = 0);
std::vector<uint8_t> InsertTextToMultiplePagesFitSpacing(WinExtract::WinPdfDocument* doc, const std::map<int, std::vector<WinInsertTextTask>>& pages_tasks, const std::string& fonts_dir, std::function<void(int, int)> progress_cb = nullptr, int num_threads_opt = 0);
std::vector<uint8_t> InsertRectsToMultiplePages(WinExtract::WinPdfDocument* doc, const std::map<int, std::vector<WinInsertRectTask>>& pages_tasks, std::function<void(int, int)> progress_cb = nullptr, int num_threads_opt = 0);

} // namespace Winnerz
