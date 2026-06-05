#include "cmap.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

#include "parse.hpp"

namespace WinExtract {
namespace {

static uint8_t hex_to_byte_local(char h1, char h2) {
    auto to_nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return 0;
    };
    return static_cast<uint8_t>((to_nibble(h1) << 4) | to_nibble(h2));
}

static bool parse_hex_token_to_bytes(const std::string& token, std::vector<uint8_t>& out) {
    out.clear();
    if (token.size() < 2 || token.front() != '<' || token.back() != '>') {
        return false;
    }
    if (token == "<<" || token == ">>") {
        return false;
    }

    std::string hex;
    hex.reserve(token.size());
    for (size_t i = 1; i + 1 < token.size(); ++i) {
        char c = token[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        hex.push_back(c);
    }

    if (hex.empty()) {
        return false;
    }

    if ((hex.size() % 2) != 0) {
        hex.push_back('0');
    }

    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(hex_to_byte_local(hex[i], hex[i + 1]));
    }
    return !out.empty();
}

static bool is_cmap_hex_token(const std::string& tok) {
    return tok.size() >= 2 && tok.front() == '<' && tok.back() == '>' && tok != "<<" && tok != ">>";
}

static bool parse_int_token_strict(const std::string& tok, int& out) {
    if (tok.empty()) {
        return false;
    }

    char* end_ptr = nullptr;
    long v = wz_strtol(tok.c_str(), &end_ptr, 10);
    if (end_ptr == tok.c_str() || *end_ptr != '\0') {
        return false;
    }
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
        return false;
    }

    out = static_cast<int>(v);
    return true;
}

static std::vector<std::string> tokenize_cmap_stream(const std::string& text) {
    std::vector<std::string> toks;
    size_t pos = 0;
    while (pos < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        if (std::isspace(c)) {
            ++pos;
            continue;
        }
        if (c == '%') {
            while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') {
                ++pos;
            }
            continue;
        }
        if (c == '[' || c == ']') {
            toks.push_back(text.substr(pos, 1));
            ++pos;
            continue;
        }
        if (c == '<') {
            if (pos + 1 < text.size() && text[pos + 1] == '<') {
                toks.push_back("<<");
                pos += 2;
                continue;
            }
            size_t end = text.find('>', pos + 1);
            if (end == std::string::npos) {
                break;
            }
            toks.push_back(text.substr(pos, end - pos + 1));
            pos = end + 1;
            continue;
        }
        if (c == '>') {
            if (pos + 1 < text.size() && text[pos + 1] == '>') {
                toks.push_back(">>");
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }

        size_t start = pos;
        while (pos < text.size()) {
            unsigned char ch = static_cast<unsigned char>(text[pos]);
            if (std::isspace(ch) || ch == '%' || ch == '[' || ch == ']' || ch == '<' || ch == '>') {
                break;
            }
            ++pos;
        }
        if (pos > start) {
            toks.push_back(text.substr(start, pos - start));
        }
    }

    return toks;
}

} // namespace

std::vector<WinCodeSpaceRange> parse_cmap_codespace_ranges(const std::vector<uint8_t>& bytes) {
    std::vector<WinCodeSpaceRange> ranges;
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::vector<std::string> toks = tokenize_cmap_stream(text);

    auto bytes_to_u32 = [](const std::vector<uint8_t>& b, uint32_t& out) -> bool {
        if (b.empty() || b.size() > 4) {
            return false;
        }
        uint32_t v = 0;
        for (uint8_t x : b) {
            v = (v << 8) | static_cast<uint32_t>(x);
        }
        out = v;
        return true;
    };

    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] != "begincodespacerange") {
            continue;
        }

        int count = 0;
        if (i == 0 || !parse_int_token_strict(toks[i - 1], count) || count <= 0) {
            continue;
        }

        ++i;
        int remaining = count;
        while (i < toks.size() && remaining > 0) {
            while (i < toks.size() && !is_cmap_hex_token(toks[i])) {
                ++i;
            }
            if (i + 1 >= toks.size()) {
                break;
            }

            std::vector<uint8_t> low_bytes;
            std::vector<uint8_t> high_bytes;
            if (parse_hex_token_to_bytes(toks[i], low_bytes) && parse_hex_token_to_bytes(toks[i + 1], high_bytes) &&
                !low_bytes.empty() && low_bytes.size() == high_bytes.size()) {
                uint32_t low = 0;
                uint32_t high = 0;
                if (bytes_to_u32(low_bytes, low) && bytes_to_u32(high_bytes, high)) {
                    WinCodeSpaceRange r;
                    r.nbytes = static_cast<int>(low_bytes.size());
                    r.low = low;
                    r.high = high;
                    ranges.push_back(r);
                }
            }

            i += 2;
            --remaining;
        }

        if (i > 0) {
            --i;
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const WinCodeSpaceRange& a, const WinCodeSpaceRange& b) {
        if (a.nbytes != b.nbytes) {
            return a.nbytes > b.nbytes;
        }
        if (a.low != b.low) {
            return a.low < b.low;
        }
        return a.high < b.high;
    });

    return ranges;
}

} // namespace WinExtract
