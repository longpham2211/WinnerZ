#pragma once

#include <cstdint>
#include <vector>

#include "pdf_engine.hpp"

namespace WinExtract {

std::vector<WinCodeSpaceRange> parse_cmap_codespace_ranges_fitz(const std::vector<uint8_t>& bytes);

} // namespace WinExtract
