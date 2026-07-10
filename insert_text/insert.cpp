#include "insert.hpp"
#include "pdf_engine.hpp"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <array>

#ifdef WINEXTRACT_USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <hb-subset.h>

static std::mutex global_shaping_mutex;

#include "miniz.h"
#include <zlib.h>
#include <set>
#include <unordered_set>

#include "../extract_text/ucdn.hpp"
#include "../extract_text/bidi_imp.hpp"

namespace Winnerz {

static std::vector<uint32_t> Utf8ToUtf32(const std::string& utf8) {
    std::vector<uint32_t> utf32;
    size_t i = 0;
    while (i < utf8.length()) {
        uint32_t cp = 0;
        unsigned char c = utf8[i];
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= utf8.length()) break;
            cp = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= utf8.length()) break;
            cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= utf8.length()) break;
            cp = ((c & 0x07) << 18) | ((utf8[i+1] & 0x3F) << 12) | ((utf8[i+2] & 0x3F) << 6) | (utf8[i+3] & 0x3F);
            i += 4;
        } else {
            i += 1;
        }
        utf32.push_back(cp);
    }
    return utf32;
}

struct ShapedGlyph {
    uint32_t id;
    float x_advance;
    float y_advance;
    float x_offset;
    float y_offset;
};

struct ShapedLine {
    std::string text;
    std::vector<ShapedGlyph> glyphs;
    float width;
};

struct LogicalGlyph {
    uint32_t glyphid;
    float x_advance;
    float y_advance;
    float x_offset;
    float y_offset;
    int bidi_level;
    uint32_t cluster;
};

struct BidiStateInsert {
    std::vector<uint32_t>* utf32;
    std::vector<LogicalGlyph>* logical_glyphs;
    hb_font_t* hb_font;
    float scale;
};

