#include "type_3.hpp"

#include <algorithm>
#include <cctype>

namespace {

constexpr int kReplacementCharacter = 0xFFFD;

}

namespace WinExtract {

bool is_type3_font_subtype_fitz(const std::string& subtype) {
    // Keep this check strict to mirror PDF name semantics while tolerating parser noise.
    std::string s = subtype;
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());

    if (!s.empty() && s.front() == '/') {
        s.erase(s.begin());
    }

    return s == "Type3" || s == "type3";
}

float scale_type3_width_fitz(float raw_width, float font_matrix_a) {
    // Fitz effective advance for Type3 widths: t3matrix.a * Width
    return raw_width * font_matrix_a;
}

void apply_type3_ascii_fallback_fitz(std::unordered_map<int, std::vector<int>>& cmap) {
    bool has_single_byte_map = true;
    for (int i = 0; i < 256; ++i) {
        if (cmap.find(i) == cmap.end()) {
            has_single_byte_map = false;
            break;
        }
    }
    if (!has_single_byte_map) {
        return;
    }

    for (int i = 32; i < 127; ++i) {
        auto it = cmap.find(i);
        if (it == cmap.end() || it->second.size() != 1) {
            continue;
        }
        if (it->second[0] == kReplacementCharacter) {
            it->second[0] = i;
        }
    }
}

} // namespace WinExtract
