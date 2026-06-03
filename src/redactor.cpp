// redactor.cpp
// Clone 1:1 logic pdf_redact_text_filter từ fitz/pdf-clean.c
// Không dùng bất kỳ module nào của Fitz hay PDFium.
// Chỉ dùng pipeline WinnerZ: WinPdfDocument + WinPdfInterpreter.
//
// Ánh xạ logic Fitz → WinnerZ:
//   pdf_redact_text_filter (pdf-clean.c:537)  → WinRedactTextFilter::should_remove_glyph()
//   pdf_redact_end_page    (pdf-clean.c:485)  → WinRedactContext::end_page_draw_boxes()
//   pdf_apply_redaction_imp(pdf-clean.c:1122) → WinRedactContext::apply()
//   pdf_filter_page_contents (pdf-clean.c:395)→ WinRedactInterpreter::run_filter()
//   pdf_update_stream      (pdf-clean.c:420)  → WinRedactWriter::commit()

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
//  Các kiểu nội bộ (mirror fitz structs, không phụ thuộc fitz)
// ============================================================

using WinExtract::Rect;
using WinExtract::Vec2;
using WinExtract::Quad;

// Một vùng redact (tương đương annotation Subtype=Redact trong Fitz)
struct WinRedactZone {
    Rect rect;   // PDF coordinates (bottom-left origin)
};

// ─── Helpers hình học ────────────────────────────────────────────────────────

static bool rects_are_valid_and_intersect(const Rect& a, const Rect& b) {
    // Clone: fz_is_valid_rect(fz_intersect_rect(bbox, r)) (pdf-clean.c:577)
    // Giao của 2 rect không rỗng là 1 rect hợp lệ
    float ix0 = (std::max)(a.x0, b.x0);
    float iy0 = (std::max)(a.y0, b.y0);
    float ix1 = (std::min)(a.x1, b.x1);
    float iy1 = (std::min)(a.y1, b.y1);
    // "valid rect": x0 <= x1 AND y0 <= y1  (Fitz: fz_is_valid_rect)
    return ix0 <= ix1 && iy0 <= iy1;
}

static bool rect_contains(const Rect& outer, const Rect& inner) {
    // Clone: fz_contains_rect (dùng trong rect_touches_redactions, pdf-clean.c:964)
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

// Clone: rect_touches_redactions (pdf-clean.c:938)
// Trả về: 0 = không chạm, 1 = chạm nhưng không bao hết, 2 = vùng redact bao hết bbox
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
//  WinRedactTextFilter — clone pdf_redact_text_filter
// ============================================================

// Clone 1:1 từ pdf_redact_text_filter (pdf-clean.c:537–591)
//
// Fitz shrink bbox 1/10 theo cả 2 chiều trước khi kiểm tra giao:
//   bbox.x0 += w/10;  bbox.x1 -= w/10;
//   bbox.y0 += h/10;  bbox.y1 -= h/10;
// Sau đó kiểm tra fz_is_valid_rect(fz_intersect_rect(char_bbox, redact_rect))
//
// Trả về true nếu ký tự NÊN BỊ XOÁ.
static bool should_remove_glyph(const Rect& raw_char_bbox,
                                 const std::vector<WinRedactZone>& zones) {
    Rect bbox = raw_char_bbox;

    // Clone: shrink 1/10 theo mỗi chiều (pdf-clean.c:552–557)
    float w = bbox.x1 - bbox.x0;
    float h = bbox.y1 - bbox.y0;
    bbox.x0 += w / 10.0f;
    bbox.x1 -= w / 10.0f;
    bbox.y0 += h / 10.0f;
    bbox.y1 -= h / 10.0f;

    for (const auto& z : zones) {
        // Clone: fz_is_valid_rect(fz_intersect_rect(bbox, r)) (pdf-clean.c:577,584)
        if (rects_are_valid_and_intersect(bbox, z.rect))
            return true;
    }
    return false;
}

// ============================================================
//  Token writer — ghi lại PDF operator stream (không dùng Fitz Buffer)
// ============================================================

// Ghi một chuỗi byte (string literal PDF) theo cú pháp (hex) hoặc text
static void append_pdf_string(std::string& out, const std::vector<uint8_t>& data) {
    // Dùng hex string để an toàn với mọi byte: <AABB...>
    out += '<';
    char buf[3];
    for (uint8_t b : data) {
        snprintf(buf, sizeof(buf), "%02X", b);
        out += buf;
    }
    out += '>';
}

static void append_float(std::string& out, float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v);
    out += buf;
}

