#include <unordered_map>
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace WinExtract {

std::unordered_map<int, std::vector<int>> parse_tounicode_cmap_fitz(const std::vector<uint8_t>& bytes);

} // namespace WinExtract