static void fz_bidi_insert_cb(const uint32_t *fragment, size_t fragmentLen, int bidiLevel, int script, void *arg) {
    BidiStateInsert* state = static_cast<BidiStateInsert*>(arg);
    size_t start = fragment - state->utf32->data();
    
    hb_buffer_t* hb_buffer = hb_buffer_create();
    hb_buffer_add_utf32(hb_buffer, state->utf32->data(), state->utf32->size(), start, fragmentLen);
    hb_buffer_guess_segment_properties(hb_buffer);
    hb_buffer_set_direction(hb_buffer, (bidiLevel & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    
    hb_shape(state->hb_font, hb_buffer, NULL, 0);
    
    unsigned int glyph_count;
    hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(hb_buffer, &glyph_count);
    hb_glyph_position_t* glyph_pos = hb_buffer_get_glyph_positions(hb_buffer, &glyph_count);
    
    std::vector<LogicalGlyph> fragment_glyphs;
    for (unsigned int i = 0; i < glyph_count; ++i) {
        LogicalGlyph g;
        g.glyphid = glyph_info[i].codepoint;
        g.x_advance = glyph_pos[i].x_advance * state->scale;
        g.y_advance = glyph_pos[i].y_advance * state->scale;
        g.x_offset = glyph_pos[i].x_offset * state->scale;
        g.y_offset = glyph_pos[i].y_offset * state->scale;
        g.bidi_level = bidiLevel;
        g.cluster = glyph_info[i].cluster;
        fragment_glyphs.push_back(g);
    }
    
    if (bidiLevel & 1) {
        std::reverse(fragment_glyphs.begin(), fragment_glyphs.end());
    }
    
    state->logical_glyphs->insert(state->logical_glyphs->end(), fragment_glyphs.begin(), fragment_glyphs.end());
    hb_buffer_destroy(hb_buffer);
}

static std::vector<ShapedLine> HarfbuzzWordWrap(const std::string& text, hb_font_t* hb_font, float font_size, float max_width) {
    std::vector<ShapedLine> lines;
    if (text.empty()) return lines;
    
    hb_face_t* hb_face = hb_font_get_face(hb_font);
    float upem = hb_face_get_upem(hb_face);
    if (upem == 0.0f) upem = 1000.0f;
    float scale = font_size / (upem * 64.0f);

    bool has_rtl = false;
    std::vector<uint32_t> utf32 = Utf8ToUtf32(text);
    for (uint32_t c : utf32) {
        int bc = ucdn_get_bidi_class(c);
        if (bc == UCDN_BIDI_CLASS_R || bc == UCDN_BIDI_CLASS_AL || bc == UCDN_BIDI_CLASS_RLE || bc == UCDN_BIDI_CLASS_RLO) {
            has_rtl = true;
            break;
        }
    }

    std::vector<LogicalGlyph> logical_glyphs;

    if (!has_rtl) {
        hb_buffer_t* hb_buffer = hb_buffer_create();
        hb_buffer_add_utf8(hb_buffer, text.c_str(), -1, 0, -1);
        hb_buffer_guess_segment_properties(hb_buffer);
        hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR);
        hb_shape(hb_font, hb_buffer, NULL, 0);
        
        unsigned int glyph_count;
        hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(hb_buffer, &glyph_count);
        hb_glyph_position_t* glyph_pos = hb_buffer_get_glyph_positions(hb_buffer, &glyph_count);
        
        for (unsigned int i = 0; i < glyph_count; ++i) {
            LogicalGlyph g;
            g.glyphid = glyph_info[i].codepoint;
            g.x_advance = glyph_pos[i].x_advance * scale;
            g.y_advance = glyph_pos[i].y_advance * scale;
            g.x_offset = glyph_pos[i].x_offset * scale;
            g.y_offset = glyph_pos[i].y_offset * scale;
            g.bidi_level = 0;
            g.cluster = glyph_info[i].cluster;
            logical_glyphs.push_back(g);
        }
        hb_buffer_destroy(hb_buffer);
    } else {
        BidiStateInsert state;
        state.utf32 = &utf32;
        state.logical_glyphs = &logical_glyphs;
        state.hb_font = hb_font;
        state.scale = scale;
        
        fz_bidi_direction baseDir = FZ_BIDI_UNSET;
        fz_bidi_fragment_text(nullptr, utf32.data(), utf32.size(), &baseDir, fz_bidi_insert_cb, &state, 0);
    }

    auto wrap_and_reorder = [&](const std::vector<LogicalGlyph>& line_logical_glyphs, float width) {
        std::vector<LogicalGlyph> reordered = line_logical_glyphs;
        int max_level = 0;
        for (const auto& g : reordered) if (g.bidi_level > max_level) max_level = g.bidi_level;
        
        while (max_level > 0) {
            size_t i = 0;
            while (i < reordered.size()) {
                if (reordered[i].bidi_level >= max_level) {
                    size_t j = i + 1;
                    while (j < reordered.size() && reordered[j].bidi_level >= max_level) j++;
                    std::reverse(reordered.begin() + i, reordered.begin() + j);
                    i = j;
                } else i++;
            }
            max_level--;
        }
        
        ShapedLine sl;
        sl.width = width;
        for (const auto& g : reordered) {
            sl.glyphs.push_back({g.glyphid, g.x_advance, g.y_advance, g.x_offset, g.y_offset});
        }
        return sl;
    };

    float current_width = 0;
    std::vector<LogicalGlyph> current_line_glyphs;
    int last_space_idx = -1;
    float width_at_last_space = 0;
    
    for (size_t i = 0; i < logical_glyphs.size(); ++i) {
        auto& g = logical_glyphs[i];
        uint32_t cluster = g.cluster;
        bool is_space = false;
        bool is_newline = false;
        
        if (has_rtl) {
            is_space = (cluster < utf32.size() && utf32[cluster] == ' ');
            is_newline = (cluster < utf32.size() && utf32[cluster] == '\n');
        } else {
            is_space = (cluster < text.size() && text[cluster] == ' ');
            is_newline = (cluster < text.size() && text[cluster] == '\n');
        }

        if (is_newline) {
            lines.push_back(wrap_and_reorder(current_line_glyphs, current_width));
            current_line_glyphs.clear();
            current_width = 0;
            last_space_idx = -1;
            width_at_last_space = 0;
            continue;
        }

        if (is_space) {
            last_space_idx = static_cast<int>(current_line_glyphs.size());
            width_at_last_space = current_width;
        }
        
        if (current_width + g.x_advance > max_width && !current_line_glyphs.empty()) {
            if (last_space_idx != -1) {
                std::vector<LogicalGlyph> next_line_glyphs;
                float next_width = 0;
                for (size_t j = last_space_idx + 1; j < current_line_glyphs.size(); ++j) {
                    next_line_glyphs.push_back(current_line_glyphs[j]);
                    next_width += current_line_glyphs[j].x_advance;
                }
                
                current_line_glyphs.resize(last_space_idx + 1);
                float line_width = width_at_last_space + current_line_glyphs.back().x_advance;
                lines.push_back(wrap_and_reorder(current_line_glyphs, line_width));
                
                current_line_glyphs = std::move(next_line_glyphs);
                current_width = next_width;
                last_space_idx = -1;
                width_at_last_space = 0;
            } else {
                lines.push_back(wrap_and_reorder(current_line_glyphs, current_width));
                current_line_glyphs.clear();
                current_width = 0;
                last_space_idx = -1;
                width_at_last_space = 0;
            }
        }
        
        current_line_glyphs.push_back(g);
        current_width += g.x_advance;
    }
    
    if (!current_line_glyphs.empty()) {
        lines.push_back(wrap_and_reorder(current_line_glyphs, current_width));
    }
    
    return lines;
}

// Zlib compress with Z_DEFAULT_COMPRESSION for much better speed
static std::vector<uint8_t> compress_zlib(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    z_stream defstream;
    defstream.zalloc = Z_NULL;
    defstream.zfree = Z_NULL;
    defstream.opaque = Z_NULL;
    defstream.avail_in = data.size();
    defstream.next_in = const_cast<Bytef*>(data.data());
    
    std::vector<uint8_t> dest(compressBound(data.size()));
    defstream.avail_out = dest.size();
    defstream.next_out = dest.data();
    
    if (deflateInit(&defstream, Z_DEFAULT_COMPRESSION) != Z_OK) return data;
    deflate(&defstream, Z_FINISH);
    dest.resize(dest.size() - defstream.avail_out);
    deflateEnd(&defstream);
    return dest;
}

struct fz_matrix {
    float a, b, c, d, e, f;
};

static fz_matrix fz_scale(float sx, float sy) {
    return {sx, 0, 0, sy, 0, 0};
}

static fz_matrix fz_translate(float tx, float ty) {
    return {1, 0, 0, 1, tx, ty};
}

static fz_matrix fz_pre_rotate(fz_matrix m, float degrees) {
    float angle = degrees * 3.14159265358979323846f / 180.0f;
    float s = std::sin(angle);
    float c = std::cos(angle);
    fz_matrix r = {c, s, -s, c, 0, 0};
    return {
        r.a * m.a + r.b * m.c,
        r.a * m.b + r.b * m.d,
        r.c * m.a + r.d * m.c,
        r.c * m.b + r.d * m.d,
        m.e, m.f
    };
}

static fz_matrix fz_concat(fz_matrix left, fz_matrix right) {
    return {
        left.a * right.a + left.b * right.c,
        left.a * right.b + left.b * right.d,
        left.c * right.a + left.d * right.c,
        left.c * right.b + left.d * right.d,
        left.e * right.a + left.f * right.c + right.e,
        left.e * right.b + left.f * right.d + right.f
    };
}

static fz_matrix fz_invert_matrix(fz_matrix m) {
    float det = m.a * m.d - m.b * m.c;
    if (det == 0.0f) return {1, 0, 0, 1, 0, 0};
    float rdet = 1.0f / det;
    return {
        m.d * rdet,
        -m.b * rdet,
        -m.c * rdet,
        m.a * rdet,
        (m.c * m.f - m.d * m.e) * rdet,
        (m.b * m.e - m.a * m.f) * rdet
    };
}

static std::array<float, 4> fz_transform_rect(const std::array<float, 4>& r, fz_matrix m) {
    float x0 = r[0] * m.a + r[1] * m.c + m.e;
    float y0 = r[0] * m.b + r[1] * m.d + m.f;
    float x1 = r[2] * m.a + r[1] * m.c + m.e;
    float y1 = r[2] * m.b + r[1] * m.d + m.f;
    float x2 = r[0] * m.a + r[3] * m.c + m.e;
    float y2 = r[0] * m.b + r[3] * m.d + m.f;
    float x3 = r[2] * m.a + r[3] * m.c + m.e;
    float y3 = r[2] * m.b + r[3] * m.d + m.f;

    return {
        std::min({x0, x1, x2, x3}),
        std::min({y0, y1, y2, y3}),
        std::max({x0, x1, x2, x3}),
        std::max({y0, y1, y2, y3})
    };
}

static fz_matrix ComputePageCTM(const WinExtract::WinPageGeometry& geo) {
    float userunit = 1.0f;
    
    std::array<float, 4> mediabox = {geo.mediabox.x0, geo.mediabox.y0, geo.mediabox.x1, geo.mediabox.y1};
    std::array<float, 4> cropbox = {geo.cropbox.x0, geo.cropbox.y0, geo.cropbox.x1, geo.cropbox.y1};

    int rotate = geo.rotate;
    if (rotate < 0) rotate = 360 - ((-rotate) % 360);
    if (rotate >= 360) rotate = rotate % 360;
    rotate = 90 * ((rotate + 45) / 90);
    if (rotate >= 360) rotate = 0;

    fz_matrix page_ctm = fz_scale(userunit, -userunit);
    page_ctm = fz_pre_rotate(page_ctm, -static_cast<float>(rotate));

    std::array<float, 4> trans_cropbox = fz_transform_rect(cropbox, page_ctm);
    page_ctm = fz_concat(page_ctm, fz_translate(-trans_cropbox[0], -trans_cropbox[1]));
    
    return page_ctm;
}

static std::pair<std::string, std::map<int, std::string>> CreateCIDFontObjects(
    const std::vector<uint8_t>& ttf_data, int start_obj_id, FT_Face face, const std::string& font_alias,
    const std::unordered_set<uint32_t>& used_codepoints, hb_font_t* subset_hb_font) {
    int stream_obj_id    = start_obj_id;
    int descriptor_obj_id = start_obj_id + 1;
    int cidfont_obj_id   = start_obj_id + 2;
    int tounicode_obj_id = start_obj_id + 3;
    int type0_obj_id     = start_obj_id + 4;
    
    auto compressed_font = compress_zlib(ttf_data);
    
    // Fix for OTF (CFF) fonts in Edge / Adobe
    bool is_cff = false;
    if (ttf_data.size() >= 4) {
        if (ttf_data[0] == 'O' && ttf_data[1] == 'T' && ttf_data[2] == 'T' && ttf_data[3] == 'O') {
            is_cff = true;
        }
    }
    
    std::map<int, std::string> objs;
    
    // Font Stream
    std::string stream_out = std::to_string(stream_obj_id) + " 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(compressed_font.size());
    if (is_cff) {
        stream_out += " /Subtype /OpenType";
    } else {
        stream_out += " /Length1 " + std::to_string(ttf_data.size());
    }
    stream_out += " >>\nstream\n";
    stream_out.append(reinterpret_cast<const char*>(compressed_font.data()), compressed_font.size());
    stream_out += "\nendstream\nendobj\n";
    objs[stream_obj_id] = stream_out;
    
    float scale_em = 1000.0f / (face->units_per_EM > 0 ? face->units_per_EM : 1000);
    int bbox_xMin = face->bbox.xMin * scale_em;
    int bbox_yMin = face->bbox.yMin * scale_em;
    int bbox_xMax = face->bbox.xMax * scale_em;
    int bbox_yMax = face->bbox.yMax * scale_em;
    int ascent    = face->ascender * scale_em;
    int descent   = face->descender * scale_em;
    
    // FontDescriptor
    objs[descriptor_obj_id] = std::to_string(descriptor_obj_id) + " 0 obj\n<< /Type /FontDescriptor /FontName /" + font_alias + " /Flags 32 /FontBBox [" + std::to_string(bbox_xMin) + " " + std::to_string(bbox_yMin) + " " + std::to_string(bbox_xMax) + " " + std::to_string(bbox_yMax) + "] /ItalicAngle 0 /Ascent " + std::to_string(ascent) + " /Descent " + std::to_string(descent) + " /CapHeight " + std::to_string(ascent) + " /StemV 80 ";
    if (is_cff) {
        objs[descriptor_obj_id] += "/FontFile3 " + std::to_string(stream_obj_id) + " 0 R >>\nendobj\n";
    } else {
        objs[descriptor_obj_id] += "/FontFile2 " + std::to_string(stream_obj_id) + " 0 R >>\nendobj\n";
    }
    
    std::string w_array = "[ ";
    const int CHUNK_SIZE = 1000;
    for (int start = 0; start < face->num_glyphs; start += CHUNK_SIZE) {
        int end = std::min((int)face->num_glyphs, start + CHUNK_SIZE);
        w_array += std::to_string(start) + " [ ";
        for (int i = start; i < end; ++i) {
            FT_Load_Glyph(face, i, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
            int w = std::round(face->glyph->metrics.horiAdvance * scale_em);
            w_array += std::to_string(w) + " ";
            if (i % 20 == 19) w_array += "\n";
        }
        w_array += "] \n";
    }
    w_array += "]";
    
    // CIDFontType
    std::string cid_subtype = is_cff ? "/CIDFontType0" : "/CIDFontType2";
    std::string cid_dict = std::to_string(cidfont_obj_id) + " 0 obj\n<< /Type /Font /Subtype " + cid_subtype + " /BaseFont /" + font_alias + " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " + std::to_string(descriptor_obj_id) + " 0 R /DW 1000 /W " + w_array;
    if (!is_cff) {
        cid_dict += " /CIDToGIDMap /Identity";
    }
    cid_dict += " >>\nendobj\n";
    objs[cidfont_obj_id] = cid_dict;
    
    // ToUnicode
    std::string cmap = "/CIDInit /ProcSet findresource begin\n"
                       "12 dict begin\n"
                       "begincmap\n"
                       "/CIDSystemInfo\n"
                       "<< /Registry (Adobe)\n"
                       "   /Ordering (UCS)\n"
                       "   /Supplement 0\n"
                       ">> def\n"
                       "/CMapName /Adobe-Identity-UCS def\n"
                       "/CMapType 2 def\n"
                       "1 begincodespacerange\n"
                       "<0000> <FFFF>\n"
                       "endcodespacerange\n";
    
    std::map<FT_UInt, FT_ULong> cid_to_unicode;
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    FT_UInt gindex;
    FT_ULong charcode = FT_Get_First_Char(face, &gindex);
    while (gindex != 0) {
        if (charcode <= 0xFFFF) {
            cid_to_unicode[gindex] = charcode;
        }
        charcode = FT_Get_Next_Char(face, charcode, &gindex);
    }
    
    std::vector<std::string> bfranges;
    for (const auto& pair : cid_to_unicode) {
        char buf[128];
        snprintf(buf, sizeof(buf), "<%04X> <%04X> <%04lX>", pair.first, pair.first, pair.second);
        bfranges.push_back(buf);
    }
    if (!bfranges.empty()) {
        size_t i = 0;
        while (i < bfranges.size()) {
            size_t chunk_size = std::min<size_t>(100, bfranges.size() - i);
            cmap += std::to_string(chunk_size) + " beginbfrange\n";
            for (size_t j = 0; j < chunk_size; ++j) {
                cmap += bfranges[i + j] + "\n";
            }
            cmap += "endbfrange\n";
            i += chunk_size;
        }
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    auto comp_cmap = compress_zlib(std::vector<uint8_t>(cmap.begin(), cmap.end()));
    std::string cmap_out = std::to_string(tounicode_obj_id) + " 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(comp_cmap.size()) + " >>\nstream\n";
    cmap_out.append(reinterpret_cast<const char*>(comp_cmap.data()), comp_cmap.size());
    cmap_out += "\nendstream\nendobj\n";
    objs[tounicode_obj_id] = cmap_out;

    // Type0
    objs[type0_obj_id] = std::to_string(type0_obj_id) + " 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /" + font_alias + "-Identity-H /Encoding /Identity-H /DescendantFonts [" + std::to_string(cidfont_obj_id) + " 0 R] /ToUnicode " + std::to_string(tounicode_obj_id) + " 0 R >>\nendobj\n";
    
    return {"/" + font_alias, objs};
}

std::string create_hex_string_single(uint32_t gid) {
    char buf[10];
    snprintf(buf, sizeof(buf), "<%04X>", gid);
    return std::string(buf);
}

std::string create_hex_string(const std::vector<uint32_t>& glyph_ids) {
    std::string out = "<";
    char buf[10];
    for (uint32_t gid : glyph_ids) {
        snprintf(buf, sizeof(buf), "%04X", gid);
        out += buf;
    }
    out += ">";
    return out;
}

// Find the closing '>>' that matches the '<<' opened at or just after `start`.
// Skips nested dicts and handles PDF strings (parentheses) to avoid false matches.
static size_t find_matching_dict_end(const std::string& s, size_t start) {
    int depth = 0;
    for (size_t i = start; i + 1 < s.size(); ) {
        if (s[i] == '(' ) {
            // skip PDF string
            ++i;
            while (i < s.size() && s[i] != ')') {
                if (s[i] == '\\') ++i; // escaped char
                ++i;
            }
        } else if (s[i] == '<' && s[i+1] == '<') {
            depth++;
            i += 2;
        } else if (s[i] == '>' && s[i+1] == '>') {
            depth--;
            if (depth == 0) return i;
            i += 2;
        } else {
            ++i;
        }
    }
    return std::string::npos;
}

// -----------------------------------------------------------------------
// Font discovery: scan fonts_dir, classify each .ttf/.otf by bold/italic.
// grid[bold_idx][italic_idx] — 0=false, 1=true.
// -----------------------------------------------------------------------
struct FontSlot {
    std::vector<uint8_t> data;
    FT_Face face = nullptr;
    hb_font_t* hb_font = nullptr;
    std::string pdf_name;   // e.g. "/F_WNZRN_0"
    int type0_obj_id = -1;
    bool used = false;
    std::string family_name;
    std::string family_name_norm;
    bool has_black = false;
    bool has_heavy = false;
    bool has_light = false;
    bool has_thin = false;
    bool is_safe_fallback = false;
    bool is_bold = false;
    bool is_italic = false;
    std::unordered_set<uint32_t> used_codepoints;
    std::vector<uint8_t> used_bitmap;

    std::vector<uint8_t> subset_data;
    FT_Face subset_face = nullptr;
    hb_font_t* subset_hb_font = nullptr;

    ~FontSlot() {
        if (subset_hb_font) hb_font_destroy(subset_hb_font);
        if (subset_face) FT_Done_Face(subset_face);
        if (hb_font) hb_font_destroy(hb_font);
        if (face) FT_Done_Face(face);
    }

    FontSlot() = default;
    FontSlot(const FontSlot&) = delete;
    FontSlot& operator=(const FontSlot&) = delete;

    FontSlot(FontSlot&& other) noexcept {
        data = std::move(other.data);
        face = other.face; other.face = nullptr;
        hb_font = other.hb_font; other.hb_font = nullptr;
        pdf_name = std::move(other.pdf_name);
        type0_obj_id = other.type0_obj_id;
        used = other.used;
        family_name = std::move(other.family_name);
        family_name_norm = std::move(other.family_name_norm);
        has_black = other.has_black;
        has_heavy = other.has_heavy;
        has_light = other.has_light;
        has_thin = other.has_thin;
        is_safe_fallback = other.is_safe_fallback;
        is_bold = other.is_bold;
        is_italic = other.is_italic;
        used_codepoints = std::move(other.used_codepoints);
        used_bitmap = std::move(other.used_bitmap);
        subset_data = std::move(other.subset_data);
        subset_face = other.subset_face; other.subset_face = nullptr;
        subset_hb_font = other.subset_hb_font; other.subset_hb_font = nullptr;
    }
    FontSlot& operator=(FontSlot&& other) noexcept {
        if (this != &other) {
            if (subset_hb_font) hb_font_destroy(subset_hb_font);
            if (subset_face) FT_Done_Face(subset_face);
            if (hb_font) hb_font_destroy(hb_font);
            if (face) FT_Done_Face(face);
            
            data = std::move(other.data);
            face = other.face; other.face = nullptr;
            hb_font = other.hb_font; other.hb_font = nullptr;
            pdf_name = std::move(other.pdf_name);
            type0_obj_id = other.type0_obj_id;
            used = other.used;
            family_name = std::move(other.family_name);
            family_name_norm = std::move(other.family_name_norm);
            has_black = other.has_black;
            has_heavy = other.has_heavy;
            has_light = other.has_light;
            has_thin = other.has_thin;
            is_safe_fallback = other.is_safe_fallback;
            is_bold = other.is_bold;
            is_italic = other.is_italic;
            used_codepoints = std::move(other.used_codepoints);
            used_bitmap = std::move(other.used_bitmap);
            subset_data = std::move(other.subset_data);
            subset_face = other.subset_face; other.subset_face = nullptr;
            subset_hb_font = other.subset_hb_font; other.subset_hb_font = nullptr;
        }
        return *this;
    }
};

#include <mutex>
static std::mutex g_fonts_mutex;
static std::string g_fonts_loaded_dir = "";
static std::vector<FontSlot> g_font_grid[2][2];
static std::vector<std::vector<uint8_t>> g_full_charset_cache[2][2];
static FT_Library g_ft_library = nullptr;
static std::string NormalizeFontName(const std::string& name);

static void ScanFontsDir(const std::string& fonts_dir, FT_Library library, std::vector<FontSlot> grid[2][2]) {
    namespace fs = std::filesystem;
    if (!fs::exists(fonts_dir) || !fs::is_directory(fonts_dir)) return;

    for (const auto& entry : fs::directory_iterator(fonts_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
        // Only allow .ttf to avoid Adobe Acrobat OpenType CFF (OTTO) subsetting bugs
        if (ext != ".ttf") continue;

        // Read font bytes
        std::ifstream f(entry.path(), std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        std::streamsize sz = f.tellg();
        if (sz <= 0) continue;
        f.seekg(0);
        std::vector<uint8_t> data((size_t)sz);
        if (!f.read(reinterpret_cast<char*>(data.data()), sz)) continue;
        
        // Adobe Acrobat has a massive bug/strictness issue with OpenType CFF fonts.
        // It requires embedded CIDFontType0 CFF fonts to be CID-Keyed.
        // But almost all modern CFF fonts (.otf or .ttf) are Name-Keyed.
        // This causes Acrobat to reject the font entirely and render substitute garbage (&$&C).
        // Skip CFF (OTTO) fonts entirely.
        if (data.size() >= 4 && data[0] == 'O' && data[1] == 'T' && data[2] == 'T' && data[3] == 'O') {
            continue;
        }

        FT_Face face;
        if (FT_New_Memory_Face(library, data.data(), (FT_Long)sz, 0, &face)) continue;

        std::string family_name;
        if (face->family_name) {
            family_name = face->family_name;
        }
        std::string fname_lower = family_name;
        std::transform(fname_lower.begin(), fname_lower.end(), fname_lower.begin(), [](unsigned char c){ return std::tolower(c); });

        // 1. Detect via FreeType style flags (most reliable)
        bool is_bold   = (face->style_flags & FT_STYLE_FLAG_BOLD)   != 0;
        bool is_italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;

        // 2. Cross-check via filename keywords (some fonts don't set flags)
        std::string flower = entry.path().filename().string();
        std::transform(flower.begin(), flower.end(), flower.begin(), [](unsigned char c){ return std::tolower(c); });
        if (!is_bold   && (flower.find("bold") != std::string::npos || fname_lower.find("bold") != std::string::npos)) is_bold = true;
        if (!is_italic && (flower.find("italic") != std::string::npos || flower.find("oblique") != std::string::npos || fname_lower.find("italic") != std::string::npos)) is_italic = true;

        int bi = is_bold   ? 1 : 0;
        int ii = is_italic ? 1 : 0;

        FontSlot slot;
        slot.data = std::move(data);
        slot.face = face;
        slot.family_name = family_name;
        slot.family_name_norm = NormalizeFontName(family_name);
        slot.has_black = (fname_lower.find("black") != std::string::npos);
        slot.has_heavy = (fname_lower.find("heavy") != std::string::npos);
        slot.has_light = (fname_lower.find("light") != std::string::npos);
        slot.has_thin  = (fname_lower.find("thin") != std::string::npos);
        slot.is_safe_fallback = true;
        slot.is_bold = is_bold;
        slot.is_italic = is_italic;

        // Prepare used_bitmap
        slot.used_bitmap.assign(1114112, 0);

        // Map subset to quick array
        FT_Select_Charmap(face, FT_ENCODING_UNICODE);
        FT_UInt gindex;
        FT_ULong charcode = FT_Get_First_Char(face, &gindex);
        while (gindex != 0) {
            if (charcode < 1114112) {
                slot.used_bitmap[charcode] = 1;
            }
            charcode = FT_Get_Next_Char(face, charcode, &gindex);
        }

        FT_Set_Pixel_Sizes(face, face->units_per_EM, face->units_per_EM);
        hb_font_t* hb_f = hb_ft_font_create(face, NULL);
        hb_ft_font_set_funcs(hb_f);
        slot.hb_font = hb_f;
        
        grid[bi][ii].push_back(std::move(slot));
    }
}

// Strip PDF subset prefix (e.g. "ABCXYZ+") and style suffixes ("-BoldMT", "-ItalicMT", "-Bold", "-Italic", "MT", "PS")
static std::string NormalizeFontName(const std::string& name) {
    std::string s = name;
    // Remove subset prefix: up to 6 uppercase letters followed by '+'
    if (s.size() > 7) {
        size_t plus = s.find('+');
        if (plus != std::string::npos && plus <= 7) {
            s = s.substr(plus + 1);
        }
    }
    // Remove known suffixes (case-insensitive)
    const char* suffixes[] = {
        "-BoldOblique", "-BoldItalic", "-Bold", "-Oblique", "-Italic",
        "-BoldMT", "-ItalicMT", "-RomanMT", "MT", "PS", "-Roman", nullptr
    };
    for (int k = 0; suffixes[k]; ++k) {
        std::string suf = suffixes[k];
        if (s.size() >= suf.size()) {
            // case-insensitive compare suffix
            std::string tail = s.substr(s.size() - suf.size());
            std::string suf_lower = suf;
            std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c){ return std::tolower(c); });
            std::transform(suf_lower.begin(), suf_lower.end(), suf_lower.begin(), [](unsigned char c){ return std::tolower(c); });
            if (tail == suf_lower) {
                s = s.substr(0, s.size() - suf.size());
                break; // remove at most one suffix
            }
        }
    }
    // Lowercase alphanumeric only
    std::string out;
    for (char c : s) if (std::isalnum((unsigned char)c)) out += std::tolower((unsigned char)c);
    return out;
}

std::vector<uint8_t> InsertTextToMultiplePages(WinExtract::WinPdfDocument* doc, const std::map<int, std::vector<WinInsertTextTask>>& pages_tasks, const std::string& fonts_dir, std::function<void(int, int)> progress_cb, int num_threads_opt) {
    if (!doc || pages_tasks.empty()) {
        return {};
    }
    auto t_start = std::chrono::high_resolution_clock::now();
    // Global caching to prevent reading fonts 100+ times and causing slowdowns
    std::lock_guard<std::mutex> lock(g_fonts_mutex);
    if (g_fonts_loaded_dir != fonts_dir) {
        if (!g_ft_library && FT_Init_FreeType(&g_ft_library)) {
            return {};
        }
        
        for (int b = 0; b < 2; b++) {
            for (int i = 0; i < 2; i++) {
                g_font_grid[b][i].clear();
                g_full_charset_cache[b][i].clear();
            }
        }
        
        if (!fonts_dir.empty()) {
            ScanFontsDir(fonts_dir, g_ft_library, g_font_grid);
        }

        bool any_font = false;
        for (int b = 0; b < 2; b++) for (int i = 0; i < 2; i++) if (!g_font_grid[b][i].empty()) any_font = true;
        if (!any_font) {
            namespace fs = std::filesystem;
            std::vector<fs::path> builtin_candidates;

            const char* src_file = __FILE__;
            if (src_file && *src_file) {
                fs::path src_path(src_file);
                builtin_candidates.push_back(src_path.parent_path().parent_path() / "fonts");
                builtin_candidates.push_back(src_path.parent_path().parent_path() / "winnerz" / "fonts");
            }
            builtin_candidates.push_back(fs::path("fonts"));
            builtin_candidates.push_back(fs::path("winnerz") / "fonts");

            for (const auto& candidate : builtin_candidates) {
                if (fs::exists(candidate) && fs::is_directory(candidate)) {
                    ScanFontsDir(candidate.string(), g_ft_library, g_font_grid);
                    for (int b = 0; b < 2; b++) for (int i = 0; i < 2; i++) if (!g_font_grid[b][i].empty()) any_font = true;
                    if (any_font) break;
                }
            }
        }

        if (!any_font) {
            std::cerr << "No fonts found in user dir '" << fonts_dir << "' and no built-in fonts available." << std::endl;
            return {};
        }

        for (int b = 0; b < 2; b++) {
            for (int i = 0; i < 2; i++) {
                g_full_charset_cache[b][i].resize(g_font_grid[b][i].size());
                for (size_t f = 0; f < g_font_grid[b][i].size(); ++f) {
                    g_full_charset_cache[b][i][f] = g_font_grid[b][i][f].used_bitmap;
                }
            }
        }
        g_fonts_loaded_dir = fonts_dir;
    } else {
        // Reset per-request mutable state
        for (int b = 0; b < 2; b++) {
            for (int i = 0; i < 2; i++) {
                for (auto& slot : g_font_grid[b][i]) {
                    slot.used_codepoints.clear();
                    if (!slot.used) {
                        slot.used = true;
                        slot.used_bitmap.assign(1114112, 0);
                    } else {
                        // Keep used_bitmap as is, we will clear it before subsetting later if needed.
                        // Actually, we are building a cumulative used_bitmap per slot for the whole document!
                        // So we do NOT clear it here! We just keep it.
                    }
                    slot.subset_data.clear();
                    if (slot.subset_face) { FT_Done_Face(slot.subset_face); slot.subset_face = nullptr; }
                    if (slot.subset_hb_font) { hb_font_destroy(slot.subset_hb_font); slot.subset_hb_font = nullptr; }
                    slot.used = false;
                }
            }
        }
    }

    auto& font_grid = g_font_grid;
    auto& full_charset_cache = g_full_charset_cache;

    // Temporarily point GetFontSlot to use full_charset_cache — we do it inline:
    // Re-implement lookup using the saved caches directly.
    auto GetFontSlotFast = [&](const WinInsertTextTask& task, const std::vector<uint32_t>& utf32) -> FontSlot* {
        if (utf32.empty()) return nullptr;
        std::string task_ff_norm = NormalizeFontName(task.font_family);
        int req_b = task.bold ? 1 : 0;
        int req_i = task.italic ? 1 : 0;

        auto check_font_index = [&](int b, int i, size_t f) -> bool {
            const auto& cache = full_charset_cache[b][i][f];
            for (uint32_t cp : utf32) {
                if (cp >= 1114112 || cache[cp] == 0) return false;
            }
            return true;
        };

        auto find_name_in_grid = [&](int b, int i, const std::string& target_norm) -> FontSlot* {
            if (target_norm.empty()) return nullptr;
            for (size_t f = 0; f < font_grid[b][i].size(); ++f) {
                if (!check_font_index(b, i, f)) continue;
                auto& slot = font_grid[b][i][f];
                
                if (slot.family_name_norm == target_norm) return &slot;
                
                if (slot.family_name_norm.find(target_norm) != std::string::npos) {
                    return &slot;
                } else if (slot.family_name_norm.size() >= 4 && target_norm.find(slot.family_name_norm) != std::string::npos) {
                    return &slot;
                }
            }
            return nullptr;
        };

        auto find_fallback_in_grid = [&](int b, int i) -> FontSlot* {
            for (size_t f = 0; f < font_grid[b][i].size(); ++f) {
                if (!check_font_index(b, i, f)) continue;
                auto& slot = font_grid[b][i][f];
                if (slot.is_safe_fallback) return &slot;
            }
            return nullptr;
        };

        FontSlot* best = nullptr;

        if (!task_ff_norm.empty()) {
            best = find_name_in_grid(req_b, req_i, task_ff_norm);
            if (!best) {
                for (int b = 0; b < 2; b++) {
                    for (int i = 0; i < 2; i++) {
                        if (b == req_b && i == req_i) continue;
                        best = find_name_in_grid(b, i, task_ff_norm);
                        if (best) break;
                    }
                    if (best) break;
                }
            }
        }

        if (!best) {
            best = find_fallback_in_grid(req_b, req_i);
            if (!best) {
                for (int b = 0; b < 2; b++) {
                    for (int i = 0; i < 2; i++) {
                        if (b == req_b && i == req_i) continue;
                        best = find_fallback_in_grid(b, i);
                        if (best) break;
                    }
                    if (best) break;
                }
            }
        }
        
        if (!best) {
            for (int b = 0; b < 2; b++) {
                for (int i = 0; i < 2; i++) {
                    for (size_t f = 0; f < font_grid[b][i].size(); ++f) {
                        if (check_font_index(b, i, f)) {
                            best = &font_grid[b][i][f];
                            break;
                        }
                    }
                    if (best) break;
                }
                if (best) break;
            }
        }

        if (!best) {
            for (size_t f = 0; f < font_grid[req_b][req_i].size(); ++f) {
                if (check_font_index(req_b, req_i, f)) {
                    best = &font_grid[req_b][req_i][f];
                    break;
                }
            }
        }

        if (!best) {
            for (int b = 0; b < 2; b++) {
                for (int i = 0; i < 2; i++) {
                    if (b == req_b && i == req_i) continue;
                    for (size_t f = 0; f < font_grid[b][i].size(); ++f) {
                        if (check_font_index(b, i, f)) {
                            best = &font_grid[b][i][f];
                            break;
                        }
                    }
                    if (best) break;
                }
                if (best) break;
            }
        }

        // -----------------------------------------------------------
        // FINAL FALLBACK: Ignore missing glyphs (check_font_index).
        // It's better to render text with "tofu boxes" (missing glyphs)
        // than to completely drop the line.
        // -----------------------------------------------------------
        if (!best) {
            auto find_name_in_grid_no_check = [&](int b, int i, const std::string& target_norm) -> FontSlot* {
                if (target_norm.empty()) return nullptr;
                for (size_t f = 0; f < font_grid[b][i].size(); ++f) {
                    auto& slot = font_grid[b][i][f];
                    if (slot.family_name_norm == target_norm) return &slot;
                    if (slot.family_name_norm.find(target_norm) != std::string::npos) return &slot;
                    if (slot.family_name_norm.size() >= 4 && target_norm.find(slot.family_name_norm) != std::string::npos) return &slot;
                }
                return nullptr;
            };

            if (!task_ff_norm.empty()) {
                best = find_name_in_grid_no_check(req_b, req_i, task_ff_norm);
                if (!best) {
                    for (int b = 0; b < 2; b++) {
                        for (int i = 0; i < 2; i++) {
                            if (b == req_b && i == req_i) continue;
                            best = find_name_in_grid_no_check(b, i, task_ff_norm);
                            if (best) break;
                        }
                        if (best) break;
                    }
                }
            }
            
            if (!best) {
                // Just grab the first available font
                for (int b = 0; b < 2 && !best; b++) {
                    for (int i = 0; i < 2 && !best; i++) {
                        if (!font_grid[b][i].empty()) best = &font_grid[b][i][0];
                    }
                }
            }
        }

        return best;
    };

    std::map<int, std::vector<FontSlot*>> precomputed_slots;
    for (const auto& item : pages_tasks) {
        int page_index = item.first;
        for (const auto& task : item.second) {
            auto utf32 = Utf8ToUtf32(task.text);
            FontSlot* slot = GetFontSlotFast(task, utf32);
            precomputed_slots[page_index].push_back(slot);
            if (slot) {
                slot->used = true;
                for (uint32_t cp : utf32) slot->used_codepoints.insert(cp);
            }
        }
    }
    auto t_font_resolve = std::chrono::high_resolution_clock::now();

    // ---- Embed ONLY used font variants as CID fonts ----
    int next_obj_id = doc->get_max_obj_id() + 1;
    std::string all_font_entries;
    std::map<int, std::string> new_objects;

    // Slot aliases: (bold=0,italic=0)→RN, (1,0)→BN, (0,1)→RI, (1,1)→BI
    const char* alias_table[2][2] = { {"F_WNZRN", "F_WNZRI"}, {"F_WNZBN", "F_WNZBI"} };
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 2; i++) {
            for (size_t f_idx = 0; f_idx < font_grid[b][i].size(); ++f_idx) {
                auto& slot = font_grid[b][i][f_idx];
                if (!slot.used) continue;
                
                // Bypass HarfBuzz subsetting entirely. Adobe Acrobat is notoriously strict
                // about embedded subset fonts missing standard tables (like 'name', 'OS/2').
                // By embedding the full original font, we guarantee 100% compatibility across
                // all strict PDF viewers, including Edge and Acrobat.
                slot.subset_data = slot.data;
                
                if (slot.subset_face == nullptr) {
                    FT_New_Memory_Face(g_ft_library, slot.subset_data.data(), slot.subset_data.size(), 0, &slot.subset_face);
                    if (slot.subset_face) {
                        FT_Set_Pixel_Sizes(slot.subset_face, slot.subset_face->units_per_EM, slot.subset_face->units_per_EM);
                        slot.subset_hb_font = hb_ft_font_create(slot.subset_face, NULL);
                        hb_ft_font_set_funcs(slot.subset_hb_font);
                    }
                }
                
                FT_Face active_face = slot.subset_face ? slot.subset_face : slot.face;
                const char* ps_name_cstr = FT_Get_Postscript_Name(active_face);
                std::string base_font_name = ps_name_cstr ? std::string(ps_name_cstr) : (std::string(alias_table[b][i]) + "_" + std::to_string(f_idx));
                std::replace_if(base_font_name.begin(), base_font_name.end(), [](char c){ return c <= 32 || c > 126 || c == '/' || c == '<' || c == '>' || c == '[' || c == ']' || c == '(' || c == ')'; }, '-');
                
                std::string subset_tag = "";
                for (int t = 0; t < 6; ++t) {
                    subset_tag += (char)('A' + ((f_idx + b + i + t * 7) % 26));
                }
                base_font_name = subset_tag + "+" + base_font_name;
                
                auto [pdf_name, objs] = CreateCIDFontObjects(
                    slot.subset_data, next_obj_id, active_face, base_font_name, slot.used_codepoints, slot.subset_hb_font);
                slot.pdf_name    = pdf_name;
                slot.type0_obj_id = next_obj_id + 4;
                next_obj_id += 5;
                for (const auto& kv : objs) new_objects[kv.first] = kv.second;
                all_font_entries += pdf_name + " " + std::to_string(slot.type0_obj_id) + " 0 R ";
            }
        }
    }
    auto t_font_subset = std::chrono::high_resolution_clock::now();


    std::map<int, std::vector<uint8_t>> pages_streams;
    std::map<int, std::string> updated_objects;

    int total_pages = pages_tasks.size();
    std::atomic<int> pages_processed(0);
    std::mutex output_mutex;
    std::mutex doc_mutex;

    std::vector<std::pair<int, std::vector<WinInsertTextTask>>> task_list(pages_tasks.begin(), pages_tasks.end());
    std::atomic<size_t> current_task{0};

    int num_threads = num_threads_opt;
    if (num_threads <= 0) num_threads = std::thread::hardware_concurrency();
    if (num_threads <= 0) num_threads = 4;
    int actual_threads = std::min((int)num_threads, (int)task_list.size());

    std::vector<std::thread> threads;
    for (int t = 0; t < actual_threads; ++t) {
        threads.emplace_back([&]() {
            while (true) {
                size_t i = current_task.fetch_add(1);
                if (i >= task_list.size()) break;
                
                int page_index = task_list[i].first;
                const auto& tasks = task_list[i].second;

                WinExtract::WinPdfObject page_obj;
                WinExtract::WinPageGeometry geo;
                fz_matrix page_ctm;
                fz_matrix inv_ctm;
                
                {
                    std::lock_guard<std::mutex> lock(doc_mutex);
                    page_obj = doc->read_obj(doc->get_page_id(page_index));
                    geo = doc->get_page_geometry(page_index);
                }
                
                std::string page_dict = page_obj.dict;
                page_ctm = ComputePageCTM(geo);
                inv_ctm = fz_invert_matrix(page_ctm);

                // Robust font injection handling inherited and indirect /Resources
                bool font_injected = false;
                int res_node_id = page_obj.id;
                WinExtract::WinPdfObject res_node_obj = page_obj;
                std::string res_dict = page_dict;
                bool is_indirect_res = false;
                int indirect_res_id = -1;

                std::unordered_set<int> visited;
                while (visited.insert(res_node_id).second) {
                    size_t res_pos = res_dict.find("/Resources");
                    if (res_pos != std::string::npos) {
                        size_t val_start = res_dict.find_first_not_of(" \t\r\n", res_pos + 10);
                        if (val_start != std::string::npos) {
                            if (res_dict[val_start] == '<' && res_dict[val_start+1] == '<') {
                                break; // Found inline /Resources
                            } else if (std::isdigit((unsigned char)res_dict[val_start])) {
                                int ref_id = std::stoi(res_dict.substr(val_start));
                                indirect_res_id = ref_id;
                                is_indirect_res = true;
                                {
                                    std::lock_guard<std::mutex> lock(doc_mutex);
                                    res_node_obj = doc->read_obj(ref_id);
                                }
                                res_dict = res_node_obj.dict;
                                res_node_id = ref_id;
                                break;
                            }
                        }
                    }
                    size_t parent_pos = res_dict.find("/Parent");
                    if (parent_pos != std::string::npos) {
                        size_t val_start = res_dict.find_first_not_of(" \t\r\n", parent_pos + 7);
                        if (val_start != std::string::npos && std::isdigit((unsigned char)res_dict[val_start])) {
                            int ref_id = std::stoi(res_dict.substr(val_start));
                            res_node_id = ref_id;
                            {
                                std::lock_guard<std::mutex> lock(doc_mutex);
                                res_node_obj = doc->read_obj(ref_id);
                            }
                            res_dict = res_node_obj.dict;
                            continue;
                        }
                    }
                    break;
                }

                // Now we inject into res_dict
                bool res_dict_modified = false;
                if (is_indirect_res) {
                    size_t font_key_pos = res_dict.find("/Font");
                    if (font_key_pos != std::string::npos) {
                        size_t val_start = res_dict.find_first_not_of(" \t\r\n", font_key_pos + 5);
                        if (val_start != std::string::npos && res_dict[val_start] == '<' && res_dict[val_start+1] == '<') {
                            size_t fdict_end = find_matching_dict_end(res_dict, val_start);
                            if (fdict_end != std::string::npos) {
                                res_dict.insert(fdict_end, all_font_entries);
                                font_injected = true;
                                res_dict_modified = true;
                            }
                        } else if (val_start != std::string::npos && std::isdigit((unsigned char)res_dict[val_start])) {
                            int font_ref_id = std::stoi(res_dict.substr(val_start));
                            WinExtract::WinPdfObject font_obj;
                            {
                                std::lock_guard<std::mutex> lock(doc_mutex);
                                font_obj = doc->read_obj(font_ref_id);
                            }
                            std::string f_dict = font_obj.dict;
                            size_t f_end = f_dict.rfind(">>");
                            if (f_end != std::string::npos) {
                                f_dict.insert(f_end, all_font_entries);
                                std::lock_guard<std::mutex> lock(output_mutex);
                                updated_objects[font_ref_id] = std::to_string(font_ref_id) + " " + std::to_string(font_obj.gen) + " obj\n" + f_dict + "\nendobj\n";
                                font_injected = true;
                            }
                        }
                    }
                    if (!font_injected) {
                        size_t endobj = res_dict.rfind(">>");
                        if (endobj != std::string::npos) {
                            res_dict.insert(endobj, " /Font << " + all_font_entries + ">> ");
                            font_injected = true;
                            res_dict_modified = true;
                        }
                    }
                    if (font_injected && res_dict_modified) {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        updated_objects[indirect_res_id] = std::to_string(res_node_obj.id) + " " + std::to_string(res_node_obj.gen) + " obj\n" + res_dict + "\nendobj\n";
                    }
                } else {
                    size_t res_pos = res_dict.find("/Resources");
                    if (res_pos != std::string::npos) {
                        size_t res_dict_start = res_dict.find("<<", res_pos);
                        if (res_dict_start != std::string::npos) {
                            size_t res_dict_end = find_matching_dict_end(res_dict, res_dict_start);
                            size_t search_end = (res_dict_end != std::string::npos) ? res_dict_end : res_dict.size();
                            size_t font_key_pos = res_dict.find("/Font", res_dict_start);
                            
                            if (font_key_pos != std::string::npos && font_key_pos < search_end) {
                                size_t val_start = res_dict.find_first_not_of(" \t\r\n", font_key_pos + 5);
                                if (val_start != std::string::npos && val_start < search_end && res_dict[val_start] == '<' && res_dict[val_start+1] == '<') {
                                    size_t fdict_end = find_matching_dict_end(res_dict, val_start);
                                    if (fdict_end != std::string::npos) {
                                        res_dict.insert(fdict_end, all_font_entries);
                                        font_injected = true;
                                        res_dict_modified = true;
                                    }
                                } else if (val_start != std::string::npos && val_start < search_end && std::isdigit((unsigned char)res_dict[val_start])) {
                                    int font_ref_id = std::stoi(res_dict.substr(val_start));
                                    WinExtract::WinPdfObject font_obj;
                                    {
                                        std::lock_guard<std::mutex> lock(doc_mutex);
                                        font_obj = doc->read_obj(font_ref_id);
                                    }
                                    std::string f_dict = font_obj.dict;
                                    size_t f_end = f_dict.rfind(">>");
                                    if (f_end != std::string::npos) {
                                        f_dict.insert(f_end, all_font_entries);
                                        std::lock_guard<std::mutex> lock(output_mutex);
                                        updated_objects[font_ref_id] = std::to_string(font_ref_id) + " " + std::to_string(font_obj.gen) + " obj\n" + f_dict + "\nendobj\n";
                                        font_injected = true;
                                    }
                                }
                            }
                            if (!font_injected && res_dict_end != std::string::npos) {
                                res_dict.insert(res_dict_end, " /Font << " + all_font_entries + ">> ");
                                font_injected = true;
                                res_dict_modified = true;
                            }
                        }
                    } else {
                        size_t endobj = page_dict.rfind(">>");
                        if (endobj != std::string::npos) {
                            page_dict.insert(endobj, " /Resources << /Font << " + all_font_entries + ">> >> ");
                            font_injected = true;
                            res_dict_modified = true;
                        }
                    }
                    
                    if (font_injected && res_dict_modified) {
                        if (res_node_id == page_obj.id) {
                            page_dict = res_dict;
                        } else {
                            std::lock_guard<std::mutex> lock(output_mutex);
                            updated_objects[res_node_id] = std::to_string(res_node_obj.id) + " " + std::to_string(res_node_obj.gen) + " obj\n" + res_dict + "\nendobj\n";
                        }
                    }
                }

                std::string updated_obj_str = std::to_string(page_obj.id) + " " + std::to_string(page_obj.gen) + " obj\n" + page_dict + "\nendobj\n";

                std::string page_stream;
                page_stream += "q\n";
                // Reset all text state parameters to isolate from previous content streams
                page_stream += "0 Tc 0 Tw 100 Tz 0 TL 0 Tr 0 Ts\n";

                std::vector<FontSlot*> p_slots = precomputed_slots[page_index];
                size_t task_idx = 0;

                for (const auto& task : tasks) {
                    float bbox_w = task.x1 - task.x0;
                    float bbox_h = task.y1 - task.y0;
                    FontSlot* slot = p_slots[task_idx++];
                    if (bbox_w <= 0 || bbox_h <= 0 || !slot) continue;

                    hb_font_t* cur_hb_font = slot->subset_hb_font ? slot->subset_hb_font : slot->hb_font;
                    const std::string& cur_font_name = slot->pdf_name;

                    float fs = task.font_size;
                    std::vector<ShapedLine> lines;
                    
                    std::string clean_text = task.text;
                    clean_text.erase(std::remove(clean_text.begin(), clean_text.end(), '\r'), clean_text.end());

                    if (task.multiline) {
                        std::lock_guard<std::mutex> lock(global_shaping_mutex);
                        lines = HarfbuzzWordWrap(clean_text, cur_hb_font, fs, bbox_w);
                    } else {
                        {
                            std::lock_guard<std::mutex> lock(global_shaping_mutex);
                            lines = HarfbuzzWordWrap(clean_text, cur_hb_font, fs, 1e9f);
                        }
                        
                        if (!lines.empty()) {
                            float total_w = 0.0f;
                            for (const auto& ln : lines) total_w += ln.width;

                            if (total_w > bbox_w && total_w > 0.0f) {
                                fs = fs * (bbox_w / total_w);
                                {
                                    std::lock_guard<std::mutex> lock(global_shaping_mutex);
                                    lines = HarfbuzzWordWrap(clean_text, cur_hb_font, fs, 1e9f);
                                }
                            }
                        }
                    }
                    if (lines.empty()) continue;

                    auto to_pdf = [&inv_ctm](float xi, float yi) -> std::pair<float, float> {
                        return { xi * inv_ctm.a + yi * inv_ctm.c + inv_ctm.e, xi * inv_ctm.b + yi * inv_ctm.d + inv_ctm.f };
                    };

                    float baseline_y_img = task.y0 + fs * 0.8f;
                    if (baseline_y_img > task.y1) baseline_y_img = task.y1;

                    auto sanitize_commas = [](char* buf) {
                        for (char* p = buf; *p; ++p) {
                            if (*p == ',') *p = '.';
                        }
                    };

                    page_stream += "BT\n";
                    char font_buf[80];
                    snprintf(font_buf, sizeof(font_buf), "%s %.3f Tf\n", cur_font_name.c_str(), fs);
                    sanitize_commas(font_buf);
                    page_stream += font_buf;

                    char color_buf[64];
                    snprintf(color_buf, sizeof(color_buf), "%.3f %.3f %.3f rg\n", task.r / 255.0f, task.g / 255.0f, task.b / 255.0f);
                    sanitize_commas(color_buf);
                    page_stream += color_buf;

                    bool synth_italic = task.italic && !slot->is_italic;
                    bool synth_bold = task.bold && !slot->is_bold;

                    if (synth_bold) {
                        page_stream += "2 Tr\n";
                        char stroke_color[64];
                        snprintf(stroke_color, sizeof(stroke_color), "%.3f %.3f %.3f RG\n", task.r / 255.0f, task.g / 255.0f, task.b / 255.0f);
                        sanitize_commas(stroke_color);
                        page_stream += stroke_color;
                        char lw_buf[64];
                        snprintf(lw_buf, sizeof(lw_buf), "%.4f w\n", fs * 0.03f);
                        sanitize_commas(lw_buf);
                        page_stream += lw_buf;
                    }

                    float skew = synth_italic ? 0.2125f : 0.0f;

                    bool is_rtl_line = false;
                    std::vector<uint32_t> utf32 = Utf8ToUtf32(clean_text);
                    for (uint32_t c : utf32) {
                        int bc = ucdn_get_bidi_class(c);
                        if (bc == UCDN_BIDI_CLASS_R || bc == UCDN_BIDI_CLASS_AL || bc == UCDN_BIDI_CLASS_RLE || bc == UCDN_BIDI_CLASS_RLO) {
                            is_rtl_line = true;
                            break;
                        }
                    }

                    float cur_y_img = baseline_y_img;
                    for (const auto& line : lines) {
                        float cur_x_img = task.x0;
                        if (is_rtl_line && line.width < bbox_w) {
                            cur_x_img = task.x1 - line.width;
                        }
                        
                        for (const auto& g : line.glyphs) {
                            float xi = cur_x_img + g.x_offset;
                            float yi = cur_y_img + g.y_offset;

                            auto [xp, yp] = to_pdf(xi, yi);

                            char tm_buf[128];
                            snprintf(tm_buf, sizeof(tm_buf), "1 0 %.4f 1 %.3f %.3f Tm\n", skew, xp, yp);
                            sanitize_commas(tm_buf);
                            page_stream += tm_buf;
                            page_stream += create_hex_string_single(g.id) + " Tj\n";
                            cur_x_img += g.x_advance;
                        }
                        cur_y_img += fs * 1.2f;
                    }

                    if (synth_bold) {
                        page_stream += "0 Tr\n";
                    }
                    page_stream += "ET\n";
                }
                
                page_stream += "Q\n";
                std::vector<uint8_t> stream_data(page_stream.begin(), page_stream.end());
                std::vector<uint8_t> compressed_stream = compress_zlib(stream_data);

                {
                    std::lock_guard<std::mutex> lock(output_mutex);
                    updated_objects[page_obj.id] = updated_obj_str;
                    pages_streams[page_index] = std::move(compressed_stream);
                }
                
                int processed = ++pages_processed;
                if (progress_cb) progress_cb(processed, total_pages);
            }
        });
    }

    for (auto& th : threads) {
        if (th.joinable()) th.join();
    }
    auto t_pages_loop = std::chrono::high_resolution_clock::now();

    // Cleanup all font slots per-request mutated data only
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 2; i++) {
            for (auto& slot : font_grid[b][i]) {
                slot.used_codepoints.clear();
                if (slot.subset_hb_font) { hb_font_destroy(slot.subset_hb_font); slot.subset_hb_font = nullptr; }
                if (slot.subset_face) { FT_Done_Face(slot.subset_face); slot.subset_face = nullptr; }
                slot.subset_data.clear();
                slot.used = false;
            }
        }
    }
    // Do NOT FT_Done_FreeType(g_ft_library) because we cache fonts globally

    // Call save_incremental_update on WinPdfDocument
    // We already added new_objects, we just need to use save_multiple_pages_content_incremental_to_bytes logic but extended.
    // Wait, save_incremental_update handles both updated and new objects!
    // But it doesn't automatically create stream objects for pages_streams. We need to do it here.

    // stream_start_id: right after all font objects (next_obj_id already advanced past them)
    int stream_start_id = next_obj_id;
    
    // Create global q and Q streams to wrap original contents and isolate their CTM state.
    // Adding extra newlines ensures safe concatenation if the previous stream lacked a trailing separator.
    int q_stream_id = stream_start_id++;
    new_objects[q_stream_id] = std::to_string(q_stream_id) + " 0 obj\n<< /Length 3 >>\nstream\n\nq\n\nendstream\nendobj\n";
    
    int Q_stream_id = stream_start_id++;
    new_objects[Q_stream_id] = std::to_string(Q_stream_id) + " 0 obj\n<< /Length 3 >>\nstream\n\nQ\n\nendstream\nendobj\n";

    for (const auto& item : pages_streams) {
        int page_index = item.first;
        WinExtract::WinPdfObject page_obj = doc->read_obj(doc->get_page_id(page_index));
        int stream_id = stream_start_id++;
        std::string stream_content = std::to_string(stream_id) + " 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(item.second.size()) + " >>\nstream\n";
        stream_content.append(reinterpret_cast<const char*>(item.second.data()), item.second.size());
        stream_content += "\nendstream\nendobj\n";
        new_objects[stream_id] = stream_content;
        
        // Update the page dictionary to wrap existing contents in q...Q and append new stream
        std::string dict = updated_objects[page_obj.id];
        size_t contents_pos = dict.find("/Contents");
        if (contents_pos != std::string::npos) {
            size_t value_start = dict.find_first_not_of(" \t\r\n", contents_pos + 9);
            if (value_start != std::string::npos) {
                if (dict[value_start] == '[') {
                    // It's an array: [ old1 old2 ] -> [ q_stream old1 old2 Q_stream new_stream ]
                    size_t array_end = dict.find("]", value_start);
                    if (array_end != std::string::npos) {
                        dict.insert(array_end, " " + std::to_string(Q_stream_id) + " 0 R " + std::to_string(stream_id) + " 0 R ");
                        dict.insert(value_start + 1, " " + std::to_string(q_stream_id) + " 0 R ");
                    }
                } else {
                    // It's a single reference: 5 0 R
                    size_t r_pos = dict.find(" R", value_start);
                    if (r_pos != std::string::npos) {
                        int ref_id = std::stoi(dict.substr(value_start));
                        WinExtract::WinPdfObject ref_obj;
                        {
                            std::lock_guard<std::mutex> lock(doc_mutex);
                            ref_obj = doc->read_obj(ref_id);
                        }
                        size_t first_non_ws = ref_obj.dict.find_first_not_of(" \t\r\n");
                        if (first_non_ws != std::string::npos && ref_obj.dict[first_non_ws] == '[') {
                            // The referenced object is an array! Modify the array object directly.
                            std::string arr_dict = ref_obj.dict;
                            size_t array_end = arr_dict.find("]", first_non_ws);
                            if (array_end != std::string::npos) {
                                arr_dict.insert(array_end, " " + std::to_string(Q_stream_id) + " 0 R " + std::to_string(stream_id) + " 0 R ");
                                arr_dict.insert(first_non_ws + 1, " " + std::to_string(q_stream_id) + " 0 R ");
                                std::lock_guard<std::mutex> lock(output_mutex);
                                updated_objects[ref_id] = std::to_string(ref_id) + " " + std::to_string(ref_obj.gen) + " obj\n" + arr_dict + "\nendobj\n";
                            }
                        } else {
                            // It's a stream object. Safe to wrap in the page dict.
                            dict.insert(r_pos + 2, " " + std::to_string(Q_stream_id) + " 0 R " + std::to_string(stream_id) + " 0 R ]");
                            dict.insert(value_start, "[ " + std::to_string(q_stream_id) + " 0 R ");
                        }
                    }
                }
            }
        } else {
            dict.insert(dict.rfind(">>"), " /Contents " + std::to_string(stream_id) + " 0 R ");
        }
        updated_objects[page_obj.id] = dict;
    }
    auto t_dict_update = std::chrono::high_resolution_clock::now();

    auto result = doc->save_incremental_update(updated_objects, new_objects);
    auto t_end = std::chrono::high_resolution_clock::now();
    
    std::cout << "C++ Profiling:\n";
    std::cout << "  Font Resolve: " << std::chrono::duration<double>(t_font_resolve - t_start).count() << "s\n";
    std::cout << "  Font Subset : " << std::chrono::duration<double>(t_font_subset - t_font_resolve).count() << "s\n";
    std::cout << "  Pages Loop  : " << std::chrono::duration<double>(t_pages_loop - t_font_subset).count() << "s\n";
    std::cout << "  Dict Update : " << std::chrono::duration<double>(t_dict_update - t_pages_loop).count() << "s\n";
    std::cout << "  Save Increm : " << std::chrono::duration<double>(t_end - t_dict_update).count() << "s\n";
    
    return result;
}
std::vector<uint8_t> InsertRectsToMultiplePages(WinExtract::WinPdfDocument* doc, const std::map<int, std::vector<WinInsertRectTask>>& pages_tasks, std::function<void(int, int)> progress_cb, int num_threads_opt) {
    std::mutex doc_mutex;
    std::mutex output_mutex;
    std::map<int, std::string> updated_objects;
    std::map<int, std::string> new_objects;
    std::map<int, std::string> pages_streams; // using strings directly for uncompressed streams

    auto t_start = std::chrono::high_resolution_clock::now();

    // 1. Prepare raw content stream for each page
    for (const auto& item : pages_tasks) {
        int page_index = item.first;
        const auto& tasks = item.second;
        if (tasks.empty()) continue;

        WinExtract::WinPageGeometry geo;
        {
            std::lock_guard<std::mutex> lock(doc_mutex);
            geo = doc->get_page_geometry(page_index);
        }
        fz_matrix page_ctm = ComputePageCTM(geo);
        fz_matrix inv_ctm = fz_invert_matrix(page_ctm);

        auto to_pdf = [&inv_ctm](float xi, float yi) -> std::pair<float, float> {
            return { xi * inv_ctm.a + yi * inv_ctm.c + inv_ctm.e, xi * inv_ctm.b + yi * inv_ctm.d + inv_ctm.f };
        };

        std::string stream_content;
        for (const auto& rect : tasks) {
            float r = std::max(0, std::min(255, rect.r)) / 255.0f;
            float g = std::max(0, std::min(255, rect.g)) / 255.0f;
            float b = std::max(0, std::min(255, rect.b)) / 255.0f;

            auto [pdf_x0, pdf_y0] = to_pdf(rect.x0, rect.y1); 
            auto [pdf_x1, pdf_y1] = to_pdf(rect.x1, rect.y0); 

            float x = std::min(pdf_x0, pdf_x1);
            float y = std::min(pdf_y0, pdf_y1);
            float w = std::abs(pdf_x1 - pdf_x0);
            float h = std::abs(pdf_y1 - pdf_y0);

            char buf[128];
            snprintf(buf, sizeof(buf), "%.3f %.3f %.3f rg\n%.3f %.3f %.3f %.3f re\nf\n", r, g, b, x, y, w, h);
            stream_content += buf;
        }
        
        pages_streams[page_index] = stream_content;
        if (progress_cb) progress_cb(page_index, static_cast<int>(pages_tasks.size()));
    }

    auto t_pages_loop = std::chrono::high_resolution_clock::now();

    int next_obj_id = doc->get_max_obj_id() + 1;
    int stream_start_id = next_obj_id;
    
    // Create global q and Q streams to wrap original contents
    int q_stream_id = stream_start_id++;
    new_objects[q_stream_id] = std::to_string(q_stream_id) + " 0 obj\n<< /Length 3 >>\nstream\n\nq\n\nendstream\nendobj\n";
    
    int Q_stream_id = stream_start_id++;
    new_objects[Q_stream_id] = std::to_string(Q_stream_id) + " 0 obj\n<< /Length 3 >>\nstream\n\nQ\n\nendstream\nendobj\n";

    for (const auto& item : pages_streams) {
        int page_index = item.first;
        WinExtract::WinPdfObject page_obj = doc->read_obj(doc->get_page_id(page_index));
        int stream_id = stream_start_id++;
        
        std::string stream_content = std::to_string(stream_id) + " 0 obj\n<< /Length " + std::to_string(item.second.size()) + " >>\nstream\n";
        stream_content.append(item.second);
        stream_content += "\nendstream\nendobj\n";
        new_objects[stream_id] = stream_content;
        
        std::string dict = page_obj.dict; // Need to fetch dict if updated_objects doesn't have it.
        size_t contents_pos = dict.find("/Contents");
        if (contents_pos != std::string::npos) {
            size_t value_start = dict.find_first_not_of(" \t\r\n", contents_pos + 9);
            if (value_start != std::string::npos) {
                if (dict[value_start] == '[') {
                    size_t array_end = dict.find("]", value_start);
                    if (array_end != std::string::npos) {
                        dict.insert(array_end, " " + std::to_string(Q_stream_id) + " 0 R " + std::to_string(stream_id) + " 0 R ");
                        dict.insert(value_start + 1, " " + std::to_string(q_stream_id) + " 0 R ");
                    }
                } else {
                    size_t r_pos = dict.find(" R", value_start);
                    if (r_pos != std::string::npos) {
                        int ref_id = std::stoi(dict.substr(value_start));
                        WinExtract::WinPdfObject ref_obj = doc->read_obj(ref_id);
                        size_t first_non_ws = ref_obj.dict.find_first_not_of(" \t\r\n");
                        if (first_non_ws != std::string::npos && ref_obj.dict[first_non_ws] == '[') {
                            std::string arr_dict = ref_obj.dict;
                            size_t array_end = arr_dict.find("]", first_non_ws);
                            if (array_end != std::string::npos) {
                                arr_dict.insert(array_end, " " + std::to_string(Q_stream_id) + " 0 R " + std::to_string(stream_id) + " 0 R ");
                                arr_dict.insert(first_non_ws + 1, " " + std::to_string(q_stream_id) + " 0 R ");
                                updated_objects[ref_id] = std::to_string(ref_id) + " " + std::to_string(ref_obj.gen) + " obj\n" + arr_dict + "\nendobj\n";
                            }
                        } else {
                            dict.insert(r_pos + 2, " " + std::to_string(Q_stream_id) + " 0 R " + std::to_string(stream_id) + " 0 R ]");
                            dict.insert(value_start, "[ " + std::to_string(q_stream_id) + " 0 R ");
                        }
                    }
                }
            }
        } else {
            dict.insert(dict.rfind(">>"), " /Contents " + std::to_string(stream_id) + " 0 R ");
        }
        updated_objects[page_obj.id] = std::to_string(page_obj.id) + " " + std::to_string(page_obj.gen) + " obj\n" + dict + "\nendobj\n";
    }
    
    auto t_dict_update = std::chrono::high_resolution_clock::now();
    auto result = doc->save_incremental_update(updated_objects, new_objects);
    auto t_end = std::chrono::high_resolution_clock::now();
    
    std::cout << "Rect Insert Profiling:\n";
    std::cout << "  Streams Gen : " << std::chrono::duration<double>(t_pages_loop - t_start).count() << "s\n";
    std::cout << "  Dict Update : " << std::chrono::duration<double>(t_dict_update - t_pages_loop).count() << "s\n";
    std::cout << "  Save Increm : " << std::chrono::duration<double>(t_end - t_dict_update).count() << "s\n";
    
    return result;
}
float MeasureTextWidth(const std::string& text, const std::string& font_path, float font_size, bool is_bold, bool is_italic) {
#ifndef WINEXTRACT_USE_FREETYPE
    return 0.0f;
#else
    if (text.empty() || font_path.empty()) return 0.0f;
    std::lock_guard<std::mutex> lock(global_shaping_mutex);

    FT_Library ft_library = nullptr;
    if (FT_Init_FreeType(&ft_library)) return 0.0f;

    FT_Face ft_face = nullptr;
    if (FT_New_Face(ft_library, font_path.c_str(), 0, &ft_face)) {
        FT_Done_FreeType(ft_library);
        return 0.0f;
    }

    FT_Set_Char_Size(ft_face, 0, (int)(font_size * 64), 72, 72);

    hb_font_t* hb_font = hb_ft_font_create(ft_face, NULL);
    if (!hb_font) {
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        return 0.0f;
    }

    hb_buffer_t* hb_buffer = hb_buffer_create();
    hb_buffer_add_utf8(hb_buffer, text.c_str(), -1, 0, -1);
    hb_buffer_guess_segment_properties(hb_buffer);
    
    hb_shape(hb_font, hb_buffer, NULL, 0);

    unsigned int glyph_count;
    hb_glyph_position_t* glyph_pos = hb_buffer_get_glyph_positions(hb_buffer, &glyph_count);
    
    float total_width = 0.0f;
    for (unsigned int i = 0; i < glyph_count; ++i) {
        total_width += glyph_pos[i].x_advance;
    }
    
    total_width = total_width / 64.0f;
    
    hb_buffer_destroy(hb_buffer);
    hb_font_destroy(hb_font);
    FT_Done_Face(ft_face);
    FT_Done_FreeType(ft_library);

    return total_width;
#endif
}

} // namespace Winnerz
#endif
