#include "unicode.hpp"

#include <cctype>
#include <string>
#include <unordered_map>

#include "adobe_glyph_list.hpp"

namespace WinExtract {
namespace {

static int parse_hex_codepoint(const std::string& s) {
    if (s.empty() || s.size() > 6) {
        return 0;
    }

    int v = 0;
    for (char c : s) {
        int n = -1;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        if (n < 0) {
            return 0;
        }
        v = (v << 4) | n;
    }
    return v;
}

} // namespace

int glyph_name_to_unicode_fitz(std::string name) {
    if (name.empty()) {
        return 0;
    }

    size_t dot = name.find('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }

    if (name == "f_i" || name == "fi") return 0xFB01;
    if (name == "f_l" || name == "fl") return 0xFB02;
    if (name == "f_f_i" || name == "ffi") return 0xFB03;
    if (name == "f_f_l" || name == "ffl") return 0xFB04;

    size_t underscore = name.find('_');
    if (underscore != std::string::npos) {
        // [FIX]: Truncate at underscore like MuPDF encodings.c:121, 
        // but only if it's not a known ligature starting with 'f'.
        if (name[0] != 'f') {
            name = name.substr(0, underscore);
        }
    }

    const auto& builtin_glyphs = get_adobe_glyph_map();
    auto builtin_it = builtin_glyphs.find(name);
    if (builtin_it != builtin_glyphs.end()) {
        return builtin_it->second;
    }

    // [FIX]: Handle 'uni' prefix (e.g., uni2014)
    if (name.size() >= 7 && name.compare(0, 3, "uni") == 0) {
        std::string hex = name.substr(3, 4);
        int cp = parse_hex_codepoint(hex);
        if (cp > 0) return cp;
    }

    // [FIX]: Handle 'u' prefix (e.g., u2014 or u00A0)
    if (name.size() >= 5 && name[0] == 'u') {
        std::string hex = name.substr(1);
        bool ok = true;
        for (char c : hex) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                ok = false;
                break;
            }
        }
        if (ok) {
            int cp = parse_hex_codepoint(hex);
            if (cp > 0) return cp;
        }
    }

    // [FIX]: Handle 'a' prefix followed by digits (e.g., a45)
    if (name.size() >= 2 && name[0] == 'a') {
        bool is_num = true;
        for (size_t i = 1; i < name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                is_num = false;
                break;
            }
        }
        if (is_num) {
            try {
                return std::stoi(name.substr(1));
            } catch (...) {}
        }
    }

    // [FIX]: Handle 'g' prefix or pure digits (e.g., g45 or 45)
    size_t start_idx = 0;
    if (name.size() >= 2 && name[0] == 'g' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        start_idx = 1;
    }

    bool is_num = true;
    for (size_t i = start_idx; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            is_num = false;
            break;
        }
    }
    if (is_num && name.size() > start_idx) {
        try {
            return std::stoi(name.substr(start_idx));
        } catch (...) {}
    }

    // [FIX]: Return 0 instead of 0xFFFD so caller can distinguish failure.
    return 0;
}

} // namespace WinExtract
