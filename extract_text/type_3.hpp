#include <unordered_map>
#pragma once

#include <map>
#include <string>
#include <vector>

namespace WinExtract {

bool is_type3_font_subtype_fitz(const std::string& subtype);
float scale_type3_width_fitz(float raw_width, float font_matrix_a);
void apply_type3_ascii_fallback_fitz(std::unordered_map<int, std::vector<int>>& cmap);

} // namespace WinExtract
