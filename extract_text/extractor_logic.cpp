#include "extractor_logic.hpp"
#include "ucdn.hpp"
#include <algorithm>
#include <cmath>

namespace WinExtract {

// --- HẰNG SỐ CHUẨN CỦA MUPDF ---
constexpr float PARAGRAPH_DIST = 1.5f;
constexpr float SPACE_DIST = 0.15f;
constexpr float SPACE_MAX_DIST = 0.8f;
constexpr float BASE_MAX_DIST = 0.8f;
constexpr float FAKE_BOLD_MAX_DIST = 0.05f;

struct InternalMatrix { float a, b, c, d, e, f; };

static Vec2 transform_vec(Vec2 v, const InternalMatrix& m) {
    return {v.x * m.a + v.y * m.c, v.x * m.b + v.y * m.d};
}

static Vec2 normalize_vec(Vec2 v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len > 0) return {v.x / len, v.y / len};
    return {0, 0};
}

static float vec_dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

static float matrix_expansion(const InternalMatrix& m) {
    return std::sqrt(std::abs(m.a * m.d - m.b * m.c));
}

static bool may_add_space(int lastchar) {
    return (lastchar != ' ' && (lastchar < 0x700 || (lastchar >= 0x2000 && lastchar <= 0x20CF)));
}

static bool is_unicode_hyphen(int c) {
    return (c == '-'    || c == 0xAD   || c == 0x2010 || c == 0x2011 || 
            c == 0x2012 || c == 0x2013 || c == 0x2014 || c == 0x2015 || 
            c == 0x2212 || c == 0xFE63 || c == 0xFF0D);
}

static bool plausible_bullet(int c) {
    switch (c) {
        case '*': case 0x00B7: case 0x2022: case 0x2023: case 0x2043:
        case 0x204C: case 0x204D: case 0x2219: case 0x25C9: case 0x25CB:
        case 0x25CF: case 0x25D8: case 0x25E6: case 0x2619: case 0x261A:
        case 0x261B: case 0x261C: case 0x261D: case 0x261E: case 0x261F:
        case 0x2765: case 0x2767: case 0x29BE: case 0x29BF: case 0x2660:
        case 0x2661: case 0x2662: case 0x2663: case 0x2664: case 0x2665:
        case 0x2666: case 0x2667: case 0x1F446: case 0x1F447: case 0x1F448:
        case 0x1F449: case 0x1F597: case 0x1F598: case 0x1F599: case 0x1F59A:
        case 0x1F59B: case 0x1F59C: case 0x1F59D: case 0x1F59E: case 0x1F59F:
        case 0x1F5A0: case 0x1F5A1: case 0x1F5A2: case 0x1F5A3: case 0x1FBC1:
        case 0x1FBC2: case 0x1FBC3: case 0xFFFD:
            return true;
        default:
            return false;
    }
}

static void merge_rect(Rect& dst, const Rect& src, bool& has_value) {
    if (!has_value) { dst = src; has_value = true; return; }
    dst.x0 = std::min(dst.x0, src.x0);
    dst.y0 = std::min(dst.y0, src.y0);
    dst.x1 = std::max(dst.x1, src.x1);
    dst.y1 = std::max(dst.y1, src.y1);
}

MuLogicExtractor::MuLogicExtractor() {
    cur_block = nullptr; cur_line = nullptr;
    last_line = nullptr;
    last_char = ' '; last_bidi = 0; 
    pen = {0, 0}; lag_pen = {0, 0}; start = {0, 0};
    new_obj = true; maybe_bullet = false;
    dehyphenate = true; // Khởi tạo biến này
}

void MuLogicExtractor::begin_page(float width, float height) {
    page.mediabox = {0, 0, width, height};
    page.blocks.clear();
    cur_block = nullptr; cur_line = nullptr;
    last_line = nullptr;
    last_char = ' '; last_bidi = 0;
    pen = {0, 0}; lag_pen = {0, 0}; start = {0,0};
    new_obj = true; maybe_bullet = false;
}

void MuLogicExtractor::hint_new_text_obj() {
    new_obj = true;
}

