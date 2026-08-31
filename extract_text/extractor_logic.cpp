#include "extractor_logic.hpp"
#include "pdf_engine.hpp"
#include "ucdn.hpp"
#include "bidi_imp.hpp"
#include <algorithm>
#include <cmath>

namespace WinExtract {

constexpr float PARAGRAPH_DIST = 1.5f;
constexpr float SPACE_DIST = 0.15f;
constexpr float SPACE_MAX_DIST = 0.8f;
constexpr float BASE_MAX_DIST = 0.8f;
constexpr float FAKE_BOLD_MAX_DIST = 0.1f;

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

static bool is_mark_non_spacing(int c) {
    return ucdn_get_general_category(c) == UCDN_GENERAL_CATEGORY_MN;
}

static void merge_rect(Rect& dst, const Rect& src, bool& has_value) {
    if (!has_value) { dst = src; has_value = true; return; }
    dst.x0 = std::min(dst.x0, src.x0);
    dst.y0 = std::min(dst.y0, src.y0);
    dst.x1 = std::max(dst.x1, src.x1);
    dst.y1 = std::max(dst.y1, src.y1);
}

WinTextExtractor::WinTextExtractor() {
    cur_block = nullptr; cur_line = nullptr;
    last_line = nullptr;
    last_char = ' '; last_bidi = 0; 
    pen = {0, 0}; lag_pen = {0, 0}; start = {0, 0};
    new_obj = true; maybe_bullet = false;
    // first no-glyph char (if any) is not spuriously treated as a swallowed dup.
    last_was_fake_bold = true;
}

void WinTextExtractor::begin_page(float width, float height) {
    page.mediabox = {0, 0, width, height};
    page.blocks.clear();
    cur_block = nullptr; cur_line = nullptr;
    last_line = nullptr;
    last_char = ' '; last_bidi = 0;
    pen = {0, 0}; lag_pen = {0, 0}; start = {0,0};
    new_obj = true; maybe_bullet = false;
    last_was_fake_bold = true;
}

void WinTextExtractor::hint_new_text_obj() {
    new_obj = true;
}

static bool is_within_fake_bold_distance(float a, float b, float size) {
    float d = a - b;
    if (d < 0) d = -d;
    return size > 0 ? (d / size) < FAKE_BOLD_MAX_DIST : d < FAKE_BOLD_MAX_DIST;
}

static bool check_for_fake_bold(
    WinBlock* first_block_ptr,
    std::vector<WinBlock>& blocks,
    const std::string &font_name,
    int c,
    Vec2 p,
    float size,
    bool bold,
    bool italic,
    uint32_t color
)
{
    (void)first_block_ptr;
    for (auto& block : blocks) {
        if (block.type != BlockType::TEXT) continue;
        for (auto& line : block.lines) {
            WinChar* prev_char = nullptr;
            for (auto& ch : line.chars) {
                if (ch.c == c &&
                    is_within_fake_bold_distance(ch.origin.x, p.x, size) &&
                    is_within_fake_bold_distance(ch.origin.y, p.y, size) &&
                    ch.font_name == font_name)
                {
                    if (c == ' ') {
                        // Overlaying spaces only counts as boldening if a
                        // neighbouring char is already marked bold.
                        WinChar* next_char = nullptr;
                        // find char after ch within the same line
                        for (size_t k = 0; k < line.chars.size(); ++k) {
                            if (&line.chars[k] == &ch && k + 1 < line.chars.size()) {
                                next_char = &line.chars[k + 1];
                            }
                        }
                        if ((prev_char && prev_char->is_bold) || (next_char && next_char->is_bold)) {
                            ch.is_bold = true;
                        }
                        ch.color = color;
                        return true;
                    } else {
                        ch.is_bold = true;
                        ch.color = color;
                        return true;
                    }
                }
                prev_char = &ch;
            }
        }
    }
    (void)italic;
    (void)bold;
    return false;
}

// Ligature / presentation-form expansion + whitespace normalization, matching
// the pre-processing MuPDF does in fz_add_stext_char() before ever reaching
static bool expand_ligature(int c, int out[3], int& n) {
    switch (c) {
        case 0xFB00: out[0]='f'; out[1]='f'; n=2; return true;
        case 0xFB01: out[0]='f'; out[1]='i'; n=2; return true;
        case 0xFB02: out[0]='f'; out[1]='l'; n=2; return true;
        case 0xFB03: out[0]='f'; out[1]='f'; out[2]='i'; n=3; return true;
        case 0xFB04: out[0]='f'; out[1]='f'; out[2]='l'; n=3; return true;
        case 0xFB05: /* long st */
        case 0xFB06: out[0]='s'; out[1]='t'; n=2; return true;
        default: return false;
    }
}

static int normalize_whitespace(int c) {
    switch (c) {
        case 0x0009: /* tab */
        case 0x0020: /* space */
        case 0x00A0: /* no-break space */
        case 0x1680: /* ogham space mark */
        case 0x180E: /* mongolian vowel separator */
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
        case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
        case 0x200A: /* en/em/etc spaces */
        case 0x202F: /* narrow no-break space */
        case 0x205F: /* medium mathematical space */
        case 0x3000: /* ideographic space */
            return ' ';
        default:
            return c;
    }
}

void WinTextExtractor::add_char(int unicode, float x, float y, float adv, float matrix[6], 
                  const std::string& font_name, float size, uint32_t color, 
                  bool bold, bool italic, bool serif, bool mono, int wmode, float ascender, float descender, 
                  int bidi_level, bool has_real_glyph) 
{
    if (unicode == -1) return;
    if (unicode == '\r' || unicode == '\n') return;
    int bidi = (bidi_level >= 0) ? bidi_level : 0;
    int main_glyph = has_real_glyph ? 1 : -1;
    int c = unicode;
    if (!preserve_ligatures) {
        int lig[3];
        int n = 0;
        if (expand_ligature(c, lig, n)) {
            add_char_imp(lig[0], main_glyph, adv, matrix, font_name, size, color, bold, italic, serif, mono, wmode, bidi, false, ascender, descender, false);
            for (int i = 1; i < n; ++i)
                add_char_imp(lig[i], -1, 0, matrix, font_name, size, color, bold, italic, serif, mono, wmode, bidi, false, ascender, descender, false);
            return;
        }
        // Alphabetic and Arabic presentation forms -> compatibility decompose.
        if ((c >= 0xFB00 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFC)) {
            uint32_t decomp[18];
            int n2 = ucdn_compat_decompose((uint32_t)c, decomp);
            if (n2 > 0) {
                add_char_imp((int)decomp[0], main_glyph, adv, matrix, font_name, size, color, bold, italic, serif, mono, wmode, bidi, false, ascender, descender, false);
                for (int i = 1; i < n2; ++i)
                    add_char_imp((int)decomp[i], -1, 0, matrix, font_name, size, color, bold, italic, serif, mono, wmode, bidi, false, ascender, descender, false);
                return;
            }
        }
    }

    if (!preserve_whitespace) {
        c = normalize_whitespace(c);
    }

    add_char_imp(c, main_glyph, adv, matrix, font_name, size, color, bold, italic, serif, mono, wmode, bidi, false, ascender, descender, false);
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../include/stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../include/stb/stb_image_write.h"
#include "../include/stb/base64.hpp"

#include "miniz.h"
#include "lcms2.h"
#include "icc_profiles.h"

static void write_png_data_to_vector(void *context, void *data, int size) {
    auto* vec = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

void WinTextExtractor::add_image(const Rect& bbox, const WinImageXObject& img) {
    if (!img.stream_ptr || img.stream_ptr->empty()) return;
    
    int w = img.width;
    int h = img.height;
    std::string base64_data = "";
    std::string ext = "png";
    
    std::vector<uint8_t> png_data;
    bool success = false;
    
    if (img.filter == "DCTDecode") {
        // Decode JPEG natively using stb_image
        int req_comp = 3; 
        // For CMYK JPEG, stb_image might load it as 4 channels, or we might need to handle it.
        // stb_image actually converts CMYK JPEGs to RGB automatically in some cases, but often it just loads as 4 channels.
        int x, y, comp;
        stbi_uc* pixels = stbi_load_from_memory(img.stream_ptr->data(), img.stream_ptr->size(), &x, &y, &comp, 0);
        if (pixels) {
            w = x; h = y;
            if (comp == 4 && img.color_space.find("CMYK") != std::string::npos) {
                bool inverted_cmyk = false;
                if (img.decode.size() >= 8) {
                    if (img.decode[0] > img.decode[1]) {
                        inverted_cmyk = true;
                    }
                } else if (img.filter == "DCTDecode" || img.filter == "DCT") {
                    // Adobe CMYK JPEGs in PDF are typically inverted (0 = 100% ink)
                    inverted_cmyk = true;
                }

#if WINNERZ_USE_LCMS2
                // LCMS2 expects standard CMYK (255 = 100% ink)
                if (inverted_cmyk) {
                    for (size_t i = 0; i < (size_t)(w * h * 4); i++) {
                        pixels[i] = 255 - pixels[i];
                    }
                }

                // We have 4 channels (CMYK). We should use LCMS2 to convert to RGB.
                cmsHPROFILE inProfile = cmsCreate_sRGBProfile(); // fallback
                if (img.color_space.find("CMYK") != std::string::npos) {
                    inProfile = cmsOpenProfileFromMem(Coated_Fogra39L_VIGC_300_icc, sizeof(Coated_Fogra39L_VIGC_300_icc));
                }
                cmsHPROFILE outProfile = cmsCreate_sRGBProfile();
                cmsHTRANSFORM transform = cmsCreateTransform(inProfile, TYPE_CMYK_8, outProfile, TYPE_RGB_8, INTENT_PERCEPTUAL, 0);
                
                std::vector<uint8_t> rgb_pixels(w * h * 3);
                cmsDoTransform(transform, pixels, rgb_pixels.data(), w * h);
                
                cmsDeleteTransform(transform);
                cmsCloseProfile(inProfile);
                cmsCloseProfile(outProfile);
                
                stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, 3, rgb_pixels.data(), w * 3);
                success = true;
#else
                std::vector<uint8_t> rgb_pixels(w * h * 3);
                for (size_t i = 0; i < (size_t)(w * h); i++) {
                    float c = pixels[i * 4 + 0] / 255.0f;
                    float m = pixels[i * 4 + 1] / 255.0f;
                    float y = pixels[i * 4 + 2] / 255.0f;
                    float k = pixels[i * 4 + 3] / 255.0f;
                    
                    if (inverted_cmyk) {
                        rgb_pixels[i * 3 + 0] = (uint8_t)(255.0f * c * k);
                        rgb_pixels[i * 3 + 1] = (uint8_t)(255.0f * m * k);
                        rgb_pixels[i * 3 + 2] = (uint8_t)(255.0f * y * k);
                    } else {
                        rgb_pixels[i * 3 + 0] = (uint8_t)(255.0f * (1.0f - c) * (1.0f - k));
                        rgb_pixels[i * 3 + 1] = (uint8_t)(255.0f * (1.0f - m) * (1.0f - k));
                        rgb_pixels[i * 3 + 2] = (uint8_t)(255.0f * (1.0f - y) * (1.0f - k));
                    }
                }
                stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, 3, rgb_pixels.data(), w * 3);
                success = true;
#endif
            } else {
                stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, comp, pixels, w * comp);
                success = true;
            }
            stbi_image_free(pixels);
        }
    } else if (img.filter == "FlateDecode" || img.filter.empty()) {
        std::vector<uint8_t> raw_pixels;
        bool has_pixels = false;
        
        // Bỏ mz_uncompress vì pdf_engine_helpers.inc.cpp đã giải nén sẵn
        raw_pixels = *img.stream_ptr;
        has_pixels = true;

        if (has_pixels && img.bits_per_component == 8) {
            int comps = raw_pixels.size() / (w * h);
            if (!img.indexed_palette.empty() && comps == 1) {
                int out_comps = (img.smask_ptr && img.smask_ptr->size() >= (size_t)(w * h)) ? 4 : 3;
                std::vector<uint8_t> out_pixels(w * h * out_comps);
                const uint8_t* smask = (out_comps == 4) ? img.smask_ptr->data() : nullptr;
                for (size_t i = 0; i < (size_t)(w * h); i++) {
                    uint8_t idx = raw_pixels[i];
                    if (idx * 3 + 2 < img.indexed_palette.size()) {
                        out_pixels[i * out_comps + 0] = img.indexed_palette[idx * 3 + 0];
                        out_pixels[i * out_comps + 1] = img.indexed_palette[idx * 3 + 1];
                        out_pixels[i * out_comps + 2] = img.indexed_palette[idx * 3 + 2];
                    } else {
                        out_pixels[i * out_comps + 0] = 0;
                        out_pixels[i * out_comps + 1] = 0;
                        out_pixels[i * out_comps + 2] = 0;
                    }
                    if (out_comps == 4) {
                        out_pixels[i * out_comps + 3] = smask[i];
                    }
                }
                stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, out_comps, out_pixels.data(), w * out_comps);
                success = true;
            } else if (comps == 1 || comps == 3) {
                if (img.smask_ptr && img.smask_ptr->size() >= (size_t)(w * h)) {
                    int out_comps = comps + 1;
                    std::vector<uint8_t> out_pixels(w * h * out_comps);
                    const uint8_t* smask = img.smask_ptr->data();
                    for (size_t i = 0; i < (size_t)(w * h); i++) {
                        for (int c = 0; c < comps; c++) {
                            out_pixels[i * out_comps + c] = raw_pixels[i * comps + c];
                        }
                        out_pixels[i * out_comps + comps] = smask[i];
                    }
                    stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, out_comps, out_pixels.data(), w * out_comps);
                    success = true;
                } else {
                    stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, comps, raw_pixels.data(), w * comps);
                    success = true;
                }
            } else if (comps == 4) {
                bool inverted_cmyk = false;
                if (img.decode.size() >= 8) {
                    if (img.decode[0] > img.decode[1]) {
                        inverted_cmyk = true;
                    }
                }
#if WINNERZ_USE_LCMS2
                // LCMS2 expects standard CMYK (255 = 100% ink)
                if (inverted_cmyk) {
                    for (size_t i = 0; i < (size_t)(w * h * 4); i++) {
                        raw_pixels[i] = 255 - raw_pixels[i];
                    }
                }

                // Assuming CMYK FlateDecode or Raw CMYK, we MUST convert to RGB so PPTX doesn't display corrupt colors.
                cmsHPROFILE inProfile = cmsCreate_sRGBProfile(); // fallback
                if (img.color_space.find("CMYK") != std::string::npos) {
                    inProfile = cmsOpenProfileFromMem(Coated_Fogra39L_VIGC_300_icc, sizeof(Coated_Fogra39L_VIGC_300_icc));
                }
                cmsHPROFILE outProfile = cmsCreate_sRGBProfile();
                cmsHTRANSFORM transform = cmsCreateTransform(inProfile, TYPE_CMYK_8, outProfile, TYPE_RGB_8, INTENT_PERCEPTUAL, 0);
                
                std::vector<uint8_t> rgb_pixels(w * h * 3);
                cmsDoTransform(transform, raw_pixels.data(), rgb_pixels.data(), w * h);
                
                cmsDeleteTransform(transform);
                cmsCloseProfile(inProfile);
                cmsCloseProfile(outProfile);
                
                stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, 3, rgb_pixels.data(), w * 3);
                success = true;
