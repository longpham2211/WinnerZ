#include "cmap_parse.hpp"

#include <cctype>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "cmap_table.hpp"
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

static bool parse_hex_token_to_bytes(const std::string &token, std::vector<uint8_t> &out) {
    out.clear();
    if (token.size() < 2 || token.size() > 4096) {
        return false;
    }

    if (token.front() == '<' && token.back() == '>') {
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

    if (token.front() == '(' && token.back() == ')') {
        for (size_t i = 1; i + 1 < token.size(); ++i) {
            if (token[i] == '\\' && i + 2 < token.size()) {
                char next_c = token[i + 1];
                if (next_c == 'n') { out.push_back('\n'); i++; }
                else if (next_c == 'r') { out.push_back('\r'); i++; }
                else if (next_c == 't') { out.push_back('\t'); i++; }
                else if (next_c == 'b') { out.push_back('\b'); i++; }
                else if (next_c == 'f') { out.push_back('\f'); i++; }
                else if (next_c == '(' || next_c == ')' || next_c == '\\') {
                    out.push_back(static_cast<uint8_t>(next_c));
                    i++;
                }
                else if (next_c >= '0' && next_c <= '7') {
                    int octal_val = 0;
                    int j = 0;
                    while (j < 3 && i + 1 + j < token.size() && token[i + 1 + j] >= '0' && token[i + 1 + j] <= '7') {
                        octal_val = octal_val * 8 + (token[i + 1 + j] - '0');
                        j++;
                    }
                    out.push_back(static_cast<uint8_t>(octal_val));
                    i += j;
                } else {
                    out.push_back(static_cast<uint8_t>(next_c));
                    i++;
                }
            } else {
                out.push_back(static_cast<uint8_t>(token[i]));
            }
        }
        return !out.empty();
    }

    return false;
}

static bool parse_hex_token_to_int(const std::string &token, int &out) {
    if (token.size() < 2 || token.front() != '<' || token.back() != '>') {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!parse_hex_token_to_bytes(token, bytes)) {
        return false;
    }

    uint64_t v = 0;
    for (uint8_t b : bytes) {
        v = (v << 8) | static_cast<uint64_t>(b);
        if (v > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
    }
    out = static_cast<int>(v);
    return true;
}

static bool parse_hex_token_to_unicode_sequence(const std::string &token, std::vector<int> &out) {
    out.clear();
    std::vector<uint8_t> bytes;
    if (!parse_hex_token_to_bytes(token, bytes)) {
        return false;
    }

    if (bytes.empty()) {
        return false;
    }

    if (bytes.size() == 1) {
        out.push_back(bytes[0]);
        return true;
    }

    if ((bytes.size() % 2) != 0) {
        bytes.insert(bytes.begin(), 0);
    }

    for (size_t i = 0; i + 1 < bytes.size();) {
        const uint16_t u = static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
        i += 2;

        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= bytes.size()) {
                return false;
            }
            const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
            if (v < 0xDC00 || v > 0xDFFF) {
                return false;
            }
            cp = 0x10000u + (((static_cast<uint32_t>(u) - 0xD800u) << 10) |
                             (static_cast<uint32_t>(v) - 0xDC00u));
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;
        }

        if (cp > 0x10FFFFu) {
            return false;
        }
        if (cp == 0 && bytes.size() == 2) {
            // Allow U+0000 as base for identity ranges like <0000><FFFF><0000>
            out.push_back(0);
            return true;
        }
        if (cp == 0) {
            return false;
        }

        out.push_back(static_cast<int>(cp));
    }

    return !out.empty();
}

static bool is_cmap_hex_token(const std::string &tok) {
    return tok.size() >= 2 && tok.front() == '<' && tok.back() == '>' &&
           tok != "<<" && tok != ">>";
}

static bool is_cmap_string_token(const std::string &tok) {
    if (tok.size() < 2) return false;
    if (tok.front() == '<' && tok.back() == '>' && tok != "<<" && tok != ">>") return true;
    if (tok.front() == '(' && tok.back() == ')') return true;
    return false;
}