// BƯỚC 1: Xử lý Ligature & Whitespace y hệt MuPDF
void MuLogicExtractor::add_char(int unicode, float x, float y, float adv, float matrix[6], 
                  const std::string& font_name, float size, uint32_t color, 
                  bool bold, bool italic, bool serif, bool mono, int wmode, float ascender, float descender, 
                  int bidi_level, bool has_real_glyph) 
{
    if (unicode == -1) return;

    int bidi = (bidi_level >= 0) ? bidi_level : 0;
    
    // Ánh xạ has_real_glyph sang số nguyên giống MuPDF
    int main_glyph = has_real_glyph ? 1 : -1; 

    add_char_imp(unicode, main_glyph, adv, matrix, font_name, size, color, bold, italic, serif, mono, wmode, bidi, false, ascender, descender, false);
}

// BƯỚC 2: Bộ máy tính toán không gian và gộp Line/Block (TRÁI TIM CỦA MUPDF)
void MuLogicExtractor::add_char_imp(int c, int glyph, float adv, float matrix[6], const std::string& font_name, 
                      float size, uint32_t color, bool bold, bool italic, bool serif, bool mono,
                      int wmode, int bidi, bool force_new_line, float ascender, float descender, bool is_synthetic_space) 
{
    InternalMatrix m = { matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5] };
    bool new_para = false;
    bool new_line = true;
    int add_space = 0;
    Vec2 dir, ndir, p, q, delta;
    float spacing = 0, base_offset = 0;

    bidi = bidi & 1; // Chỉ giữ cờ RTL để dùng bit 2 làm cờ "visual" reordering.

    float m_size = matrix_expansion(m);
    if (m_size <= 0.0f) m_size = size;

    if (wmode == 0) dir = {1.0f, 0.0f}; else dir = {0.0f, -1.0f};
    dir = transform_vec(dir, m);
    ndir = normalize_vec(dir);

    if (wmode == 0) {
        p.x = m.e; p.y = m.f;
        q.x = m.e + adv * dir.x; q.y = m.f + adv * dir.y;
    } else {
        p.x = m.e - adv * dir.x; p.y = m.f - adv * dir.y;
        q.x = m.e; q.y = m.f;
    }

    cur_block = page.blocks.empty() ? nullptr : &page.blocks.back();
    cur_line = cur_block ? (cur_block->lines.empty() ? nullptr : &cur_block->lines.back()) : nullptr;

    // 1. Xử lý ActualText (glyph == -1)
    if (cur_line != nullptr && glyph == -1) {
        WinChar wc;
        wc.c = c; wc.bidi = bidi; wc.origin = pen; wc.size = m_size; 
        wc.color = color; wc.is_bold = bold; wc.is_italic = italic; 
        wc.is_serif = serif; wc.is_mono = mono; wc.font_name = font_name;
        
        wc.quad.ll = {pen.x, pen.y}; wc.quad.ul = {pen.x, pen.y};
        wc.quad.lr = {pen.x, pen.y}; wc.quad.ur = {pen.x, pen.y};
        
        cur_line->chars.push_back(wc);
        
        last_bidi = bidi;
        last_char = c;
        last_line = cur_line;
        return; 
    }

    // 2. Kiểm tra ngắt dòng / ngắt đoạn
    if (cur_line == nullptr || cur_line->wmode != wmode || vec_dot(ndir, cur_line->dir) < 0.999f) {
        new_para = true;
        new_line = true;
    } else {
        // [PHỤC HỒI]: Chặn Fake Bold (PDF in 2 chữ cái đè lên nhau)
        float dist = std::hypot(p.x - lag_pen.x, p.y - lag_pen.y) / m_size;
        if (dist < FAKE_BOLD_MAX_DIST && c == last_char && glyph >= 0) {
            return; // Bỏ qua không in thêm chữ này nữa
        }

        delta.x = p.x - pen.x;
        delta.y = p.y - pen.y;

        spacing = (ndir.x * delta.x + ndir.y * delta.y) / m_size;
        base_offset = (-ndir.y * delta.x + ndir.x * delta.y) / m_size;

        if (std::abs(base_offset) < BASE_MAX_DIST) {
            if ((bidi & 1) != (last_bidi & 1)) {
                new_line = false; 
            } else if (bidi & 1) { 
                Vec2 logical_delta = {p.x - lag_pen.x, p.y - lag_pen.y};
                float logical_spacing = (ndir.x * logical_delta.x + ndir.y * logical_delta.y) / m_size + adv;

                if (std::abs(logical_spacing) < SPACE_DIST) new_line = false;
                else if (std::abs(spacing) < SPACE_DIST) { bidi = 3; new_line = false; } 
                else if (logical_spacing < 0 && logical_spacing > -SPACE_MAX_DIST) {
                    if (wmode == 0 && may_add_space(last_char)) add_space = 1;
                    new_line = false;
                } else if (spacing < 0 && spacing > -SPACE_MAX_DIST) {
                    new_line = false; 
                } else if (spacing > 0 && spacing < SPACE_MAX_DIST) {
                    bidi = 3;
                    if (wmode == 0 && may_add_space(last_char)) add_space = 1 + (spacing > SPACE_DIST*2 ? 1 : 0);
                    new_line = false;
                } else new_line = true;
            } else { 
                if (std::abs(spacing) < SPACE_DIST) new_line = false;
                else if (spacing < 0 && spacing > -SPACE_MAX_DIST) new_line = false;
                else if (spacing > 0 && spacing < SPACE_MAX_DIST) {
                    if (wmode == 0 && may_add_space(last_char)) add_space = 1 + (spacing > SPACE_DIST*2 ? 1 : 0);
                    new_line = false;
                } else new_line = true;
            }
        } else if (std::abs(base_offset) <= PARAGRAPH_DIST) {
            if (wmode == 0 && cur_line && new_obj) {
                if ((p.x - start.x) > 0.5f && !maybe_bullet) new_para = true; 
            }
            new_line = true;
        } else {
            new_para = true;
            new_line = true;
        }
    }

    if (new_para || !cur_block) {
        page.blocks.push_back({BlockType::TEXT, {0, 0, 0, 0}, {}});
        cur_block = &page.blocks.back();
        cur_line = nullptr;
    }

    if (new_line && this->dehyphenate && is_unicode_hyphen(last_char) && last_line != nullptr) {
        last_line->joined = true;
    }

    if (new_line || !cur_line || force_new_line) {
        page.blocks.back().lines.push_back({{0, 0, 0, 0}, ndir, wmode, false, {}});
        cur_line = &page.blocks.back().lines.back();
        start = p;
        if (glyph == -2) maybe_bullet = true;
        else maybe_bullet = plausible_bullet(c);
    }

    // 3. Quy chuẩn lại các non-glyph theo cách của MuPDF
    if (glyph == -2) glyph = -1;

    // Đẩy Space ảo vào dòng nếu phát hiện khoảng cách chữ quá lớn
    if (add_space > 0) {
        // [SỬA LỖI 1:1]: Không gọi đệ quy. Chèn trực tiếp để lấp đầy khoảng trống từ 'pen' đến 'p'
        WinChar space_char;
        space_char.c = ' ';
        space_char.bidi = bidi;
        space_char.origin = pen; // Bắt đầu từ đuôi chữ trước
        space_char.size = m_size;
        space_char.color = color;
        space_char.is_bold = bold;
        space_char.is_italic = italic;
        space_char.is_serif = serif;
        space_char.is_mono = mono;
        space_char.font_name = font_name;
        space_char.is_synthetic = true;
        space_char.ascender = ascender;
        space_char.descender = descender;

        // Tính toán độ dốc (ascender/descender)
        Vec2 sa = {0, ascender};
        Vec2 sd = {0, descender};
        if (wmode == 1) { sa = {1, 0}; sd = {0, 0}; }
        sa = transform_vec(sa, m);
        sd = transform_vec(sd, m);

        // Kéo dài Bounding Box: Bên trái là 'pen' (chữ trước), bên phải là 'p' (chữ sau)
        space_char.quad.ll = {pen.x + sd.x, pen.y + sd.y};
        space_char.quad.ul = {pen.x + sa.x, pen.y + sa.y};
        space_char.quad.lr = {p.x + sd.x, p.y + sd.y};
        space_char.quad.ur = {p.x + sa.x, p.y + sa.y};

        cur_line->chars.push_back(space_char);
    }

    WinChar wc;
    wc.c = c; wc.bidi = bidi; wc.origin = p; wc.size = m_size;
    wc.color = color; wc.is_bold = bold; wc.is_italic = italic;
    wc.is_serif = serif;
    wc.is_mono = mono;
    wc.font_name = font_name;
    wc.is_synthetic = is_synthetic_space;
    wc.ascender = ascender;
    wc.descender = descender;

    Vec2 a = {0, ascender};
    Vec2 d = {0, descender}; 

    if (wmode == 1) { a = {1, 0}; d = {0, 0}; }

    a = transform_vec(a, m);
    d = transform_vec(d, m);

    wc.quad.ll = {p.x + d.x, p.y + d.y};
    wc.quad.ul = {p.x + a.x, p.y + a.y};
    wc.quad.lr = {q.x + d.x, q.y + d.y};
    wc.quad.ur = {q.x + a.x, q.y + a.y};

    cur_line->chars.push_back(wc);

    last_char = c;
    last_bidi = bidi;
    last_line = cur_line;
    lag_pen = p;
    pen = q;
    new_obj = false;
}

