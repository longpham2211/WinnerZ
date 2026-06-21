#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace winnerz {

struct PreviewImage {
	int width = 0;
	int height = 0;
	int stride = 0;
	int channels = 4;
	std::vector<uint8_t> rgba;
};

// Returns true on success and fills out_image with RGBA pixels.
bool RenderPdfPagePreview(const std::string& pdf_path,
                          const std::vector<uint8_t>& mem_data,
                          int page_index,
                          float scale,
                          const std::array<float, 4>* clip,
                          PreviewImage& out_image,
                          std::string* error_message = nullptr);

bool IsPdfiumPreviewEnabled();

}  // namespace winnerz