// ============================================================
//  WinRedactInterpreter
//  Đây là "pdf_filter_content_stream" của WinnerZ:
//  Chạy qua Content Stream, với mỗi toán tử văn bản, quyết định
//  giữ hay bỏ từng byte dựa theo should_remove_glyph().
// ============================================================

// Trạng thái text state tối giản (mirror TextState trong pdf_engine.cpp)
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

// Ma trận nhân: m = a * b (PDF column-major 3x3)
static void mat_mul(const float a[6], const float b[6], float out[6]) {
    out[0] = a[0]*b[0] + a[1]*b[2];
    out[1] = a[0]*b[1] + a[1]*b[3];
    out[2] = a[2]*b[0] + a[3]*b[2];
    out[3] = a[2]*b[1] + a[3]*b[3];
    out[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    out[5] = a[4]*b[1] + a[5]*b[3] + b[5];
}

// =============================================================================
//  TRM / Glyph BBox  —   pdf-op-filter.c::filter_show_char (673+)
//
//  Fitz tách riêng 2 bước:
//    1. trm_local  = Tfs × Th × Tm  (text space, chưa nhân CTM)
//    2. ctm        = pending.ctm ⊕ sent.ctm ⊕ page_transform
//    3. bbox       = fontspace bbox  (dùng ascender/descender từ font metrics)
//    4. Gọi text_filter(trm_local, ctm, bbox_fontspace)
//    5. Bên trong text_filter: trm_device = concat(trm_local, ctm)
//                              bbox_device = transform_rect(bbox_fontspace, trm_device)
//    6. shrink bbox_device 1/10 rồi intersect test
// =============================================================================

// Bước 1: Tính trm LOCAL (text space). Không nhân CTM.
// pdf_tos_make_trm: Tfs * Th * Tm (đặt vào trm_local)
static void compute_trm_local(const TextState& st, float trm_local[6]) {
    float h  = st.h_scale / 100.0f;
    float fs = st.font_size;
    float rise = st.text_rise;
    // Theo PDF spec: trm = [ Tfs*Th*a  Tfs*Th*b  Tfs*c  Tfs*d  Trise*c+e  Trise*d+f ]
    trm_local[0] = fs * h * st.tm[0];
    trm_local[1] = fs * h * st.tm[1];
    trm_local[2] = fs * st.tm[2];
    trm_local[3] = fs * st.tm[3];
    trm_local[4] = rise * st.tm[2] + st.tm[4];
    trm_local[5] = rise * st.tm[3] + st.tm[5];
}

// Bước 2: Tính bbox trong FONT SPACE — x,y dùng ascender/descender từ font metrics.
// Clone pdf-op-filter.c:698–711:
//   WMode 0: bbox = { x0=0, y0=descender, x1=adv, y1=ascender }
//   WMode 1: bbox = { x0=font_bbox_x0, y0=0, x1=font_bbox_x1, y1=adv_vert }
//   font_bbox_x0/x1: Fitz lấy từ fz_font_bbox. Ta xấp xỉ bằng [-0.5, 0.5] cho CJK.
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

// Bước 3: Transform bbox từ font space → device space.
// Clone pdf-clean.c:548–549:
//   trm_device = concat(trm_local, ctm)
//   bbox_device = transform_rect(bbox_fontspace, trm_device)
static Rect transform_rect_by_matrix(const Rect& r, const float m[6]) {
    // Transform 4 góc của rect, lấy AABB
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

// Hàm tiện ích: tính glyph bbox device-space từ TextState + CTM hiện tại.
// Đây là hàm thay thế compute_glyph_bbox cũ, logic đã được đồng bộ với Fitz.
static Rect compute_glyph_bbox_device(
        const TextState& st, const float ctm[6],
        float adv, float ascender, float descender,
        float font_bbox_x0, float font_bbox_x1) {
    // Bước 1: trm_local (text space)
    float trm_local[6];
    compute_trm_local(st, trm_local);

    // Bước 2: bbox trong font space
    Rect bbox_fs = compute_bbox_fontspace(adv, ascender, descender,
                                          font_bbox_x0, font_bbox_x1, st.wmode);

    // Bước 3: trm_device = concat(trm_local, ctm)
    float trm_device[6];
    mat_mul(trm_local, ctm, trm_device);

    // Bước 4: transform bbox → device space
    return transform_rect_by_matrix(bbox_fs, trm_device);
}

// ─── Parse helpers cực nhỏ cho stream lọc ───────────────────────────────────

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

// Tiến tới token tiếp theo trong stream (raw bytes)
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
    std::vector<uint8_t> bytes;    // cho String
    std::string name_str;          // cho Name
    std::vector<Token> items;      // cho Array
    std::string dict_raw;          // cho Dictionary
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

// Đọc một token từ stream PDF thô. Trả về vị trí sau token.
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
    const bool next_ok = (i + 2 >= len) || is_whitespace((char)data[i + 2]) || is_delim((char)data[i + 2]);
    return prev_ok && next_ok;
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
//  Core: lọc content stream — hàm tương đương pdf_filter_content_stream
//  trong Fitz, nhưng chỉ tập trung vào text filtering (text_filter).
// ============================================================

// Lấy glyph advance từ font width map (gọi qua WinnerZ API)
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

// Thực hiện Td:  tlm += [tx, ty];  tm = tlm
static void move_text_position(TextState& st, float tx, float ty) {
    // tlm[4] += tx*tlm[0] + ty*tlm[2]
    // tlm[5] += tx*tlm[1] + ty*tlm[3]
    float nx = tx * st.tlm[0] + ty * st.tlm[2];
    float ny = tx * st.tlm[1] + ty * st.tlm[3];
    st.tlm[4] += nx; st.tlm[5] += ny;
    std::copy(st.tlm, st.tlm+6, st.tm);
}

// Cập nhật tm sau khi vẽ 1 glyph có advance `adv`.
// Clone pdf_tos_move_after_char (Fitz):
//   WMode 0: tx = (Tfs*adv + Tc) * Th;  ty = 0
//   WMode 1: tx = 0;  ty = -(Tfs*adv + Tc)  ← âm vì text đi xuống
static void advance_text_matrix(TextState& st, float adv,
                                 float char_spacing_adj = 0.0f) {
    float h = st.h_scale / 100.0f;
    if (st.wmode == 0) {
        // Clone pdf-op-filter.c filter_show_space: pre_translate(tm, tadj*scale, 0)
        float tx = (adv * st.font_size + char_spacing_adj) * h;
        st.tm[4] += tx * st.tm[0];
        st.tm[5] += tx * st.tm[1];
    } else {
        // WMode 1: text advances in -y direction in text space
        // Clone pdf-op-filter.c: pre_translate(tm, 0, tadj)
        // tadj = adv * font_size + char_spacing  (đã là âm khi adv>0 vì text đi xuống)
        float ty = -(adv * st.font_size + char_spacing_adj);
        st.tm[4] += ty * st.tm[2];
        st.tm[5] += ty * st.tm[3];
    }
}

// =============================================================================
//  filter_show_string — Clone 1:1 từ Fitz filter_show_string (pdf-op-filter.c:986)
//
//  Fitz xử lý từng ký tự trong chuỗi:
//    1. Đọc code từng ký tự theo CMap/codespace
//    2. Gọi filter_show_char → bbox → should_remove?
//    3. Nếu GIỮ: cộng bytes vào segment hiện tại
//    4. Nếu XÓA: flush segment hiện tại, rồi thêm 1 kerning number
//       (đây là Tm_adjust) để bù trừ cho ký tự đã bỏ, giữ nguyên vị trí các ký tự kế tiếp
//    5. Tiếp tục advance text matrix dù ký tự có bị xóa hay không
//
//  Kết quả: danh sách các segment dùng cho mảng TJ:
//    [ (bytes1) kerning1 (bytes2) kerning2 ... ]
//  Nếu xóa 1 ký tự giữa chuỗi, phần trước được emit, rồi thêm kerning bù trừ
//  cho advance của ký tự đó, rồi tiếp tục phần sau.
// =============================================================================

struct TjSegment {
    bool is_number;          // true = kerning number, false = byte string
    std::vector<uint8_t> bytes;
    float number = 0.0f;     // kerning value (1000× tương đương TJ)
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

    // Lưu cache font metrics
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

    // Fitz: Tm_adjust — tích lũy kerning bù trừ cho các ký tự bị xóa
    // (pdf-op-filter.c:937–942 adjust_text())
    float Tm_adjust = 0.0f;  // đơn vị: normalized (chia cho Tfs trước khi emit)

    // Segment đang xây dựng
    std::vector<uint8_t> current_seg;

    size_t i = 0;
    while (i < input_bytes.size()) {
        // ── Bước 1: Đọc code theo codespace (clone pdf_decode_cmap) ──
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

        // ── Bước 2: Tính device-space bbox và quyết định (clone filter_show_char) ──
        Rect char_bbox = compute_glyph_bbox_device(
            st, ctm, adv, asc, dsc, font_bbox_x0, font_bbox_x1);
        bool remove = should_remove_glyph(char_bbox, zones);

        // ── Bước 3: xử lý kết quả (clone filter_string_to_segment p.901–931) ──
        if (!remove) {
            // 保守 ký tự: nếu có Tm_adjust đang chờ, emit nó rước
            if (Tm_adjust != 0.0f) {
                // Flush current segment nếu có
                if (!current_seg.empty()) {
                    TjSegment seg;
                    seg.is_number = false;
                    seg.bytes = std::move(current_seg);
                    segments.push_back(std::move(seg));
                    current_seg.clear();
                }
                // Clone adjust_text + flush_adjustment (pdf-op-filter.c:953–973):
                // emit Tm_adjust*1000 vào mảng TJ
                TjSegment adj;
                adj.is_number = true;
                // Tm_adjust trong Fitz đơn vị normalized (chia Tfs).  
                // TJ kerning đơn vị 1/1000 text unit ⇒ nhân 1000
                adj.number = Tm_adjust * 1000.0f;
                segments.push_back(std::move(adj));
                Tm_adjust = 0.0f;
            }
            // Thêm bytes vào segment hiện tại
            for (int k = 0; k < consumed; k++)
                current_seg.push_back(input_bytes[i + (size_t)k]);
        } else {
            // Xóa ký tự: flush segment, accumulate Tm_adjust
            if (!current_seg.empty()) {
                TjSegment seg;
                seg.is_number = false;
                seg.bytes = std::move(current_seg);
                segments.push_back(std::move(seg));
                current_seg.clear();
            }
            // Clone adjust_text (pdf-op-filter.c:937–942):
            // Tm_adjust += char_tx / Tfs  (trong WMode 0)
            // char_tx = advance của ký tự bị xóa (trong text space, trước scale Tfs)
            // Tm_adjust đơn vị: normalized advance
            bool is_space = (consumed == 1 && code == 0x20) ||
                            (consumed == 2 && code == 0x0020);
            float tc = st.char_spacing;
            float tw = is_space ? st.word_spacing : 0.0f;
            if (st.wmode == 0) {
                // char_tx = (adv*Tfs + Tc + Tw) * Th  (với Th = h_scale/100)
                // Tm_adjust sử dụng normalized, tức là chia Tfs:
                // Tm_adjust += (adv + (Tc+Tw)/Tfs) * Th
                // Nưng Fitz thực sự làm: 
                //   adjust_text(char_tx / scale, char_ty)
                //   skip_dist = -char_tx / scale = -(adv*Tfs + Tc + Tw) * Th / Th = -(adv*Tfs + Tc + Tw)
                //   Và chia Tfs: skip_dist / Tfs = -(adv + (Tc+Tw)/Tfs)
                // Nhưng vì emit x1000, thực tế:
                //   TJ_value = -(adv*Tfs + Tc + Tw) / Tfs * 1000 * (1/Th)
                // Đơn giản hơn: TJ_value = -( adv + (Tc+Tw)/Tfs ) * 1000
                float h = st.h_scale / 100.0f;
                (void)h;  // Fitz chia scale (Th) trong adjust_text, rồi nhân lại kâu emit
                // Vốn có xét Th nhưng vì flush_adjustment * 1000 rồi filter_show_space
                // cũng * Th, nên net effect: TJ kerning = -(adv * Tfs + Tc + Tw) / scale * 1000
                // Clone: skip_dist / font_size (đƣn vị normalized 0..1)
                // Ta dùng: Tm_adjust += -(adv * Tfs + Tc + Tw) / Tfs / scale
                // = -(adv + (Tc+Tw)/Tfs) / Th * (-1)... phức tạp.
                // Đơn giản nhất khop với cách fitz emit:
                // Tm_adjust này sẽ được emit xây dựng như (adj.number = Tm_adjust*1000)
                // Fitz: skip_dist = char_tx / scale  (pdf-op-filter.c:939)
                //       Tm_adjust += skip_dist / Tfs  (adjust_text rồi chia Tfs nữa)
                // Thực tế Fitz emit: Tm_adjust * 1000 vào TJ array
                // và char_tx = (w0 + Tc + Tw) * Tfs * Th   (theo PDF spec)
                // skip_dist = -char_tx / scale = -(w0 + Tc + Tw) * Tfs  (vì scale = Th)
                // Tm_adjust += skip_dist / Tfs = -(w0 + Tc + Tw)
                // adj.number = Tm_adjust * 1000 = -(w0 + Tc + Tw) * 1000
                // TRONG đó w0 = adv (normalize 0-1), Tc = char_spacing, Tw = word_spacing (nếu space)
                Tm_adjust += -(adv + (tc + tw) / (st.font_size > 0.001f ? st.font_size : 1.0f));
            } else {
                // WMode 1: Tm_adjust += -(adv*Tfs + Tc + Tw) / Tfs
                Tm_adjust += -(adv + (tc + tw) / (st.font_size > 0.001f ? st.font_size : 1.0f));
            }
        }

        // ── Bước 4: Advance text matrix (dù xóa hay giữ) ──
        // Clone: filter_show_char gọi pdf_tos_move_after_char sau khi trả kết quả (dòng 724)
        {
            bool is_space = (consumed == 1 && code == 0x20) ||
                            (consumed == 2 && code == 0x0020);
            float extra = st.char_spacing;
            if (is_space) extra += st.word_spacing;
            advance_text_matrix(st, adv, extra);
        }

        i += (size_t)consumed;
    }

    // Flush các byte cuối cùng
    if (!current_seg.empty()) {
        TjSegment seg;
        seg.is_number = false;
        seg.bytes = std::move(current_seg);
        segments.push_back(std::move(seg));
    }

    return segments;
}

// ============================================================
//  WinRedactWriter: ghi lại content stream đã lọc
//  Tương đương pdf_new_buffer_processor trong Fitz
// ============================================================

class WinRedactWriter {
public:
    std::string buf;  // Output buffer (ASCII, tương đương fz_buffer của Fitz)

    void write(const std::string& s) { buf += s; buf += '\n'; }
    void write_raw(const std::string& s) { buf += s; }

    // Ghi operator với các operand thô đứng trước
    void emit_op(const std::string& op) {
        buf += ' '; buf += op; buf += '\n';
    }

    // Ghi string bytes dưới dạng <hex>
    void emit_string(const std::vector<uint8_t>& b) {
        append_pdf_string(buf, b);
    }

    // Ghi số float
    void emit_number(double v) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", v);
        buf += ' '; buf += tmp;
    }

    // Ghi name
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
//  run_filter — forward declaration (cần vì đệ quy cho XObject)
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
    int recursion_depth = 0);


// ============================================================
//  WinRedactInterpreter::run_filter
//  Tim của hệ thống: clone pdf_filter_content_stream + pdf_filter_Do_form.
//  Xử lý text redact và đệ quy vào Form XObject (giống Fitz instance_forms=1).
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
        int recursion_depth) {

    WinRedactWriter out;
    TextState st;
    bool in_text = false;

    // Operand stack (mirrors Fitz processor operand stack)
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

    // Vòng lặp chính — clone logic process_op trong Fitz interpreter
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

        // ── Xử lý operator ──────────────────────────────────────────────

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

        // q/Q/cm — Graphics state stack + CTM concat (clone Fitz interpreter)
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

        // Tf — Set Font (clone: cập nhật tên font và size trong TextState)
        else if (op == "Tf") {
            if (operands.size() >= 2) {
                // /FontName size Tf
                const Token& sz = operands.back();
                const Token& fn = operands[operands.size()-2];
                if (sz.type == TokType::Number) st.font_size = (float)sz.number;
                if (fn.type == TokType::Name)   st.font_name = fn.name_str;

                // Pass-through Tf nguyên xi vì font name không thay đổi
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Tf");
            } else {
                // Fallback: ghi nguyên
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Tf");
            }
        }

        // Tm — Set Text Matrix (clone: cập nhật tm và tlm)
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
        // Clone Fitz filter_show_string (pdf-op-filter.c:986): emit segments có Tm_adjust
        // Output: [<bytes1> kerning1 <bytes2> kerning2 ...] TJ
        // (Fitz cũng chúašt Tj thành TJ để có thể chen số kerning bù trừ)
        else if (op == "Tj") {
            if (in_text && !operands.empty() && operands.back().type == TokType::String) {
                const Token& t = operands.back();
                auto segs = filter_show_string(
                    t.bytes, st, ctm, zones,
                    width_map, code_bytes_map, codespace_map, vm);
                // Kiểm tra có segment nào không (có bytes cần emit)
                bool any_bytes = false;
                for (const auto& s : segs) if (!s.is_number) { any_bytes = true; break; }
                if (any_bytes) {
                    // Emit như TJ array (Fitz: op_Tj takes raw bytes string, nhưng ta cần TJ vì có kerning)
                    std::string arr_content;
                    for (const auto& seg : segs) {
                        if (!arr_content.empty()) arr_content += ' ';
                        if (seg.is_number) {
                            char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", seg.number);
                            arr_content += tmp;
                        } else {
                            append_pdf_string(arr_content, seg.bytes);
                        }
                    }
                    out.write_raw("[");
                    out.write_raw(arr_content);
                    out.write_raw("] TJ\n");
                }
                // Nếu không có byte nào được giữ: không emit gì (toàn bộ bị redact)
            } else {
                for (const auto& op2 : operands) emit_token(out, op2);
                out.emit_op("Tj");
            }
        }

        // TJ — Show strings with kerning
        // Clone Fitz filter_show_text (pdf-op-filter.c:1020): mỗi item string được filter_show_string,
        // kerning numbers giữ nguyên và update tm, phim cần also được adjust_text.
        else if (op == "TJ") {
            if (in_text && !operands.empty() && operands.back().type == TokType::Array) {
                std::string arr_content;
                bool any_bytes = false;

                for (const Token& item : operands.back().items) {
                    if (item.type == TokType::String) {
                        // Filter string, get TjSegments
                        auto segs = filter_show_string(
                            item.bytes, st, ctm, zones,
                            width_map, code_bytes_map, codespace_map, vm);
                        for (const auto& seg : segs) {
                            if (!arr_content.empty()) arr_content += ' ';
                            if (seg.is_number) {
                                char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", seg.number);
                                arr_content += tmp;
                            } else {
                                append_pdf_string(arr_content, seg.bytes);
                                any_bytes = true;
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
                        char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", item.number);
                        arr_content += tmp;
                    }
                }

                if (any_bytes) {
                    out.write_raw("[");
                    out.write_raw(arr_content);
                    out.write_raw("] TJ\n");
                }
            } else {
                for (const auto& op2 : operands) emit_token(out, op2);
                out.emit_op("TJ");
            }
        }

        // ' operator — T* + Tj (clone Fitz: same as BT/use-leading + filter_show_text)
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
                            char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", seg.number);
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

        // " operator — aw ac string " (clone Fitz: Tw Tc T* then filter_show_text)
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
                                char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", seg.number);
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
        // Clone Fitz pdf_filter_Do_form (pdf-op-filter.c:2616) với instance_forms=1:
        //   Nếu là Form XObject: tính baked_ctm = xobj_matrix * current_ctm
        //   Rồi đệ quy run_filter trên stream của XObject với baked_ctm
        //   Inline kết quả lọc vào stream cha dưới dạng: q [matrix] cm <content> Q
        else if (op == "Do") {
            std::string xobj_name;
            if (!operands.empty() && operands.back().type == TokType::Name)
                xobj_name = operands.back().name_str;

            // Tìm trong xobj_map (Form XObjects của trang này)
            auto xobj_it = xobj_map->find(xobj_name);
            if (xobj_it != xobj_map->end() && xobj_it->second.stream_ptr && !xobj_it->second.stream_ptr->empty()
                && recursion_depth < 8)  // Clone Fitz cycle detection (max depth 8)
            {
                const WinExtract::WinFormXObject& xobj = xobj_it->second;

                // Clone pdf_filter_xobject_instance (pdf-clean.c:368-369):
                //   transform = concat(xobj_matrix, current_ctm)
                // xobj_matrix là /Matrix trong dict của XObject
                const auto& xm = xobj.matrix;
                // baked_ctm = xobj_matrix * current_ctm
                float baked_ctm[6];
                mat_mul(xm.data(), ctm, baked_ctm);

                // Merge font maps: xobj có font riêng, fallback lên font của trang cha
                // Clone Fitz: old_res = xobj_res hoặc page_res nếu không có
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

                // Merge child XObjects (XObjects lồng nhau)
                WinExtract::WinFormXObjectMap merged_xobjs = xobj_map ? *xobj_map : WinExtract::WinFormXObjectMap();
                if (xobj.children) {
                    for (const auto& kv : *xobj.children)
                        merged_xobjs.insert_or_assign(kv.first, kv.second);
                }

                if (!xobj.stream_ptr) continue;

                // Đệ quy: lọc nội dung của XObject với baked_ctm
                std::string inner = run_filter(
                    *xobj.stream_ptr, baked_ctm, zones,
                    merged_width, merged_codebytes, merged_codespace,
                    merged_matrix, merged_vm,
                    std::make_shared<const WinExtract::WinFormXObjectMap>(merged_xobjs), recursion_depth + 1);

                if (!inner.empty()) {
                    // Inline kết quả: q [xobj_matrix] cm [bbox] re W n <filtered_content> Q
                    // Clone Fitz: bake matrix vào stream thay vì emit Do
                    out.write_raw("q\n");
                    // Emit xobj_matrix như cm operator
                    char cm_buf[256];
                    snprintf(cm_buf, sizeof(cm_buf), "%g %g %g %g %g %g cm\n",
                             xm[0], xm[1], xm[2], xm[3], xm[4], xm[5]);
                    out.write_raw(cm_buf);
                    
                    if (xobj.has_bbox) {
                        char bbox_buf[256];
                        snprintf(bbox_buf, sizeof(bbox_buf), "%g %g %g %g re W n\n",
                                 xobj.bbox[0], xobj.bbox[1], xobj.bbox[2] - xobj.bbox[0], xobj.bbox[3] - xobj.bbox[1]);
                        out.write_raw(bbox_buf);
                    }
                    
                    out.write_raw(inner);
                    out.write_raw("\nQ\n");
                }
                // Nếu inner rỗng (toàn bộ nội dung bị redact), bỏ Do hoàn toàn
            }
            else {
                // Image XObject hoặc không có trong map: pass-through
                for (const auto& op2 : operands) {
                    emit_token(out, op2);
                }
                out.emit_op("Do");
            }
        }

        // Tất cả operator khác: pass-through nguyên xi (clone Fitz buffer_processor passthrough)
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
//  Public API — RedactionEngine (thay thế hoàn toàn redactor.cpp cũ)
// ============================================================

// Tọa độ đầu vào của caller: top-down (y tăng xuống dưới)
// Cần đổi sang PDF coords (y tăng lên trên, origin góc dưới-trái)
static Rect convert_topdown_to_pdf(const Rect& top_down, const Rect& mediabox) {
    return {
        top_down.x0 + mediabox.x0,
        mediabox.y1 - top_down.y1,
        top_down.x1 + mediabox.x0,
        mediabox.y1 - top_down.y0
    };
}

// ─── Hàm public chính ────────────────────────────────────────────────────────

// Redact text trong một trang, trả về content stream đã lọc dưới dạng bytes.
// Caller sau đó tự ghi đè lên file PDF bằng cách cập nhật /Contents stream.
//
// page_idx: index trang (0-based)
// redact_zones_topdown: danh sách vùng cần xoá, toạ độ top-down
// ctm: Current Transformation Matrix của trang (thường là identity for first-pass)
// 
// Clone: pdf_apply_redaction_imp → pdf_filter_page_contents → pdf_filter_content_stream
std::vector<uint8_t> WinnerZ_RedactPage(
        WinExtract::WinPdfDocument& doc,
        int page_idx,
        const std::vector<WinRedactZone_TopDown>& zones_topdown,
        const float page_ctm[6]) {

    // Lấy page geometry
    WinExtract::Rect mediabox = doc.get_page_mediabox(page_idx);
    // Chuyển đổi tọa độ (top-down → PDF)
    std::vector<WinRedactZone> pdf_zones;
    pdf_zones.reserve(zones_topdown.size());
    for (const auto& z : zones_topdown) {
        WinRedactZone pz;
        pz.rect = convert_topdown_to_pdf(z.rect, mediabox);
        pdf_zones.push_back(pz);
    }

    // Lấy content stream thô của trang
    std::vector<uint8_t> raw_stream = doc.get_page_content(page_idx);
    if (raw_stream.empty()) return {};

    // Lấy font maps từ WinnerZ pipeline (y hệt cách WinPdfInterpreter dùng)
    auto width_map     = doc.get_page_font_width_map(page_idx);
    auto code_bytes_map= doc.get_page_font_code_bytes_map(page_idx);
    auto codespace_map = doc.get_page_font_codespace_map(page_idx);
    auto matrix_map    = doc.get_page_font_matrix_map(page_idx);
    auto vm_map        = doc.get_page_font_vertical_metrics_map(page_idx);

    // Lấy Form XObject map — clone Fitz: mỗi Do /Formx được xử lý đệ quy
    // tương đương pdf_filter_Do_form với instance_forms=1
    auto xobj_map = doc.get_page_form_xobject_map(page_idx);

    // Identity CTM nếu caller không truyền
    float default_ctm[6] = {1,0,0,1,0,0};
    const float* ctm = page_ctm ? page_ctm : default_ctm;

    // ── Chạy filter (clone pdf_filter_content_stream + pdf_filter_Do_form) ──
    std::string filtered = run_filter(
        raw_stream, ctm, pdf_zones,
        width_map, code_bytes_map, codespace_map, matrix_map, vm_map,
        xobj_map, /*recursion_depth=*/0);

    return std::vector<uint8_t>(filtered.begin(), filtered.end());
}

} // namespace winnerz