// BƯỚC 3: Sắp xếp RTL (Bidi) và tính Bounding Box tổng. Tuyệt đối KHÔNG sort reading order.
WinPage MuLogicExtractor::finish_page() {
    auto reverse_bidi_span = [](std::vector<WinChar>& chars, size_t start, size_t end) {
        std::reverse(chars.begin() + start, chars.begin() + end);
    };

    for (auto& block : page.blocks) {
        bool block_has_bbox = false;

        for (auto& line : block.lines) {
            bool line_has_bbox = false;
            bool needs_reorder = false;

            // Tính BBox và lật RTL nếu cần
            for (size_t i = 0; i < line.chars.size(); ++i) {
                const auto& ch = line.chars[i];
                Rect ch_bbox = {
                    std::min({ch.quad.ll.x, ch.quad.lr.x, ch.quad.ul.x, ch.quad.ur.x}),
                    std::min({ch.quad.ll.y, ch.quad.lr.y, ch.quad.ul.y, ch.quad.ur.y}),
                    std::max({ch.quad.ll.x, ch.quad.lr.x, ch.quad.ul.x, ch.quad.ur.x}),
                    std::max({ch.quad.ll.y, ch.quad.lr.y, ch.quad.ul.y, ch.quad.ur.y})
                };
                merge_rect(line.bbox, ch_bbox, line_has_bbox);

                if (ch.bidi == 3) needs_reorder = true;
            }

            // Logic reverse_bidi_line y hệt MuPDF
            if (needs_reorder && !line.chars.empty()) {
                size_t i = 0;
                while (i < line.chars.size()) {
                    if (line.chars[i].bidi != 0) {
                        size_t j = i + 1;
                        while (j < line.chars.size() && line.chars[j].bidi != 0) j++;
                        reverse_bidi_span(line.chars, i, j);
                        i = j;
                    } else {
                        i++;
                    }
                }
            }

            if (!line_has_bbox) line.bbox = {0, 0, 0, 0};
            merge_rect(block.bbox, line.bbox, block_has_bbox);
        }
        if (!block_has_bbox) block.bbox = {0, 0, 0, 0};
    }

    // Xóa block rỗng
    page.blocks.erase(
        std::remove_if(page.blocks.begin(), page.blocks.end(), 
            [](const WinBlock& b) { return b.lines.empty(); }), 
        page.blocks.end());

    return page;
}