#else
                std::vector<uint8_t> rgb_pixels(w * h * 3);
                for (size_t i = 0; i < (size_t)(w * h); i++) {
                    float c = raw_pixels[i * 4 + 0] / 255.0f;
                    float m = raw_pixels[i * 4 + 1] / 255.0f;
                    float y = raw_pixels[i * 4 + 2] / 255.0f;
                    float k = raw_pixels[i * 4 + 3] / 255.0f;
                    
                    if (inverted_cmyk) {
                        rgb_pixels[i * 3 + 0] = (uint8_t)(255.0f * c * k);
                        rgb_pixels[i * 3 + 1] = (uint8_t)(255.0f * m * k);
                        rgb_pixels[i * 3 + 2] = (uint8_t)(255.0f * y * k);
                    } else {
                        rgb_pixels[i * 3 + 0] = (uint8_t)(255.0f * (1.0f - c) * (1.0f - k));
                        rgb_pixels[i * 3 + 1] = (uint8_t)(255.0f * (1.0f - m) * (1.0f - k));
                        rgb_pixels[i * 3 + 2] = (uint8_t)(255.0f * (1.0f - y) * (1.0f - k));
                    }
                }
                stbi_write_png_to_func(write_png_data_to_vector, &png_data, w, h, 3, rgb_pixels.data(), w * 3);
                success = true;