static bool parse_int_token_strict(const std::string &tok, int &out) {
    if (tok.empty()) {
        return false;
    }

    char *end_ptr = nullptr;
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

static std::vector<std::string> tokenize_cmap_stream(const std::string &text) {
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
        if (c == '(') {
            size_t end = pos + 1;
            int paren_depth = 1;
            while (end < text.size() && paren_depth > 0) {
                if (text[end] == '(') paren_depth++;
                else if (text[end] == ')') paren_depth--;
                else if (text[end] == '\\') {
                    if (end + 1 < text.size()) end++;
                }
                end++;
                if (end - pos > 4096) break;
            }
            toks.push_back(text.substr(pos, end - pos));
            pos = end;
            continue;
        }

        size_t start = pos;
        while (pos < text.size()) {
            unsigned char ch = static_cast<unsigned char>(text[pos]);
            if (std::isspace(ch) || ch == '%' || ch == '[' || ch == ']' ||
                ch == '<' || ch == '>' || ch == '(' || ch == ')') {
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

std::unordered_map<int, std::vector<int>> parse_tounicode_cmap(const std::vector<uint8_t> &bytes) {
    std::unordered_map<int, std::vector<int>> mapping;

    // A valid ToUnicode CMap is ASCII text (PostScript).
    // If the data looks like binary (encrypted or still-compressed),
    // bail out immediately rather than hanging in the tokenizer.
    if (!bytes.empty()) {
        const size_t check_len = std::min(bytes.size(), size_t(64));
        int non_text = 0;
        for (size_t i = 0; i < check_len; ++i) {
            uint8_t b = bytes[i];
            if (b > 127 || (b < 9 && b != 0)) {
                ++non_text;
            }
        }
        if (non_text > 16) {
            // Looks like binary/encrypted data — not valid CMap text
            return mapping;
        }
    }

    const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::vector<std::string> toks = tokenize_cmap_stream(text);


    for (size_t i = 1; i < toks.size(); ++i) {
        if (toks[i] != "usecmap") {
            continue;
        }

        std::string name = toks[i - 1];
        if (!name.empty() && name[0] == '/') {
            name.erase(name.begin());
        }
        if (name.empty()) {
            continue;
        }

        const auto &base = load_system_unicode_cmap_by_name(name);
        if (!base.empty()) {
            mapping = base;
        }
    }

    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string &cmd = toks[i];

        if (cmd == "begincodespacerange") {
            ++i;
            while (i < toks.size() && toks[i] != "endcodespacerange") {
                ++i;
            }
            continue;
        }

        if (cmd == "beginbfchar" || cmd == "begincidchar") {
            const bool is_cid = (cmd == "begincidchar");
            const std::string end_cmd = is_cid ? "endcidchar" : "endbfchar";

            int count = 0;
            if (i > 0) {
                parse_int_token_strict(toks[i - 1], count);
            }
            const int failsafe_limit = (count > 0 && count < 65536) ? count * 4 : 65536;
            int loop_counter = 0;

            ++i;
            while (i < toks.size() && toks[i] != end_cmd && loop_counter < failsafe_limit) {
                while (i < toks.size() && !is_cmap_hex_token(toks[i]) && toks[i] != end_cmd) {
                    ++i;
                }
                if (i + 1 >= toks.size() || toks[i] == end_cmd) {
                    break;
                }

                int src = 0;
                if (!parse_hex_token_to_int(toks[i], src)) {
                    while (i < toks.size() && toks[i] != end_cmd) {
                        ++i;
                    }
                    break;
                }

                if (is_cid) {
                    int dst = 0;
                    if (parse_int_token_strict(toks[i + 1], dst) && src >= 0) {
                        mapping[src] = {dst};
                    }
                } else {
                    std::vector<int> dst;
                    if (is_cmap_string_token(toks[i + 1]) &&
                        parse_hex_token_to_unicode_sequence(toks[i + 1], dst) &&
                        src >= 0) {
                        mapping[src] = std::move(dst);
                    }
                }

                i += 2;
                ++loop_counter;
            }
            if (i > 0) {
                --i;
            }
            continue;
        }

        if (cmd == "beginbfrange" || cmd == "begincidrange") {
            const bool is_cid = (cmd == "begincidrange");
            const std::string end_cmd = is_cid ? "endcidrange" : "endbfrange";

            int count = 0;
            if (i > 0) {
                parse_int_token_strict(toks[i - 1], count);
            }
            const int failsafe_limit = (count > 0 && count < 65536) ? count * 4 : 65536;
            int loop_counter = 0;

            ++i;
            while (i < toks.size() && toks[i] != end_cmd && loop_counter < failsafe_limit) {
                while (i < toks.size() && !is_cmap_hex_token(toks[i]) && toks[i] != end_cmd) {
                    ++i;
                }
                if (i + 1 >= toks.size() || toks[i] == end_cmd) {
                    break;
                }

                int src_start = 0;
                int src_end = 0;
                if (!parse_hex_token_to_int(toks[i], src_start) ||
                    !parse_hex_token_to_int(toks[i + 1], src_end) ||
                    src_start < 0 || src_start > 65535 || src_end < 0 || src_end > 65535 ||
                    src_start > src_end) {
                    while (i < toks.size() && toks[i] != end_cmd) {
                        ++i;
                    }
                    break;
                }
                i += 2;

                if (is_cid) {
                    int dst = 0;
                    if (i < toks.size() && parse_int_token_strict(toks[i], dst)) {
                        for (int code = src_start; code <= src_end; ++code) {
                            mapping[code] = {dst + (code - src_start)};
                        }
                        ++i;
                    } else {
                        while (i < toks.size() && toks[i] != end_cmd) {
                            ++i;
                        }
                    }
                    ++loop_counter;
                    continue;
                }

                if (i < toks.size() && toks[i] == "[") {
                    ++i;
                    int code = src_start;
                    while (i < toks.size() && toks[i] != "]" && toks[i] != end_cmd) {
                        if (is_cmap_string_token(toks[i])) {
                            std::vector<int> dst;
                            if (code <= src_end && parse_hex_token_to_unicode_sequence(toks[i], dst)) {
                                mapping[code] = std::move(dst);
                            }
                            ++code;
                        }
                        ++i;
                    }
                    if (i < toks.size() && toks[i] == "]") {
                        ++i;
                    }
                } else if (i < toks.size() && is_cmap_string_token(toks[i])) {
                    std::vector<int> dst_start;
                    if (parse_hex_token_to_unicode_sequence(toks[i], dst_start) && !dst_start.empty()) {
                        if (dst_start.size() == 1) {
                            for (int code = src_start; code <= src_end; ++code) {
                                const int cp = dst_start.front() + (code - src_start);
                                if (cp >= 0 && cp <= 0x10FFFF) {
                                    mapping[code] = {cp};
                                }
                            }
                        } else {
                            std::vector<int> dst = dst_start;
                            for (int code = src_start; code <= src_end; ++code) {
                                mapping[code] = dst;
                                if (!dst.empty()) {
                                    dst.back()++;
                                }
                            }
                        }
                    }
                    ++i;
                } else {
                    while (i < toks.size() && toks[i] != end_cmd) {
                        ++i;
                    }
                }

                ++loop_counter;
            }

            if (i > 0) {
                --i;
            }
        }
    }

    return mapping;
}

} // namespace WinExtract