// Chuyển Unicode sang UTF8
static void append_utf8_codepoint(std::string& out, int cp) {
    if (cp == 0x2028 || cp == 0x2029) cp = '\n';
    if (cp <= 0 || cp > 0x10FFFF) return;
    if (cp <= 0x7F) out.push_back((char)cp);
    else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// BƯỚC 4: Lấy text chuẩn chỉnh, xử lý Hyphen (Joined)
std::string MuLogicExtractor::get_text(const WinPage& p) {
    std::string text_out;

    for (size_t b = 0; b < p.blocks.size(); ++b) {
        const auto& block = p.blocks[b];
        for (size_t l = 0; l < block.lines.size(); ++l) {
            const auto& line = block.lines[l];
            
            // In các ký tự trong dòng
            for (size_t i = 0; i < line.chars.size(); ++i) {
                // Nếu đây là ký tự cuối của 1 dòng bị Joined (có dấu gạch nối) thì bỏ qua không in
                if (line.joined && i == line.chars.size() - 1 && is_unicode_hyphen(line.chars[i].c)) {
                    continue; 
                }
                append_utf8_codepoint(text_out, line.chars[i].c);
            }

            // Xử lý xuống dòng: Bỏ qua '\n' nếu dòng bị nối (Joined)
            if (l < block.lines.size() - 1) {
                if (!line.joined) {
                    text_out += "\n";
                }
            }
        }
        if (b < p.blocks.size() - 1) text_out += "\n\n";
    }
    return text_out;
}

} // namespace WinExtract