#endif
            }
        }
    }
    
    if (success && !png_data.empty()) {
        base64_data = "data:image/png;base64," + Winnerz::base64_encode(png_data.data(), png_data.size());
    }

    if (page.blocks.empty() || page.blocks.back().type != BlockType::IMAGE) {
        page.blocks.push_back({BlockType::IMAGE, bbox, {}, base64_data, ext, w, h, img.color_space, img.decode});
    } else {
        page.blocks.push_back({BlockType::IMAGE, bbox, {}, base64_data, ext, w, h, img.color_space, img.decode});
    }
    cur_block = nullptr;
    cur_line = nullptr;
}

void WinTextExtractor::add_char_imp(int c, int glyph, float adv, float matrix[6], const std::string& font_name,
                                    float size, uint32_t color, bool bold, bool italic, bool serif, bool mono,
                                    int wmode, int bidi, bool force_new_line, float ascender, float descender, bool is_synthetic_space) 
{
    InternalMatrix m = { matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5] };
    bool new_para = false;
    bool new_line = true;
    int add_space = 0;
    Vec2 dir, ndir, p, q, delta;
    float spacing = 0, base_offset = 0;
    
    bidi = bidi & 1; 
    
    float m_size = matrix_expansion(m);
    if (m_size <= 0.001f) m_size = size;

    if (m_size <= 0.001f) m_size = 1.0f;
    
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
    
    auto expand_bbox = [](auto& target_bbox, const auto& quad) {
        float min_x = std::min({quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x});
        float max_x = std::max({quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x});
        float min_y = std::min({quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y});
        float max_y = std::max({quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y});
        
        if (target_bbox.x0 == target_bbox.x1 && target_bbox.y0 == target_bbox.y1) {
            target_bbox.x0 = min_x; target_bbox.y0 = min_y; 
            target_bbox.x1 = max_x; target_bbox.y1 = max_y;
        } else {
            target_bbox.x0 = std::min(target_bbox.x0, min_x);
            target_bbox.y0 = std::min(target_bbox.y0, min_y);
            target_bbox.x1 = std::max(target_bbox.x1, max_x);
            target_bbox.y1 = std::max(target_bbox.y1, max_y);
        }
    };

    // Helper to build a Quad for a char/space, shared by all three insertion sites.
    // When accurate_bboxes is set, this is the point to plug in real glyph outline
    // bboxes instead of the ascender/descender approximation (not available in this
    auto make_quad = [&](Vec2 from, Vec2 to, float asc, float desc, int wm) -> Quad {
        Vec2 a = {0, asc};
        Vec2 d = {0, desc};
        if (wm == 1) { a = {1, 0}; d = {0, 0}; }
        a = transform_vec(a, m);
        d = transform_vec(d, m);

        Quad q_out;
        q_out.ll = {from.x + d.x, from.y + d.y};
        q_out.ul = {from.x + a.x, from.y + a.y};
        q_out.lr = {to.x + d.x,   to.y + d.y};
        q_out.ur = {to.x + a.x,   to.y + a.y};

        if (accurate_bboxes) {
            // Reserved hook: override q_out with a real glyph outline bbox when
            // the font engine can supply one.
        }
        return q_out;
    };
    
    cur_block = page.blocks.empty() ? nullptr : &page.blocks.back();
    if (cur_block && cur_block->type != BlockType::TEXT) {
        cur_block = nullptr;
    }
    cur_line = cur_block ? (cur_block->lines.empty() ? nullptr : &cur_block->lines.back()) : nullptr;
    
    if (collect_styles) {
        if (glyph < 0) {
            if (last_was_fake_bold) {
                last_was_fake_bold = false;
                return;
            }
        } else if (check_for_fake_bold(page.blocks.empty() ? nullptr : &page.blocks.front(), page.blocks, font_name, c, p, m_size, bold, italic, color)) {
            last_was_fake_bold = true;
            return;
        } else {
            last_was_fake_bold = false;
        }
    }
    
    bool is_mark = is_mark_non_spacing(c);
    
    if (cur_line != nullptr && (glyph == -1 || is_mark)) {
        WinChar wc;
        wc.c = c; wc.bidi = bidi; wc.origin = pen; wc.size = m_size; 
        wc.color = color; wc.is_bold = bold; wc.is_italic = italic; 
        wc.is_serif = serif; wc.is_mono = mono; wc.font_name = font_name;
        wc.ascender = ascender;
        wc.descender = descender;
        
        wc.quad = make_quad(pen, pen, ascender, descender, wmode);
        
        cur_line->chars.push_back(wc);
        
        expand_bbox(cur_line->bbox, wc.quad);
        if (cur_block) expand_bbox(cur_block->bbox, wc.quad);
        
        last_bidi = bidi;
        last_char = c;
        last_line = cur_line;
        return; 
    }
    
    if (cur_line == nullptr || cur_line->wmode != wmode || vec_dot(ndir, cur_line->dir) < 0.999f) {
        new_para = true;
        new_line = true;
    } else {
        float dist = std::hypot(p.x - lag_pen.x, p.y - lag_pen.y) / m_size;
        if (dist < FAKE_BOLD_MAX_DIST && c == last_char && glyph >= 0) {
            return;
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
            // restore old logic
            if (wmode == 0 && cur_line && new_obj) {
                if ((p.x - start.x) > 0.5f && !maybe_bullet) new_para = true; 
            }
            new_line = true;
        } else {
            new_para = true;
            new_line = true;
        }
    }
    
    // Lazy-vector flushing would go here if BlockType::IMAGE/VECTOR interleaving
    
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
    
    if (glyph == -2) glyph = -1;
    
    if (c != ' ' && add_space > 0 && !inhibit_spaces) {
        WinChar space_char;
        space_char.c = ' ';
        space_char.bidi = bidi;
        space_char.origin = pen; 
        space_char.size = m_size;
        space_char.color = color;
        space_char.is_bold = bold;
        space_char.is_italic = italic;
        space_char.is_serif = serif;
        space_char.is_mono = mono;
        space_char.font_name = font_name;
        space_char.is_synthetic = true;
        space_char.is_synthetic_large = (add_space > 1);
        space_char.ascender = ascender;
        space_char.descender = descender;
        
        space_char.quad = make_quad(pen, p, ascender, descender, wmode);
        
        cur_line->chars.push_back(space_char);
        
        expand_bbox(cur_line->bbox, space_char.quad);
        if (cur_block) expand_bbox(cur_block->bbox, space_char.quad);
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
    
    wc.quad = make_quad(p, q, ascender, descender, wmode);
    
    cur_line->chars.push_back(wc);
    
    expand_bbox(cur_line->bbox, wc.quad);
    if (cur_block) expand_bbox(cur_block->bbox, wc.quad);
    
    last_char = c;
    last_bidi = bidi;
    last_line = cur_line;
    lag_pen = p;
    pen = q;
    new_obj = false;
}

