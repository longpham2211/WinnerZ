// redactor.cpp
// Core redaction logic adapted for WinnerZ architecture
// Independent implementation without external dependencies
// Native WinnerZ pipeline: WinPdfDocument + WinPdfInterpreter.
//
// Logical mapping components:
// Text filtering logic
// Core redaction logic
// Core redaction logic
// Core redaction logic
// Core redaction logic

#include "redactor.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

// ─── WinnerZ pipeline headers ───────────────────────────────────────────────
#include "../extract_text/pdf_engine.hpp"
#include "../extract_text/extractor_logic.hpp"
#include "../extract_text/decoder_inflate.hpp"

namespace winnerz {

// ============================================================
// Internal structures
// ============================================================

using WinExtract::Rect;
using WinExtract::Vec2;
using WinExtract::Quad;

// Defines a redaction zone
struct WinRedactZone {
    Rect rect;   // PDF coordinates (bottom-left origin)
};

// ─── Geometry helpers ────────────────────────────────────────────────────────

static bool rects_are_valid_and_intersect(const Rect& a, const Rect& b) {
    // Check if bounding boxes intersect and form a valid rectangle
    // Intersection of 2 non-empty rects is a valid rect
    float ix0 = (std::max)(a.x0, b.x0);
    float iy0 = (std::max)(a.y0, b.y0);
    float ix1 = (std::min)(a.x1, b.x1);
    float iy1 = (std::min)(a.y1, b.y1);
    // Check if bounding boxes intersect and form a valid rectangle
    return ix0 <= ix1 && iy0 <= iy1;
}

static bool rect_contains(const Rect& outer, const Rect& inner) {
    // Core redaction logic
    return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 &&
           outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

static bool rect_is_empty(const Rect& r) {
    return r.x0 > r.x1 || r.y0 > r.y1;
}

static Rect rect_intersect(const Rect& a, const Rect& b) {
    return { (std::max)(a.x0, b.x0), (std::max)(a.y0, b.y0),
             (std::min)(a.x1, b.x1), (std::min)(a.y1, b.y1) };
}

// Core redaction logic
// Returns: 0 = no touch, 1 = partial overlap, 2 = full overlap
static int rect_touches_redactions(const Rect& area,
                                   const std::vector<WinRedactZone>& zones) {
    for (const auto& z : zones) {
        Rect s = rect_intersect(area, z.rect);
        if (!rect_is_empty(s)) {
            if (rect_contains(z.rect, area))
                return 2;
            return 1;
        }
    }
    return 0;
}

// ============================================================
// Text filtering logic
// ============================================================

// Core redaction logic adapted for WinnerZ architecture
//
// Shrink bounding box by 10% on all sides before intersection check
//   bbox.x0 += w/10;  bbox.x1 -= w/10;
//   bbox.y0 += h/10;  bbox.y1 -= h/10;
static bool should_remove_glyph(const Rect& raw_char_bbox,
                                 const std::vector<WinRedactZone>& zones) {
    Rect bbox = raw_char_bbox;

    // Shrink bounding box by 10% on all sides before intersection check
    float w = bbox.x1 - bbox.x0;
    float h = bbox.y1 - bbox.y0;
    bbox.x0 += w / 10.0f;
    bbox.x1 -= w / 10.0f;
    bbox.y0 += h / 10.0f;
    bbox.y1 -= h / 10.0f;

    for (const auto& z : zones) {
        // Check if bounding boxes intersect and form a valid rectangle
        if (rects_are_valid_and_intersect(bbox, z.rect))
            return true;
    }
    return false;
}

// ============================================================
// Token writer for PDF operator stream
// ============================================================

static void append_pdf_string(std::string& out, const std::vector<uint8_t>& data) {
    out += '<';
    char buf[3];
    for (uint8_t b : data) {
        snprintf(buf, sizeof(buf), "%02X", b);
        out += buf;
    }
    out += '>';
}

static void format_pdf_float(char* buf, size_t size, float v) {
    snprintf(buf, size, "%.9f", v);
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == '0') {
        buf[--len] = '\0';
    }
    if (len > 0 && buf[len - 1] == '.') {
        buf[--len] = '\0';
    }
    if (len == 0 || (len == 1 && buf[0] == '-')) {
        snprintf(buf, size, "0");
    }
}

static void append_float(std::string& out, float v) {
    char buf[32];
    format_pdf_float(buf, sizeof(buf), v);
    out += buf;
}

// ============================================================
//  WinRedactInterpreter
// Parse and filter the content stream to remove redacted text
// ============================================================

// Minimal text state (mirrors TextState in pdf_engine.cpp)
struct TextState {
    float tm[6]  = {1,0,0,1,0,0};  // Text Matrix
    float tlm[6] = {1,0,0,1,0,0};  // Text Line Matrix
    float font_size  = 12.0f;
    float h_scale    = 100.0f;
    float char_spacing = 0.0f;
    float word_spacing = 0.0f;
    float text_rise  = 0.0f;
    float leading    = 0.0f;
    int   wmode      = 0;
    std::string font_name;
};

// Matrix multiplication: m = a * b (PDF column-major 3x3)
static void mat_mul(const float a[6], const float b[6], float out[6]) {
    out[0] = a[0]*b[0] + a[1]*b[2];
    out[1] = a[0]*b[1] + a[1]*b[3];
    out[2] = a[2]*b[0] + a[3]*b[2];
    out[3] = a[2]*b[1] + a[3]*b[3];
    out[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    out[5] = a[4]*b[1] + a[5]*b[3] + b[5];
}

// =============================================================================
// Core redaction logic
//
// 1. Compute local TRM
// 2. Transform bounding box to device space
//    1. trm_local  = Tfs × Th × Tm  (text space, pre-CTM)
//    2. ctm        = pending.ctm ⊕ sent.ctm ⊕ page_transform
//    3. bbox       = fontspace bbox (uses ascender/descender from font metrics)
//    4. Call text_filter(trm_local, ctm, bbox_fontspace)
//    5. Inside text_filter: trm_device = concat(trm_local, ctm)
//                           bbox_device = transform_rect(bbox_fontspace, trm_device)
//    6. shrink bbox_device by 10% then intersect test
// =============================================================================

// Step 1: Compute local TRM (text space). Do not multiply CTM.
// 1. Compute local TRM
// 2. Transform bounding box to device space
static void compute_trm_local(const TextState& st, float trm_local[6]) {
    float h  = st.h_scale / 100.0f;
    float fs = st.font_size;
    float rise = st.text_rise;
    // Per PDF spec: trm = [ Tfs*Th*a  Tfs*Th*b  Tfs*c  Tfs*d  Trise*c+e  Trise*d+f ]
    trm_local[0] = fs * h * st.tm[0];
    trm_local[1] = fs * h * st.tm[1];
    trm_local[2] = fs * st.tm[2];
    trm_local[3] = fs * st.tm[3];
    trm_local[4] = rise * st.tm[2] + st.tm[4];
    trm_local[5] = rise * st.tm[3] + st.tm[5];
}

// Step 2: Compute font space bbox — using ascender/descender from font metrics.
// Core redaction logic
//   WMode 0: bbox = { x0=0, y0=descender, x1=adv, y1=ascender }
//   WMode 1: bbox = { x0=font_bbox_x0, y0=0, x1=font_bbox_x1, y1=adv_vert }
// Use [-0.5, 0.5] approximation for CJK fonts
static Rect compute_bbox_fontspace(float adv, float ascender, float descender,
                                    float font_bbox_x0, float font_bbox_x1, int wmode) {
    if (wmode == 0) {
        // Clone line 700–703: bbox = [0, descender, adv, ascender]
        return { 0.0f, descender, adv, ascender };
    } else {
        // Clone line 707–711: bbox = [font_bbox.x0, 0, font_bbox.x1, adv_vert]
        return { font_bbox_x0, 0.0f, font_bbox_x1, adv };
    }
}

// Step 3: Transform bbox from font space to device space.
// Core redaction logic
//   trm_device = concat(trm_local, ctm)
//   bbox_device = transform_rect(bbox_fontspace, trm_device)
static Rect transform_rect_by_matrix(const Rect& r, const float m[6]) {
    // Transform 4 corners of rect, get AABB
    float px[4], py[4];
    // corner (r.x0, r.y0)
    px[0] = r.x0*m[0] + r.y0*m[2] + m[4];  py[0] = r.x0*m[1] + r.y0*m[3] + m[5];
    // corner (r.x0, r.y1)
    px[1] = r.x0*m[0] + r.y1*m[2] + m[4];  py[1] = r.x0*m[1] + r.y1*m[3] + m[5];
    // corner (r.x1, r.y0)
    px[2] = r.x1*m[0] + r.y0*m[2] + m[4];  py[2] = r.x1*m[1] + r.y0*m[3] + m[5];
    // corner (r.x1, r.y1)
    px[3] = r.x1*m[0] + r.y1*m[2] + m[4];  py[3] = r.x1*m[1] + r.y1*m[3] + m[5];
    return {
        *std::min_element(px, px+4), *std::min_element(py, py+4),
        *std::max_element(px, px+4), *std::max_element(py, py+4)
    };
}

// Utility: compute glyph device-space bbox from current TextState + CTM.
// Compute glyph bounding box in device space
static Rect compute_glyph_bbox_device(
        const TextState& st, const float ctm[6],
        float adv, float ascender, float descender,
        float font_bbox_x0, float font_bbox_x1) {
    // Step 1: trm_local (text space)
    float trm_local[6];
    compute_trm_local(st, trm_local);

    // Step 2: bbox in font space
    Rect bbox_fs = compute_bbox_fontspace(adv, ascender, descender,
                                          font_bbox_x0, font_bbox_x1, st.wmode);

    // Step 3: trm_device = concat(trm_local, ctm)
    float trm_device[6];
    mat_mul(trm_local, ctm, trm_device);

    // Step 4: transform bbox to device space
    return transform_rect_by_matrix(bbox_fs, trm_device);
}

// ─── Minimal parse helpers for filtering stream ─────────────────────────────

static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_delim(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static bool is_number_start(char c) {
    return c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9');
}

// Advance to the next token in stream (raw bytes)
static size_t skip_ws(const uint8_t* data, size_t pos, size_t len) {
    while (pos < len) {
        if (is_whitespace((char)data[pos])) {
            ++pos;
            continue;
        }
        if ((char)data[pos] == '%') {
            while (pos < len && data[pos] != '\n' && data[pos] != '\r') {
                ++pos;
            }
            continue;
        }
        break;
    }
    return pos;
}

static int hex_to_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static std::vector<uint8_t> parse_literal_string(const uint8_t* data, size_t& pos, size_t len) {
    std::vector<uint8_t> out;
    if (pos >= len || (char)data[pos] != '(') {
        return out;
    }

    ++pos;
    int depth = 1;
    while (pos < len && depth > 0) {
        char ch = (char)data[pos++];
        if (ch == '\\' && pos < len) {
            char esc = (char)data[pos++];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '(': out.push_back('('); break;
                case ')': out.push_back(')'); break;
                case '\\': out.push_back('\\'); break;
                case '\r':
                    if (pos < len && (char)data[pos] == '\n') {
                        ++pos;
                    }
                    break;
                case '\n':
                    break;
                default:
                    if (esc >= '0' && esc <= '7') {
                        int oct = esc - '0';
                        int consumed = 0;
                        while (consumed < 2 && pos < len && data[pos] >= '0' && data[pos] <= '7') {
                            oct = oct * 8 + ((char)data[pos] - '0');
                            ++pos;
                            ++consumed;
                        }
                        out.push_back((uint8_t)(oct & 0xFF));
                    } else {
                        out.push_back((uint8_t)esc);
                    }
                    break;
            }
            continue;
        }

        if (ch == '(') {
            ++depth;
            out.push_back((uint8_t)ch);
            continue;
        }
        if (ch == ')') {
            --depth;
            if (depth > 0) {
                out.push_back((uint8_t)ch);
            }
            continue;
        }
        out.push_back((uint8_t)ch);
    }

    return out;
}

static std::vector<uint8_t> parse_hex_string(const uint8_t* data, size_t& pos, size_t len) {
    std::vector<uint8_t> out;
    if (pos >= len || (char)data[pos] != '<' || (pos + 1 < len && (char)data[pos + 1] == '<')) {
        return out;
    } 

    ++pos;
    int pending = -1;
    while (pos < len && (char)data[pos] != '>') {
        char c = (char)data[pos++];
        if (is_whitespace(c)) {
            continue;
        }

        int nibble = hex_to_nibble(c);
        if (nibble < 0) {
            continue;
        }

        if (pending < 0) {
            pending = nibble;
        } else {
            out.push_back((uint8_t)((pending << 4) | nibble));
            pending = -1;
        }
    }

    if (pending >= 0) {
        out.push_back((uint8_t)(pending << 4));
    }

    if (pos < len && (char)data[pos] == '>') {
        ++pos;
    }
    return out;
}

static std::string parse_name_token(const uint8_t* data, size_t& pos, size_t len) {
    if (pos >= len || (char)data[pos] != '/') {
        return {};
    }
    ++pos;
    size_t start = pos;
    while (pos < len && !is_whitespace((char)data[pos]) && !is_delim((char)data[pos])) {
        ++pos;
    }
    return std::string((const char*)data + start, pos - start);
}

static double parse_number_token(const uint8_t* data, size_t& pos, size_t len) {
    size_t start = pos;
    if ((char)data[pos] == '+' || (char)data[pos] == '-') {
        ++pos;
    }
    while (pos < len && (((char)data[pos] >= '0' && (char)data[pos] <= '9') || (char)data[pos] == '.')) {
        ++pos;
    }
    std::string n((const char*)data + start, pos - start);
    return std::strtod(n.c_str(), nullptr);
}

// ─── Token types ─────────────────────────────────────────────────────────────

enum class TokType { None, Op, Number, String, Name, Array, Dictionary };

struct Token {
    TokType type = TokType::None;
    std::string op;
    double number = 0;
    std::vector<uint8_t> bytes;    // for String
    std::string name_str;          // for Name
    std::vector<Token> items;      // for Array
    std::string dict_raw;          // for Dictionary
};

static bool parse_operand(const uint8_t* data, size_t& pos, size_t len, Token& out);

static std::string parse_dictionary_token(const uint8_t* data, size_t& pos, size_t len) {
    if (pos + 1 >= len || (char)data[pos] != '<' || (char)data[pos + 1] != '<') {
        return {};
    }

    size_t start = pos;
    pos += 2;
    int depth = 1;
    while (pos < len && depth > 0) {
        if ((char)data[pos] == '%') {
            while (pos < len && data[pos] != '\n' && data[pos] != '\r') {
                ++pos;
            }
            continue;
        }

        if ((char)data[pos] == '(') {
            parse_literal_string(data, pos, len);
            continue;
        }

        if ((char)data[pos] == '<') {
            if (pos + 1 < len && (char)data[pos + 1] == '<') {
                ++depth;
                pos += 2;
                continue;
            }
            parse_hex_string(data, pos, len);
            continue;
        }

        if ((char)data[pos] == '>' && pos + 1 < len && (char)data[pos + 1] == '>') {
            --depth;
            pos += 2;
            continue;
        }

        ++pos;
    }

    if (depth != 0 || pos <= start) {
        return {};
    }
    return std::string((const char*)data + start, pos - start);
}

static std::vector<Token> parse_array_token(const uint8_t* data, size_t& pos, size_t len) {
    std::vector<Token> arr;
    if (pos >= len || (char)data[pos] != '[') {
        return arr;
    }

    ++pos;
    while (pos < len) {
        pos = skip_ws(data, pos, len);
        if (pos >= len) {
            break;
        }

        if ((char)data[pos] == ']') {
            ++pos;
            break;
        }

        Token tok;
        size_t before = pos;
        if (!parse_operand(data, pos, len, tok)) {
            ++pos;
            continue;
        }
        arr.push_back(std::move(tok));

        if (pos <= before) {
            ++pos;
        }
    }

    return arr;
}

static bool parse_operand(const uint8_t* data, size_t& pos, size_t len, Token& out) {
    pos = skip_ws(data, pos, len);
    if (pos >= len) {
        return false;
    }

    char c = (char)data[pos];
    if (c == '(') {
        out = {};
        out.type = TokType::String;
        out.bytes = parse_literal_string(data, pos, len);
        return true;
    }
    if (c == '<' && pos + 1 < len && (char)data[pos + 1] == '<') {
        out = {};
        out.type = TokType::Dictionary;
        out.dict_raw = parse_dictionary_token(data, pos, len);
        return !out.dict_raw.empty();
    }
    if (c == '<') {
        out = {};
        out.type = TokType::String;
        out.bytes = parse_hex_string(data, pos, len);
        return true;
    }
    if (c == '/') {
        out = {};
        out.type = TokType::Name;
        out.name_str = parse_name_token(data, pos, len);
        return true;
    }
    if (c == '[') {
        out = {};
        out.type = TokType::Array;
        out.items = parse_array_token(data, pos, len);
        return true;
    }
    if (is_number_start(c)) {
        out = {};
        out.type = TokType::Number;
        out.number = parse_number_token(data, pos, len);
        return true;
    }
    return false;
}

static std::string parse_operator(const uint8_t* data, size_t& pos, size_t len) {
    size_t start = pos;
    while (pos < len && !is_whitespace((char)data[pos]) && !is_delim((char)data[pos])) {
        ++pos;
    }
    return std::string((const char*)data + start, pos - start);
}

// Read a token from the raw PDF stream. Returns the position after the token.
static size_t read_token(const uint8_t* data, size_t pos, size_t len, Token& tok) {
    tok = {};
    pos = skip_ws(data, pos, len);
    if (pos >= len) {
        return pos;
    }

    if (parse_operand(data, pos, len, tok)) {
        return pos;
    }

    if (is_delim((char)data[pos])) {
        return pos + 1;
    }

    std::string op = parse_operator(data, pos, len);
    if (!op.empty()) {
        tok.type = TokType::Op;
        tok.op = std::move(op);
    }
    return pos;
}

static bool is_inline_image_id_token(const uint8_t* data, size_t i, size_t len) {
    if (i + 1 >= len) {
        return false;
    }
    if (data[i] != 'I' || data[i + 1] != 'D') {
        return false;
    }

    const bool prev_ok = (i == 0) || is_whitespace((char)data[i - 1]) || is_delim((char)data[i - 1]);
    const bool next_ok = (i + 2 >= len) || is_whitespace((char)data[i + 2]) || is_delim((char)data[i + 2]);
    return prev_ok && next_ok;
}

static bool is_inline_image_ei_token(const uint8_t* data, size_t i, size_t len) {
    if (i + 1 >= len) {
        return false;
    }
    if (data[i] != 'E' || data[i + 1] != 'I') {
        return false;
    }

    const bool prev_ok = (i > 0) && is_whitespace((char)data[i - 1]);
    
    size_t next = i + 2;
    while (next < len && is_whitespace((char)data[next])) {
        ++next;
    }
    
    if (next >= len) return true;
    
    // In valid PDF, EI is usually followed by Q, EMC, BT, etc.
    // If it's a binary character (e.g. > 127) it's likely false positive.
    if (data[next] > 127 || data[next] < 32) return false;
    
    return prev_ok;
}

// Skip the inline-image payload that follows BI and return the position right after EI.
static size_t skip_inline_image_payload(const uint8_t* data, size_t pos, size_t len) {
    // Skip inline image dictionary until ID.
    while (pos < len) {
        pos = skip_ws(data, pos, len);
        if (pos >= len) {
            return len;
        }

        if (is_inline_image_id_token(data, pos, len)) {
            pos += 2;
            if (pos < len && data[pos] == '\r') {
                ++pos;
                if (pos < len && data[pos] == '\n') {
                    ++pos;
                }
            } else if (pos < len && data[pos] == '\n') {
                ++pos;
            } else if (pos < len && is_whitespace((char)data[pos])) {
                ++pos;
            }
            break;
        }

        Token ignored;
        size_t next = read_token(data, pos, len, ignored);
        if (next <= pos) {
            ++pos;
        } else {
            pos = next;
        }
    }

    // Skip raw inline image bytes until EI.
    while (pos + 1 < len) {
        if (is_inline_image_ei_token(data, pos, len)) {
            return pos + 2;
        }
        ++pos;
    }

    return len;
}

// ============================================================
// Parse and filter the content stream to remove redacted text
// Core processing logic
// ============================================================

// Get glyph advance from font width map (via WinnerZ API)
static float get_glyph_advance(
        const WinExtract::WinFontWidthMap& width_map,
        const std::string& font_name, int code) {
    auto fit = width_map.find(font_name);
    if (fit == width_map.end()) return 0.5f;
    auto& wmap = fit->second;
    auto wit = wmap->find(code);
    if (wit != wmap->end()) return wit->second;
    auto def = wmap->find(-1);
    if (def != wmap->end()) return def->second;
    return 0.5f;
}

static float get_default_ascender(const WinExtract::WinFontVerticalMetricsMap& vm,
                                   const std::string& font_name) {
    auto it = vm.find(font_name);
    return it != vm.end() ? it->second.ascender : 0.8f;
}

static float get_default_descender(const WinExtract::WinFontVerticalMetricsMap& vm,
                                    const std::string& font_name) {
    auto it = vm.find(font_name);
    return it != vm.end() ? it->second.descender : -0.2f;
}

static bool code_in_codespace(uint32_t code_value,
                              int nbytes,
                              const std::vector<WinExtract::WinCodeSpaceRange>& ranges) {
    for (const auto& r : ranges) {
        if (r.nbytes == nbytes && code_value >= r.low && code_value <= r.high) {
            return true;
        }
    }
    return false;
}

// Execute Td: tlm += [tx, ty]; tm = tlm
static void move_text_position(TextState& st, float tx, float ty) {
    // tlm[4] += tx*tlm[0] + ty*tlm[2]
    // tlm[5] += tx*tlm[1] + ty*tlm[3]
    float nx = tx * st.tlm[0] + ty * st.tlm[2];
    float ny = tx * st.tlm[1] + ty * st.tlm[3];
    st.tlm[4] += nx; st.tlm[5] += ny;
    std::copy(st.tlm, st.tlm+6, st.tm);
}

// Update tm after rendering 1 glyph with advance `adv`.
// Advance the text matrix after rendering a character
//   WMode 0: tx = (Tfs*adv + Tc) * Th;  ty = 0
//   WMode 1: tx = 0;  ty = -(Tfs*adv + Tc)  ← negative because text goes down
static void advance_text_matrix(TextState& st, float adv,
                                 float char_spacing_adj = 0.0f) {
    float h = st.h_scale / 100.0f;
    if (st.wmode == 0) {
        // Core redaction logic
        float tx = (adv * st.font_size + char_spacing_adj) * h;
        st.tm[4] += tx * st.tm[0];
        st.tm[5] += tx * st.tm[1];
    } else {
        // WMode 1: text advances in -y direction in text space
        // Core redaction logic
        // tadj = adv * font_size + char_spacing  (already negative when adv>0 since text goes down)
        float ty = -(adv * st.font_size + char_spacing_adj);
        st.tm[4] += ty * st.tm[2];
        st.tm[5] += ty * st.tm[3];
    }
}

// =============================================================================
// Core redaction logic adapted for WinnerZ architecture
//
// Process each character in the string, computing metrics and filtering
//    1. Read code for each character according to CMap/codespace
//    2. Call filter_show_char -> bbox -> should_remove?
//    3. If KEEP: add bytes to current segment
//    4. If REMOVE: flush current segment, then add 1 kerning number
//       (this is Tm_adjust) to compensate for the skipped character, preserving positions of following characters
//    5. Always advance text matrix whether character is removed or not
//
//  Result: list of segments for TJ array:
//    [ (bytes1) kerning1 (bytes2) kerning2 ... ]
//  If a character is removed mid-string, previous part is emitted, kerning is added
//  to compensate for the advance of the removed character, then remainder continues.
// =============================================================================

struct TjSegment {
    bool is_number;          // true = kerning number, false = byte string
    std::vector<uint8_t> bytes;
    float number = 0.0f;     // kerning value (1000x equivalent to TJ)
};

static std::vector<TjSegment> filter_show_string(
        const std::vector<uint8_t>& input_bytes,
        TextState& st,
        const float ctm[6],
        const std::vector<WinRedactZone>& zones,
        const WinExtract::WinFontWidthMap& width_map,
        const WinExtract::WinFontCodeBytesMap& code_bytes_map,
        const WinExtract::WinFontCodeSpaceMap& codespace_map,
        const WinExtract::WinFontVerticalMetricsMap& vm) {

    std::vector<TjSegment> segments;

    // Cache font metrics
    int max_code_bytes = 1;
    {
        auto it = code_bytes_map.find(st.font_name);
        if (it != code_bytes_map.end() && it->second > 0)
            max_code_bytes = it->second;
    }
    const std::vector<WinExtract::WinCodeSpaceRange>* codespace_ranges = nullptr;
    {
        auto it = codespace_map.find(st.font_name);
        if (it != codespace_map.end() && !it->second->empty()) {
            codespace_ranges = it->second.get();
            for (const auto& r : *it->second)
                if (r.nbytes > max_code_bytes)
                    max_code_bytes = r.nbytes;
        }
    }
    float asc = get_default_ascender(vm, st.font_name);
    float dsc = get_default_descender(vm, st.font_name);
    const float font_bbox_x0 = -0.5f, font_bbox_x1 = 0.5f;

    // Accumulate kerning adjustments to compensate for removed characters
    // Core redaction logic
    float Tm_adjust = 0.0f;  // unit: normalized (divided by Tfs before emit)

    // Segment under construction
    std::vector<uint8_t> current_seg;

    size_t i = 0;
    while (i < input_bytes.size()) {
        // ── Step 1: Read code according to codespace ──
        int code = (int)input_bytes[i];
        int consumed = 1;
        {
            const int remaining = (int)(input_bytes.size() - i);
            const int max_len = (std::min)(max_code_bytes, remaining);
            if (max_len > 1) {
                bool matched = false;
                if (codespace_ranges != nullptr) {
                    for (int n = max_len; n >= 1; --n) {
                        uint32_t cand = 0;
                        for (int k = 0; k < n; ++k)
                            cand = (cand << 8) | (uint32_t)input_bytes[i + (size_t)k];
                        if (code_in_codespace(cand, n, *codespace_ranges)) {
                            code = (int)cand; consumed = n; matched = true; break;
                        }
                    }
                }
                if (!matched && codespace_ranges == nullptr) {
                    uint32_t cand = 0;
                    for (int k = 0; k < max_len; ++k)
                        cand = (cand << 8) | (uint32_t)input_bytes[i + (size_t)k];
                    code = (int)cand; consumed = max_len;
                }
            }
        }

        float adv = get_glyph_advance(width_map, st.font_name, code);

        // ── Step 2: Compute device-space bbox and decide ──
        Rect char_bbox = compute_glyph_bbox_device(
            st, ctm, adv, asc, dsc, font_bbox_x0, font_bbox_x1);
        bool remove = should_remove_glyph(char_bbox, zones);

        // ── Step 3: process result ──
        if (!remove) {
            // Keep character: if Tm_adjust is pending, emit it first
            if (Tm_adjust != 0.0f) {
                // Flush current segment if any
                if (!current_seg.empty()) {
                    TjSegment seg;
                    seg.is_number = false;
                    seg.bytes = std::move(current_seg);
                    segments.push_back(std::move(seg));
                    current_seg.clear();
                }
                // Core redaction logic
                // emit Tm_adjust*1000 into TJ array
                TjSegment adj;
                adj.is_number = true;
                // Accumulate kerning adjustments to compensate for removed characters
                // TJ kerning unit is 1/1000 text unit => multiply by 1000
                adj.number = Tm_adjust * 1000.0f;
                segments.push_back(std::move(adj));
                Tm_adjust = 0.0f;
            }
            // Add bytes to current segment
            for (int k = 0; k < consumed; k++)
                current_seg.push_back(input_bytes[i + (size_t)k]);
        } else {
            // Remove character: flush segment, accumulate Tm_adjust
            if (!current_seg.empty()) {
                TjSegment seg;
                seg.is_number = false;
                seg.bytes = std::move(current_seg);
                segments.push_back(std::move(seg));
                current_seg.clear();
            }
            // Core redaction logic
            // Tm_adjust += char_tx / Tfs  (in WMode 0)
            // char_tx = advance of removed character (in text space, before Tfs scale)
            // Tm_adjust unit: normalized advance
            bool is_space = (consumed == 1 && code == 0x20) ||
                            (consumed == 2 && code == 0x0020);
            float tc = st.char_spacing;
            float tw = is_space ? st.word_spacing : 0.0f;
            if (st.wmode == 0) {
                // char_tx = (adv*Tfs + Tc + Tw) * Th  (with Th = h_scale/100)
                // Tm_adjust uses normalized units, i.e., divided by Tfs:
                // Tm_adjust += (adv + (Tc+Tw)/Tfs) * Th
                // Actual scaling adjustment
                //   adjust_text(char_tx / scale, char_ty)
                //   skip_dist = -char_tx / scale = -(adv*Tfs + Tc + Tw) * Th / Th = -(adv*Tfs + Tc + Tw)
                //   And divided by Tfs: skip_dist / Tfs = -(adv + (Tc+Tw)/Tfs)
                // But since emitted x1000, actually:
                //   TJ_value = -(adv*Tfs + Tc + Tw) / Tfs * 1000 * (1/Th)
                // Simpler: TJ_value = -( adv + (Tc+Tw)/Tfs ) * 1000
                float h = st.h_scale / 100.0f;
                (void)h;  // Apply text scaling
                // Normally Th is considered, but since flush_adjustment * 1000 and filter_show_space
                // also * Th, net effect: TJ kerning = -(adv * Tfs + Tc + Tw) / scale * 1000
                // skip_dist / font_size (normalized unit 0..1)
                // We use: Tm_adjust += -(adv * Tfs + Tc + Tw) / Tfs / scale
                // = -(adv + (Tc+Tw)/Tfs) / Th * (-1)... complex.
                // Simplified spacing emission
                // This Tm_adjust will be emitted constructed as (adj.number = Tm_adjust*1000)
                // Compute skipped distance for kerning
                //       Tm_adjust += skip_dist / Tfs  (adjust_text rồi chia Tfs nữa)
                // Accumulate kerning adjustments to compensate for removed characters
                // and char_tx = (w0 + Tc + Tw) * Tfs * Th   (per PDF spec)
                // skip_dist = -char_tx / scale = -(w0 + Tc + Tw) * Tfs  (since scale = Th)
                // Tm_adjust += skip_dist / Tfs = -(w0 + Tc + Tw)
                // adj.number = Tm_adjust * 1000 = -(w0 + Tc + Tw) * 1000
                // WHERE w0 = adv (normalize 0-1), Tc = char_spacing, Tw = word_spacing (if space)
                Tm_adjust += -(adv + (tc + tw) / (st.font_size > 0.001f ? st.font_size : 1.0f));
            } else {
                // WMode 1: Tm_adjust += -(adv*Tfs + Tc + Tw) / Tfs
                Tm_adjust += -(adv + (tc + tw) / (st.font_size > 0.001f ? st.font_size : 1.0f));
            }
        }

        // ── Step 4: Advance text matrix (whether removed or kept) ──
        // Advance the text matrix after rendering a character
        {
            bool is_space = (consumed == 1 && code == 0x20) ||
                            (consumed == 2 && code == 0x0020);
            float extra = st.char_spacing;
            if (is_space) extra += st.word_spacing;
            advance_text_matrix(st, adv, extra);
        }

        i += (size_t)consumed;
    }

    // Flush remaining bytes
    if (!current_seg.empty()) {
        TjSegment seg;
        seg.is_number = false;
        seg.bytes = std::move(current_seg);
        segments.push_back(std::move(seg));
    }

    return segments;
}

// ============================================================
//  WinRedactWriter: write out filtered content stream
// Buffer processor equivalent
// ============================================================

class WinRedactWriter {
public:
    std::string buf;  // Output buffer for modified content stream

    void write(const std::string& s) { buf += s; buf += '\n'; }
    void write_raw(const std::string& s) { buf += s; }

    // Write operator with preceding raw operands
    void emit_op(const std::string& op) {
        buf += ' '; buf += op; buf += '\n';
    }

    // Write string bytes as <hex>
    void emit_string(const std::vector<uint8_t>& b) {
        append_pdf_string(buf, b);
    }

    // Write float number
    void emit_number(double v) {
        char tmp[32]; format_pdf_float(tmp, sizeof(tmp), v);
        buf += ' '; buf += tmp;
    }

    // Write name
    void emit_name(const std::string& n) {
        buf += " /"; buf += n;
    }
};

static void emit_token(WinRedactWriter& out, const Token& tok) {
    switch (tok.type) {
        case TokType::Number:
            out.emit_number(tok.number);
            break;
        case TokType::Name:
            out.emit_name(tok.name_str);
            break;
        case TokType::String:
            out.emit_string(tok.bytes);
            break;
        case TokType::Dictionary:
            out.write_raw(" ");
            out.write_raw(tok.dict_raw);
            break;
        case TokType::Array:
            out.write_raw(" [");
            for (const auto& item : tok.items) {
                emit_token(out, item);
            }
            out.write_raw("]");
            break;
        default:
            break;
    }
}

// ============================================================
//  run_filter — forward declaration (needed for recursive XObject calls)
// ============================================================
static std::string run_filter(
    const std::vector<uint8_t>& stream,
    const float page_ctm[6],
    const std::vector<WinRedactZone>& zones,
    const WinExtract::WinFontWidthMap& width_map,
    const WinExtract::WinFontCodeBytesMap& code_bytes_map,
    const WinExtract::WinFontCodeSpaceMap& codespace_map,
    const WinExtract::WinFontMatrixMap& matrix_map,
    const WinExtract::WinFontVerticalMetricsMap& vm,
    std::shared_ptr<const WinExtract::WinFormXObjectMap> xobj_map,
    std::map<int, std::vector<uint8_t>>* out_updated_xobjects,
    int recursion_depth = 0);


// ============================================================
//  WinRedactInterpreter::run_filter
// Parse and filter the content stream to remove redacted text
// Process Form XObjects recursively
// ============================================================

static std::string run_filter(
        const std::vector<uint8_t>& stream,
    const float page_ctm[6],
        const std::vector<WinRedactZone>& zones,
        const WinExtract::WinFontWidthMap& width_map,
        const WinExtract::WinFontCodeBytesMap& code_bytes_map,
        const WinExtract::WinFontCodeSpaceMap& codespace_map,
        const WinExtract::WinFontMatrixMap& matrix_map,
        const WinExtract::WinFontVerticalMetricsMap& vm,
        std::shared_ptr<const WinExtract::WinFormXObjectMap> xobj_map,
        std::map<int, std::vector<uint8_t>>* out_updated_xobjects,
        int recursion_depth) {

    WinRedactWriter out;
    TextState st;
    bool in_text = false;

    // Operand stack for stream interpretation
    std::vector<Token> operands;
    float ctm[6] = {page_ctm[0], page_ctm[1], page_ctm[2], page_ctm[3], page_ctm[4], page_ctm[5]};
    struct GraphicsStateSnapshot {
        std::array<float, 6> ctm;
        TextState text_state;
        bool in_text = false;
    };
    std::vector<GraphicsStateSnapshot> gstate_stack;

    size_t pos = 0;
    size_t len = stream.size();

    // Main interpreter loop
    while (pos < len) {
        size_t token_start = pos;
        Token tok;
        size_t next = read_token(stream.data(), pos, len, tok);
        if (next <= pos) {
            break;
        }
        pos = next;
        if (tok.type == TokType::None) {
            continue;
        }

        if (tok.type != TokType::Op) {
            operands.push_back(tok);
            continue;
        }

        // ── Process operator ────────────────────────────────────────────

        const std::string& op = tok.op;

        // BI ... ID ... EI — treat inline-image bytes as opaque payload.
        if (op == "BI") {
            size_t inline_end = skip_inline_image_payload(stream.data(), pos, len);
            if (inline_end > token_start && inline_end <= len) {
                out.write_raw(std::string((const char*)stream.data() + token_start, inline_end - token_start));
            }
            pos = inline_end;
            operands.clear();
            continue;
        }

        // BT — Begin Text (clone: in_text = true, reset TM/TLM)
        if (op == "BT") {
            in_text = true;
            std::fill(st.tm,  st.tm  + 6, 0.0f); st.tm[0]  = 1; st.tm[3]  = 1;
            std::fill(st.tlm, st.tlm + 6, 0.0f); st.tlm[0] = 1; st.tlm[3] = 1;
            out.write("BT");
        }

        // ET — End Text
        else if (op == "ET") {
            in_text = false;
            out.write("ET");
        }

        // Graphics state stack and CTM operations
        else if (op == "q") {
            GraphicsStateSnapshot snap;
            snap.ctm = {ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]};
            snap.text_state = st;
            snap.in_text = in_text;
            gstate_stack.push_back(snap);
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("q");
        }
        else if (op == "Q") {
            if (!gstate_stack.empty()) {
                GraphicsStateSnapshot top = gstate_stack.back();
                gstate_stack.pop_back();
                for (int k = 0; k < 6; ++k) {
                    ctm[k] = top.ctm[(size_t)k];
                }
                st = std::move(top.text_state);
                in_text = top.in_text;
            }
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Q");
        }
        else if (op == "cm") {
            if (operands.size() >= 6) {
                bool ok = true;
                float m2[6];
                for (int k = 0; k < 6; ++k) {
                    const Token& t = operands[operands.size() - 6 + (size_t)k];
                    if (t.type != TokType::Number) {
                        ok = false;
                        break;
                    }
                    m2[k] = (float)t.number;
                }
                if (ok) {
                    float outm[6];
                    mat_mul(m2, ctm, outm);
                    for (int k = 0; k < 6; ++k) {
                        ctm[k] = outm[k];
                    }
                }
            }
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("cm");
        }

        // Tf — Set Font (clone: update font name and size in TextState)
        else if (op == "Tf") {
            if (operands.size() >= 2) {
                // /FontName size Tf
                const Token& sz = operands.back();
                const Token& fn = operands[operands.size()-2];
                if (sz.type == TokType::Number) st.font_size = (float)sz.number;
                if (fn.type == TokType::Name)   st.font_name = fn.name_str;

                // Pass-through Tf unmodified since font name does not change
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Tf");
            } else {
                // Fallback: write unmodified
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Tf");
            }
        }

        // Tm — Set Text Matrix (clone: update tm and tlm)
        else if (op == "Tm") {
            if (operands.size() >= 6) {
                bool ok = true;
                float m[6];
                for (int k = 0; k < 6; k++) {
                    const Token& t = operands[operands.size()-6+k];
                    if (t.type != TokType::Number) { ok = false; break; }
                    m[k] = (float)t.number;
                }
                if (ok) {
                    std::copy(m, m+6, st.tm);
                    std::copy(m, m+6, st.tlm);
                }
            }
            // Pass-through Tm
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Tm");
        }

        // Td — Move text position (clone: move_text_position)
        else if (op == "Td") {
            if (operands.size() >= 2) {
                const Token& tx = operands[operands.size()-2];
                const Token& ty = operands[operands.size()-1];
                if (tx.type == TokType::Number && ty.type == TokType::Number)
                    move_text_position(st, (float)tx.number, (float)ty.number);
            }
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Td");
        }

        // TD — Move text position + set leading (clone: leading = -ty, then Td)
        else if (op == "TD") {
            if (operands.size() >= 2) {
                const Token& tx = operands[operands.size()-2];
                const Token& ty = operands[operands.size()-1];
                if (tx.type == TokType::Number && ty.type == TokType::Number) {
                    st.leading = -(float)ty.number;
                    move_text_position(st, (float)tx.number, (float)ty.number);
                }
            }
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("TD");
        }

        // T* — New line (clone: use leading)
        else if (op == "T*") {
            move_text_position(st, 0.0f, -st.leading);
            out.write("T*");
        }

        // TL — Set leading
        else if (op == "TL") {
            if (!operands.empty() && operands.back().type == TokType::Number)
                st.leading = (float)operands.back().number;
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("TL");
        }

        // Tc — char spacing
        else if (op == "Tc") {
            if (!operands.empty() && operands.back().type == TokType::Number)
                st.char_spacing = (float)operands.back().number;
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Tc");
        }

        // Tw — word spacing
        else if (op == "Tw") {
            if (!operands.empty() && operands.back().type == TokType::Number)
                st.word_spacing = (float)operands.back().number;
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Tw");
        }

        // Tz — h_scale
        else if (op == "Tz") {
            if (!operands.empty() && operands.back().type == TokType::Number)
                st.h_scale = (float)operands.back().number;
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Tz");
        }

        // Ts — text rise
        else if (op == "Ts") {
            if (!operands.empty() && operands.back().type == TokType::Number)
                st.text_rise = (float)operands.back().number;
            for (const auto& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op("Ts");
        }

        // Tj — Show string
        // Accumulate kerning adjustments to compensate for removed characters
        // Output: [<bytes1> kerning1 <bytes2> kerning2 ...] TJ
        // Accumulate kerning adjustments to compensate for removed characters
        else if (op == "Tj") {
            if (in_text && !operands.empty() && operands.back().type == TokType::String) {
                const Token& t = operands.back();
                auto segs = filter_show_string(
                    t.bytes, st, ctm, zones,
                    width_map, code_bytes_map, codespace_map, vm);
                std::string arr_content;
                for (const auto& seg : segs) {
                    if (!arr_content.empty()) arr_content += ' ';
                    if (seg.is_number) {
                        char tmp[32]; format_pdf_float(tmp, sizeof(tmp), seg.number);
                        arr_content += tmp;
                    } else {
                        append_pdf_string(arr_content, seg.bytes);
                    }
                }
                if (!arr_content.empty()) {
                    out.write_raw("[");
                    out.write_raw(arr_content);
                    out.write_raw("] TJ\n");
                }
            } else {
                for (const auto& op2 : operands) emit_token(out, op2);
                out.emit_op("Tj");
            }
        }

        // TJ — Show strings with kerning
        // Filter text string and emit surviving segments
        // kerning numbers remain unchanged and update tm, they must also be adjust_text.
        else if (op == "TJ") {
            if (in_text && !operands.empty() && operands.back().type == TokType::Array) {
                std::string arr_content;

                for (const Token& item : operands.back().items) {
                    if (item.type == TokType::String) {
                        // Filter string, get TjSegments
                        auto segs = filter_show_string(
                            item.bytes, st, ctm, zones,
                            width_map, code_bytes_map, codespace_map, vm);
                        for (const auto& seg : segs) {
                            if (!arr_content.empty()) arr_content += ' ';
                            if (seg.is_number) {
                                char tmp[32]; format_pdf_float(tmp, sizeof(tmp), seg.number);
                                arr_content += tmp;
                            } else {
                                append_pdf_string(arr_content, seg.bytes);
                            }
                        }
                    } else if (item.type == TokType::Number) {
                        // TJ kerning number: update tm (clone filter_show_text:1078-1088)
                        // tadj = -num * Tfs * 0.001  (PDF spec)
                        float tadj = (float)(-item.number * st.font_size * 0.001);
                        if (st.wmode == 0) {
                            float tx = tadj * (st.h_scale / 100.0f);
                            st.tm[4] += tx * st.tm[0];
                            st.tm[5] += tx * st.tm[1];
                        } else {
                            st.tm[4] += tadj * st.tm[2];
                            st.tm[5] += tadj * st.tm[3];
                        }
                        // Pass-through kerning number to output
                        if (!arr_content.empty()) arr_content += ' ';
                        char tmp[32]; format_pdf_float(tmp, sizeof(tmp), item.number);
                        arr_content += tmp;
                    }
                }

                if (!arr_content.empty()) {
                    out.write_raw("[");
                    out.write_raw(arr_content);
                    out.write_raw("] TJ\n");
                }
            } else {
                for (const auto& op2 : operands) emit_token(out, op2);
                out.emit_op("TJ");
            }
        }

        // Process individual text strings
        else if (op == "'") {
            move_text_position(st, 0.0f, -st.leading);
            if (in_text && !operands.empty() && operands.back().type == TokType::String) {
                const Token& t = operands.back();
                auto segs = filter_show_string(
                    t.bytes, st, ctm, zones,
                    width_map, code_bytes_map, codespace_map, vm);
                bool any_bytes = false;
                for (const auto& s : segs) if (!s.is_number) { any_bytes = true; break; }
                out.write("T*");
                if (any_bytes) {
                    std::string arr_content;
                    for (const auto& seg : segs) {
                        if (!arr_content.empty()) arr_content += ' ';
                        if (seg.is_number) {
                            char tmp[32]; format_pdf_float(tmp, sizeof(tmp), seg.number);
                            arr_content += tmp;
                        } else {
                            append_pdf_string(arr_content, seg.bytes);
                        }
                    }
                    out.write_raw("[");
                    out.write_raw(arr_content);
                    out.write_raw("] TJ\n");
                }
            } else {
                for (const auto& op2 : operands) emit_token(out, op2);
                out.emit_op("'");
            }
        }

        // Process individual text strings
        else if (op == "\"") {
            if (operands.size() >= 3) {
                const Token& aw  = operands[operands.size()-3];
                const Token& ac  = operands[operands.size()-2];
                const Token& str = operands[operands.size()-1];
                if (aw.type == TokType::Number) st.word_spacing = (float)aw.number;
                if (ac.type == TokType::Number) st.char_spacing = (float)ac.number;
                move_text_position(st, 0.0f, -st.leading);
                if (in_text && str.type == TokType::String) {
                    auto segs = filter_show_string(
                        str.bytes, st, ctm, zones,
                        width_map, code_bytes_map, codespace_map, vm);
                    bool any_bytes = false;
                    for (const auto& s : segs) if (!s.is_number) { any_bytes = true; break; }
                    // Emit Tw, Tc, T*
                    out.emit_number(aw.number); out.emit_op("Tw");
                    out.emit_number(ac.number); out.emit_op("Tc");
                    out.write("T*");
                    if (any_bytes) {
                        std::string arr_content;
                        for (const auto& seg : segs) {
                            if (!arr_content.empty()) arr_content += ' ';
                            if (seg.is_number) {
                                char tmp[32]; format_pdf_float(tmp, sizeof(tmp), seg.number);
                                arr_content += tmp;
                            } else {
                                append_pdf_string(arr_content, seg.bytes);
                            }
                        }
                        out.write_raw("[");
                        out.write_raw(arr_content);
                        out.write_raw("] TJ\n");
                    }
                } else {
                    for (const auto& op2 : operands) emit_token(out, op2);
                    out.emit_op("\"");
                }
            } else {
                for (const auto& op2 : operands) emit_token(out, op2);
                out.emit_op("\"");
            }
        }

        // ── Do — XObject (Image hoặc Form) ──────────────────────────────────
        // Process Form XObjects recursively
        //   Nếu là Form XObject: tính baked_ctm = xobj_matrix * current_ctm
        //   Rồi đệ quy run_filter trên stream của XObject với baked_ctm
        //   Inline kết quả lọc vào stream cha dưới dạng: q [matrix] cm <content> Q
        else if (op == "Do") {
            std::string xobj_name;
            if (!operands.empty() && operands.back().type == TokType::Name)
                xobj_name = operands.back().name_str;

            // Look up in xobj_map (Form XObjects for this page)
            auto xobj_it = xobj_map->find(xobj_name);
            if (xobj_it != xobj_map->end() && xobj_it->second.stream_ptr && !xobj_it->second.stream_ptr->empty()
                && recursion_depth < 8)  // Cycle detection with max depth 8
            {
                const WinExtract::WinFormXObject& xobj = xobj_it->second;

                // Process Form XObjects recursively
                //   transform = concat(xobj_matrix, current_ctm)
                // xobj_matrix is /Matrix in XObject dict
                const auto& xm = xobj.matrix;
                // baked_ctm = xobj_matrix * current_ctm
                float baked_ctm[6];
                mat_mul(xm.data(), ctm, baked_ctm);

                // Merge font maps: xobj has its own fonts, fallback to parent page fonts
                // Resolve resource dictionary
                auto merged_width = width_map;
                for (const auto& kv : xobj.font_width_map)
                    merged_width.insert_or_assign(kv.first, kv.second);

                auto merged_codebytes = code_bytes_map;
                for (const auto& kv : xobj.font_code_bytes_map)
                    merged_codebytes.insert_or_assign(kv.first, kv.second);

                auto merged_codespace = codespace_map;
                for (const auto& kv : xobj.font_codespace_map)
                    merged_codespace.insert_or_assign(kv.first, kv.second);

                auto merged_matrix = matrix_map;
                for (const auto& kv : xobj.font_matrix_map)
                    merged_matrix.insert_or_assign(kv.first, kv.second);

                auto merged_vm = vm;
                for (const auto& kv : xobj.font_vertical_metrics_map)
                    merged_vm.insert_or_assign(kv.first, kv.second);

                // Merge child XObjects (nested XObjects)
                WinExtract::WinFormXObjectMap merged_xobjs = xobj_map ? *xobj_map : WinExtract::WinFormXObjectMap();
                if (xobj.children) {
                    for (const auto& kv : *xobj.children)
                        merged_xobjs.insert_or_assign(kv.first, kv.second);
                }

                if (!xobj.stream_ptr) continue;

                // Accumulate redactions if XObject is placed multiple times
                const std::vector<uint8_t>* input_stream = xobj.stream_ptr.get();
                if (out_updated_xobjects && out_updated_xobjects->count(xobj.obj_id) > 0) {
                    input_stream = &(*out_updated_xobjects)[xobj.obj_id];
                }

                // Recursive: filter XObject content with baked_ctm
                std::string inner = run_filter(
                    *input_stream, baked_ctm, zones,
                    merged_width, merged_codebytes, merged_codespace,
                    merged_matrix, merged_vm,
                    std::make_shared<const WinExtract::WinFormXObjectMap>(merged_xobjs),
                    out_updated_xobjects,
                    recursion_depth + 1);

                if (out_updated_xobjects && xobj.obj_id > 0) {
                    (*out_updated_xobjects)[xobj.obj_id] = std::vector<uint8_t>(inner.begin(), inner.end());
                }

                // IMPORTANT: Emit the original Do operator! Do NOT inline!
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Do");
            }
            else {
                // Image XObject or not in map: pass-through
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Do");
            }
        }

        // Pass through unmodified operators to the output stream
        else {
            for (const Token& op2 : operands) {
                emit_token(out, op2);
            }
            out.emit_op(op);
        }

        operands.clear();
    }

    return out.buf;
}

// ============================================================
//  Public API — RedactionEngine (fully replaces old redactor.cpp)
// ============================================================

// Caller input coordinates: top-down (y increases downwards)
// Need to convert to PDF coords (y increases upwards, origin bottom-left)
static Rect convert_topdown_to_pdf(const Rect& top_down, const Rect& mediabox) {
    return {
        top_down.x0 + mediabox.x0,
        mediabox.y1 - top_down.y1,
        top_down.x1 + mediabox.x0,
        mediabox.y1 - top_down.y0
    };
}

static bool fz_invert_matrix(float inv[6], const float m[6]) {
    float det = m[0] * m[3] - m[1] * m[2];
    if (det == 0.0f) return false;
    float rdet = 1.0f / det;
    inv[0] = m[3] * rdet;
    inv[1] = -m[1] * rdet;
    inv[2] = -m[2] * rdet;
    inv[3] = m[0] * rdet;
    inv[4] = (m[2] * m[5] - m[3] * m[4]) * rdet;
    inv[5] = (m[1] * m[4] - m[0] * m[5]) * rdet;
    return true;
}

// ─── Main public function ────────────────────────────────────────────────────

// Redact text in a page, returning filtered content stream as bytes.
// Caller must overwrite the PDF file by updating /Contents stream.
//
// page_idx: page index (0-based)
// redact_zones_topdown: list of zones to remove, top-down coordinates
// ctm: Current Transformation Matrix của trang (thường là identity for first-pass)
// 
// Parse and filter the content stream to remove redacted text
std::vector<uint8_t> WinnerZ_RedactPage(
        WinExtract::WinPdfDocument& doc,
        int page_idx,
        const std::vector<WinRedactZone_TopDown>& zones_topdown,
        std::map<int, std::vector<uint8_t>>& out_updated_xobjects,
        const float page_ctm[6]) {

    // Get page geometry
    WinExtract::Rect mediabox = doc.get_page_geometry(page_idx).mediabox;
    
    bool has_inv = false;
    float inv[6];
    if (page_ctm) {
        has_inv = fz_invert_matrix(inv, page_ctm);
    }
    
    // Convert coordinates (top-down to PDF)
    std::vector<WinRedactZone> pdf_zones;
    pdf_zones.reserve(zones_topdown.size());
    for (const auto& z : zones_topdown) {
        WinRedactZone pz;
        if (has_inv) {
            pz.rect = transform_rect_by_matrix(z.rect, inv);
        } else {
            pz.rect = convert_topdown_to_pdf(z.rect, mediabox);
        }
        pdf_zones.push_back(pz);
    }

    // Get raw content stream of the page
    std::vector<uint8_t> raw_stream = doc.get_page_content(page_idx);
    if (raw_stream.empty()) return {};

    // Get font maps from WinnerZ pipeline (exactly as WinPdfInterpreter uses)
    auto width_map     = doc.get_page_font_width_map(page_idx);
    auto code_bytes_map= doc.get_page_font_code_bytes_map(page_idx);
    auto codespace_map = doc.get_page_font_codespace_map(page_idx);
    auto matrix_map    = doc.get_page_font_matrix_map(page_idx);
    auto vm_map        = doc.get_page_font_vertical_metrics_map(page_idx);

    // Process Form XObjects recursively
    // Process Form XObjects recursively
    auto xobj_map = doc.get_page_form_xobject_map(page_idx);

    // Identity CTM
    float default_ctm[6] = {1,0,0,1,0,0};

    // Parse and filter the content stream to remove redacted text
    std::string filtered = run_filter(
        raw_stream, default_ctm, pdf_zones,
        width_map, code_bytes_map, codespace_map, matrix_map, vm_map,
        xobj_map, &out_updated_xobjects, /*recursion_depth=*/0);

    return std::vector<uint8_t>(filtered.begin(), filtered.end());
}

} // namespace winnerz
