#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <fpdfview.h>
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
void PreloadFonts(const std::string& fonts_dir);
std::vector<uint8_t> InsertTextToMultiplePages(WinExtract::WinPdfDocument* doc, const std::map<int, std::vector<WinInsertTextTask>>& pages_tasks, const std::string& fonts_dir, std::function<void(int, int)> progress_cb = nullptr, int num_threads_opt = 0);

} // namespace Winnerz