struct BidiState {
    const uint32_t* text_start;
    std::vector<WinChar>* chars;
};

static void wz_bidi_cb(const uint32_t *fragment, size_t fragmentLen, int bidiLevel, int script, void *arg) {
    BidiState* state = static_cast<BidiState*>(arg);
    size_t offset = fragment - state->text_start;
    for (size_t i = 0; i < fragmentLen; ++i) {
        (*state->chars)[offset + i].bidi = bidiLevel;
    }
}

WinPage WinTextExtractor::finish_page() {
    auto reverse_bidi_span = [](std::vector<WinChar>& chars, size_t start, size_t end) {
        std::reverse(chars.begin() + start, chars.begin() + end);
    };

    // ends in a hyphen, mark it joined so get_text() drops the trailing hyphen.
    // Previously this only happened when a *new* line started (mid-page), so a
    // hyphen on the page's final line was never caught.
    if (this->dehyphenate && is_unicode_hyphen(last_char) && last_line != nullptr) {
        last_line->joined = true;
    }

    for (auto& block : page.blocks) {
        if (block.type == BlockType::IMAGE) continue;
        
        bool block_has_bbox = false;

        for (auto& line : block.lines) {
            bool line_has_bbox = false;
            bool needs_reorder = false;
            int rtl_count = 0;

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
                
                int bc = ucdn_get_bidi_class(ch.c);
                if (bc == UCDN_BIDI_CLASS_R || bc == UCDN_BIDI_CLASS_AL || bc == UCDN_BIDI_CLASS_RLE || bc == UCDN_BIDI_CLASS_RLO) {
                    rtl_count++;
                }
            }

            if (rtl_count > 0 && !line.chars.empty()) {
                std::stable_sort(line.chars.begin(), line.chars.end(), [](const WinChar& a, const WinChar& b) {
                    return a.quad.ll.x < b.quad.ll.x;
                });

                std::vector<uint32_t> text(line.chars.size());
                for (size_t i = 0; i < line.chars.size(); ++i) {
                    text[i] = line.chars[i].c;
                }
                
                BidiState state = { text.data(), &line.chars };
                wz_bidi_direction baseDir = WZ_BIDI_UNSET;
                wz_bidi_fragment_text(nullptr, text.data(), text.size(), &baseDir, wz_bidi_cb, &state, 0);

                int max_level = 0;
                for (size_t i = 0; i < line.chars.size(); ++i) {
                    if (line.chars[i].bidi > max_level) max_level = line.chars[i].bidi;
                }

                while (max_level > 0) {
                    size_t i = 0;
                    while (i < line.chars.size()) {
                        if (line.chars[i].bidi >= max_level) {
                            size_t j = i + 1;
                            while (j < line.chars.size() && line.chars[j].bidi >= max_level) j++;
                            reverse_bidi_span(line.chars, i, j);
                            i = j;
                        } else {
                            i++;
                        }
                    }
                    max_level--;
                }
            } else if (needs_reorder && !line.chars.empty()) {
                size_t i = 0;
                while (i < line.chars.size()) {
                    if (line.chars[i].bidi == 3) {
                        size_t j = i + 1;
                        while (j < line.chars.size() && line.chars[j].bidi == 3) j++;
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

    page.blocks.erase(
        std::remove_if(page.blocks.begin(), page.blocks.end(), 
            [](const WinBlock& b) { return b.type == BlockType::TEXT && b.lines.empty(); }), 
        page.blocks.end());

    return page;
}

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

std::string WinTextExtractor::get_text(const WinPage& p) {
    std::string text_out;

    for (size_t b = 0; b < p.blocks.size(); ++b) {
        const auto& block = p.blocks[b];
        for (size_t l = 0; l < block.lines.size(); ++l) {
            const auto& line = block.lines[l];
            
            for (size_t i = 0; i < line.chars.size(); ++i) {
                if (line.joined && i == line.chars.size() - 1 && is_unicode_hyphen(line.chars[i].c)) {
                    continue; 
                }
                append_utf8_codepoint(text_out, line.chars[i].c);
            }

            if (l < block.lines.size() - 1) {
                if (!line.joined) {
                    text_out += "\n";
                }
            }
        }
        if (b < p.blocks.size() - 1) text_out += "\n";
    }
    return text_out;
}

} // namespace WinExtract