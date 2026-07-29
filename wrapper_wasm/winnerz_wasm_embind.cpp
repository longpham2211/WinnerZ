// winnerz_wasm_embind.cpp
#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <fstream>

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include "nlohmann/json.hpp"

#include "extract_text/extractor_logic.hpp"
#include "extract_text/pdf_engine.hpp"
#include "redactor/redactor.hpp"

#define WINNERZ_WASM_BUILD 1
#include "insert_text/insert.hpp"

#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
#include "drawing.hpp"
#include "preview_pdf.hpp"
#include "fpdfview.h"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include "fpdf_ppo.h"
#endif

using json = nlohmann::json;
using namespace emscripten;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Memory & Byte Transfer Helpers (CRITICAL FOR WASM)
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> ValToVec(const emscripten::val& v) {
    if (v.isUndefined() || v.isNull()) return {};
    unsigned int l = v["byteLength"].as<unsigned int>();
    std::vector<uint8_t> rv(l);
    emscripten::val memoryView{emscripten::typed_memory_view(l, rv.data())};
    memoryView.call<void>("set", v);
    return rv;
}

static emscripten::val VecToVal(const std::vector<uint8_t>& v) {
    if (v.empty()) return emscripten::val::null();
    return emscripten::val(
        emscripten::typed_memory_view(v.size(), v.data())
    ).call<emscripten::val>("slice"); 
}

// ─────────────────────────────────────────────────────────────────────────────
// Math & Core Logic (100% matched with Python)
// ─────────────────────────────────────────────────────────────────────────────
struct fz_matrix { float a, b, c, d, e, f; };

static fz_matrix fz_scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }

static fz_matrix fz_pre_rotate(fz_matrix m, float degrees) {
    float angle = degrees * 3.14159265358979323846f / 180.0f;
    float s = std::sin(angle), c = std::cos(angle);
    fz_matrix r = {c, s, -s, c, 0, 0};
    return {
        r.a * m.a + r.b * m.c, r.a * m.b + r.b * m.d,
        r.c * m.a + r.d * m.c, r.c * m.b + r.d * m.d,
        m.e, m.f
    };
}

static fz_matrix fz_translate(float tx, float ty) { return {1, 0, 0, 1, tx, ty}; }

static fz_matrix fz_concat(fz_matrix left, fz_matrix right) {
    return {
        left.a * right.a + left.b * right.c, left.a * right.b + left.b * right.d,
        left.c * right.a + left.d * right.c, left.c * right.b + left.d * right.d,
        left.e * right.a + left.f * right.c + right.e,
        left.e * right.b + left.f * right.d + right.f
    };
}

static std::array<float, 4> fz_transform_rect(const std::array<float, 4>& r, fz_matrix m) {
    float x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3];
    float t_x0 = x0 * m.a + y0 * m.c + m.e, t_y0 = x0 * m.b + y0 * m.d + m.f;
    float t_x1 = x1 * m.a + y0 * m.c + m.e, t_y1 = x1 * m.b + y0 * m.d + m.f;
    float t_x2 = x0 * m.a + y1 * m.c + m.e, t_y2 = x0 * m.b + y1 * m.d + m.f;
    float t_x3 = x1 * m.a + y1 * m.c + m.e, t_y3 = x1 * m.b + y1 * m.d + m.f;
    return {
        std::min({t_x0, t_x1, t_x2, t_x3}), std::min({t_y0, t_y1, t_y2, t_y3}),
        std::max({t_x0, t_x1, t_x2, t_x3}), std::max({t_y0, t_y1, t_y2, t_y3})
    };
}

static std::array<float, 2> fz_transform_point(float x, float y, fz_matrix m) {
    return {x * m.a + y * m.c + m.e, x * m.b + y * m.d + m.f};
}

static std::array<float, 4> fz_intersect_rect(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    float x0 = std::max(a[0], b[0]), y0 = std::max(a[1], b[1]);
    float x1 = std::min(a[2], b[2]), y1 = std::min(a[3], b[3]);
    if (x1 < x0 || y1 < y0) return {0, 0, 0, 0};
    return {x0, y0, x1, y1};
}

static std::array<float, 4> QuadToBBox(const WinExtract::Quad& q) {
    return {
        std::min({q.ul.x, q.ur.x, q.ll.x, q.lr.x}), std::min({q.ul.y, q.ur.y, q.ll.y, q.lr.y}),
        std::max({q.ul.x, q.ur.x, q.ll.x, q.lr.x}), std::max({q.ul.y, q.ur.y, q.ll.y, q.lr.y})
    };
}

static fz_matrix ComputePageCTM(const WinExtract::WinPageGeometry& geo) {
    std::array<float, 4> mediabox = {geo.mediabox.x0, geo.mediabox.y0, geo.mediabox.x1, geo.mediabox.y1};
    std::array<float, 4> cropbox = {geo.cropbox.x0, geo.cropbox.y0, geo.cropbox.x1, geo.cropbox.y1};
    int rotate = geo.rotate;
    if (rotate < 0) rotate = 360 - ((-rotate) % 360);
    if (rotate >= 360) rotate = rotate % 360;
    rotate = 90 * ((rotate + 45) / 90);
    if (rotate >= 360) rotate = 0;

    fz_matrix ctm = fz_scale(1.0f, -1.0f);
    ctm = fz_pre_rotate(ctm, -static_cast<float>(rotate));
    cropbox = fz_intersect_rect(cropbox, mediabox);
    if (cropbox[2] - cropbox[0] < 1 || cropbox[3] - cropbox[1] < 1) cropbox = {0, 0, 1, 1};
    auto trans_cropbox = fz_transform_rect(cropbox, ctm);
    return fz_concat(ctm, fz_translate(-trans_cropbox[0], -trans_cropbox[1]));
}

static std::string Utf8FromCodepoint(int cp) {
    std::string out;
    if (cp <= 0 || cp > 0x10FFFF) return out;
    uint32_t u = static_cast<uint32_t>(cp);
    if (u <= 0x7F) { out.push_back(u); }
    else if (u <= 0x7FF) { out.push_back(0xC0 | ((u >> 6) & 0x1F)); out.push_back(0x80 | (u & 0x3F)); }
    else if (u <= 0xFFFF) { out.push_back(0xE0 | ((u >> 12) & 0x0F)); out.push_back(0x80 | ((u >> 6) & 0x3F)); out.push_back(0x80 | (u & 0x3F)); }
    else { out.push_back(0xF0 | ((u >> 18) & 0x07)); out.push_back(0x80 | ((u >> 12) & 0x3F)); out.push_back(0x80 | ((u >> 6) & 0x3F)); out.push_back(0x80 | (u & 0x3F)); }
    return out;
}

static void AppendUtf8Codepoint(std::string& out, int cp) {
    if (cp == 0x2028 || cp == 0x2029) cp = '\n';
    if (cp <= 0 || cp > 0x10FFFF) return;
    out += Utf8FromCodepoint(cp);
}

struct SpanCompat {
    std::string font_name;
    float font_size = 0, ascender = 0.8f, descender = -0.2f;
    uint32_t color = 0;
    bool is_bold = false, is_italic = false, is_serif = false, is_mono = false, is_synthetic = false, split_leading_spaces = false;
    int wmode = 0;
    WinExtract::Rect bbox{0, 0, 0, 0};
    std::vector<const WinExtract::WinChar*> chars;
};

static std::vector<SpanCompat> BuildLineSpans(const WinExtract::WinLine& line) {
    std::vector<SpanCompat> spans;
    for (const auto& ch : line.chars) {
        bool new_span = spans.empty() || spans.back().font_name != ch.font_name ||
                        std::abs(spans.back().font_size - ch.size) > 0.01f ||
                        spans.back().color != ch.color || spans.back().is_bold != ch.is_bold ||
                        spans.back().is_italic != ch.is_italic || spans.back().is_serif != ch.is_serif ||
                        spans.back().is_mono != ch.is_mono || spans.back().is_synthetic != ch.is_synthetic ||
                        spans.back().wmode != line.wmode;

        if (new_span) {
            SpanCompat sp;
            sp.font_name = ch.font_name; sp.font_size = ch.size; sp.color = ch.color;
            sp.is_bold = ch.is_bold; sp.is_italic = ch.is_italic; sp.is_serif = ch.is_serif;
            sp.is_mono = ch.is_mono; sp.is_synthetic = ch.is_synthetic; sp.ascender = ch.ascender;
            sp.descender = ch.descender; sp.wmode = line.wmode;
            spans.push_back(std::move(sp));
        }
        auto& sp = spans.back();
        sp.chars.push_back(&ch);
        auto cb = QuadToBBox(ch.quad);
        WinExtract::Rect cr{cb[0], cb[1], cb[2], cb[3]};
        if (sp.chars.size() == 1) sp.bbox = cr;
        else {
            sp.bbox.x0 = std::min(sp.bbox.x0, cr.x0); sp.bbox.y0 = std::min(sp.bbox.y0, cr.y0);
            sp.bbox.x1 = std::max(sp.bbox.x1, cr.x1); sp.bbox.y1 = std::max(sp.bbox.y1, cr.y1);
        }
    }
    return spans;
}

static int SpanFlags(const SpanCompat& s) {
    int f = 0;
    if (s.is_italic) f |= 2; if (s.is_serif) f |= 4; if (s.is_mono) f |= 8; if (s.is_bold) f |= 16;
    return f;
}

static std::string SpanText(const SpanCompat& s) {
    std::string t;
    for (const auto* ch : s.chars) if (ch) AppendUtf8Codepoint(t, ch->c);
    return t;
}

struct ExtractedPage { WinExtract::WinPage page; WinExtract::WinPageGeometry geo; };

static ExtractedPage ExtractTextPage(const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int page_index, bool /*sort*/=false) {
    auto stream = doc->get_page_content(page_index);
    auto fu = doc->get_page_font_unicode_map(page_index);
    auto fw = doc->get_page_font_width_map(page_index);
    auto fcb = doc->get_page_font_code_bytes_map(page_index);
    auto fcs = doc->get_page_font_codespace_map(page_index);
    auto fm = doc->get_page_font_matrix_map(page_index);
    auto fv = doc->get_page_font_vertical_metrics_map(page_index);
    auto fw2 = doc->get_page_font_w2_map(page_index);
    auto fc = doc->get_page_color_space_map(page_index);
    auto fx = doc->get_page_form_xobject_map(page_index);
    auto geo = doc->get_page_geometry(page_index);

    WinExtract::WinTextExtractor dev;
    dev.begin_page(geo.mediabox.x1 - geo.mediabox.x0, geo.mediabox.y1 - geo.mediabox.y0);
    WinExtract::WinPdfInterpreter::run(stream, dev, fu, fw, fcb, fcs, fm, fv, fw2, fc, fx,
                                       nullptr, 0, &geo.mediabox, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ExtractedPage res;
    res.page = dev.finish_page();
    res.geo = geo;
    return res;
}

static json ExtractedPageToJson(const ExtractedPage& extracted, int page_index, bool include_chars, bool sort_output) {
    fz_matrix ctm = ComputePageCTM(extracted.geo);
    std::array<float, 4> crop_arr = {extracted.geo.cropbox.x0, extracted.geo.cropbox.y0, extracted.geo.cropbox.x1, extracted.geo.cropbox.y1};
    std::array<float, 4> page_bbox = fz_transform_rect(crop_arr, ctm);

    float width = std::max(0.0f, page_bbox[2] - page_bbox[0]);
    float height = std::max(0.0f, page_bbox[3] - page_bbox[1]);

    json out;
    out["page_num"] = page_index;
    out["width"] = width;
    out["height"] = height;

    std::vector<const WinExtract::WinBlock*> sorted_blocks;
    for (const auto& block : extracted.page.blocks) sorted_blocks.push_back(&block);
    
    if (sort_output) {
        std::stable_sort(sorted_blocks.begin(), sorted_blocks.end(), [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b) {
            auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
            auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
            if (ab[3] != bb[3]) return ab[3] < bb[3];
            if (ab[0] != bb[0]) return ab[0] < bb[0];
            return false;
        });
    }

    json blocks_list = json::array();
    for (const auto* block_ptr : sorted_blocks) {
        const auto& block = *block_ptr;
        json block_dict;
        block_dict["type"] = (block.type == WinExtract::BlockType::TEXT) ? 0 : 1;
        auto bb = fz_transform_rect({block.bbox.x0, block.bbox.y0, block.bbox.x1, block.bbox.y1}, ctm);
        block_dict["bbox"] = {bb[0], bb[1], bb[2], bb[3]};

        json lines_list = json::array();
        for (const auto& line : block.lines) {
            json line_dict;
            auto lb = fz_transform_rect({line.bbox.x0, line.bbox.y0, line.bbox.x1, line.bbox.y1}, ctm);
            line_dict["bbox"] = {lb[0], lb[1], lb[2], lb[3]};
            line_dict["wmode"] = line.wmode;
            auto td = fz_transform_point(line.dir.x, line.dir.y, ctm);
            auto to = fz_transform_point(0, 0, ctm);
            line_dict["dir"] = {td[0] - to[0], td[1] - to[1]};

            const auto spans = BuildLineSpans(line);
            json spans_list = json::array();
            for (const auto& span : spans) {
                json span_dict;
                span_dict["text"] = SpanText(span);
                auto sb = fz_transform_rect({span.bbox.x0, span.bbox.y0, span.bbox.x1, span.bbox.y1}, ctm);
                span_dict["bbox"] = {sb[0], sb[1], sb[2], sb[3]};
                
                std::array<float, 2> origin;
                if (!span.chars.empty()) origin = fz_transform_point(span.chars.front()->origin.x, span.chars.front()->origin.y, ctm);
                else origin = fz_transform_point(span.bbox.x0, span.bbox.y0, ctm);
                span_dict["origin"] = {origin[0], origin[1]};

                span_dict["font"] = span.font_name; span_dict["size"] = span.font_size;
                span_dict["ascender"] = span.ascender; span_dict["descender"] = span.descender;
                span_dict["color"] = span.color; span_dict["flags"] = SpanFlags(span);

                if (include_chars) {
                    json chars_list = json::array();
                    for (const auto* ch : span.chars) {
                        if (!ch) continue;
                        json ch_dict;
                        ch_dict["c"] = Utf8FromCodepoint(ch->c);
                        ch_dict["u"] = ch->c;
                        auto co = fz_transform_point(ch->origin.x, ch->origin.y, ctm);
                        ch_dict["origin"] = {co[0], co[1]};
                        auto cb2 = fz_transform_rect(QuadToBBox(ch->quad), ctm);
                        ch_dict["bbox"] = {cb2[0], cb2[1], cb2[2], cb2[3]};
                        ch_dict["bidi"] = ch->bidi; ch_dict["wmode"] = span.wmode; ch_dict["flags"] = SpanFlags(span);
                        chars_list.push_back(std::move(ch_dict));
                    }
                    span_dict["chars"] = std::move(chars_list);
                }
                spans_list.push_back(std::move(span_dict));
            }
            line_dict["spans"] = std::move(spans_list);
            lines_list.push_back(std::move(line_dict));
        }
        block_dict["lines"] = std::move(lines_list);
        blocks_list.push_back(std::move(block_dict));
    }
    out["blocks"] = std::move(blocks_list);
    return out;
}

static std::string ExtractTextPlain(const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int page_index, bool sort_output) {
    const ExtractedPage ep = ExtractTextPage(doc, page_index, sort_output);
    fz_matrix ctm = ComputePageCTM(ep.geo);
    std::vector<const WinExtract::WinBlock*> sorted_blocks;
    for (const auto& block : ep.page.blocks) sorted_blocks.push_back(&block);
    
    if (sort_output) {
        std::stable_sort(sorted_blocks.begin(), sorted_blocks.end(), [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b) {
            auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
            auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
            if (ab[3] != bb[3]) return ab[3] < bb[3];
            if (ab[0] != bb[0]) return ab[0] < bb[0];
            return false;
        });
    }

    std::string text_out;
    for (const auto* bp : sorted_blocks) {
        if (bp->type != WinExtract::BlockType::TEXT) continue;
        for (const auto& line : bp->lines) {
            for (size_t ci = 0; ci < line.chars.size(); ++ci) {
                const auto& ch = line.chars[ci];
                if (line.joined && ci == line.chars.size() - 1 && ch.c == '-') continue;
                AppendUtf8Codepoint(text_out, ch.c);
            }
            if (!line.joined) text_out += "\n";
        }
    }
    return text_out;
}

// ─────────────────────────────────────────────────────────────────────────────
// WasmDocument - Lớp được Expose sang JS qua Embind
// ─────────────────────────────────────────────────────────────────────────────
class WasmDocument {
public:
    std::string path_;
    std::vector<uint8_t> mem_data_;
    std::shared_ptr<WinExtract::WinPdfDocument> doc_;

    WasmDocument(const emscripten::val& pdf_bytes_val) {
        mem_data_ = ValToVec(pdf_bytes_val);
        path_ = "<memory>";
        doc_ = WinExtract::WinPdfDocument::open_from_memory(mem_data_);
        if (!doc_) throw std::runtime_error("Cannot open PDF: invalid file or xref not found");
    }

    int pageCount() const { return doc_->count_pages(); }
    bool isEncrypted() const { return doc_->is_encrypted(); }
    void clearPageCache() { if (doc_) doc_->clear_page_cache(); }

    std::string pageRect(int i) const {
        checkPage(i);
        const auto geo = doc_->get_page_geometry(i);
        fz_matrix ctm = ComputePageCTM(geo);
        auto cb = fz_transform_rect({geo.cropbox.x0, geo.cropbox.y0, geo.cropbox.x1, geo.cropbox.y1}, ctm);
        float w = std::max(0.0f, cb[2] - cb[0]), h = std::max(0.0f, cb[3] - cb[1]);
        return json{0.0f, 0.0f, w, h}.dump();
    }

    std::string getTextPlain(int i, bool sort = false) const {
        checkPage(i);
        return ExtractTextPlain(doc_, i, sort);
    }

    std::string getDict(int i, bool sort = false) const {
        checkPage(i);
        const auto ep = ExtractTextPage(doc_, i, sort);
        return ExtractedPageToJson(ep, i, false, sort).dump();
    }

    std::string getRawDict(int i, bool sort = false) const {
        checkPage(i);
        const auto ep = ExtractTextPage(doc_, i, sort);
        return ExtractedPageToJson(ep, i, true, sort).dump();
    }

    std::string getBlocks(int i, bool sort = false) const {
        checkPage(i);
        const auto ep = ExtractTextPage(doc_, i, sort);
        fz_matrix ctm = ComputePageCTM(ep.geo);
        std::vector<const WinExtract::WinBlock*> blks;
        for (const auto& b : ep.page.blocks) blks.push_back(&b);
        if (sort) {
            std::stable_sort(blks.begin(), blks.end(), [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b) {
                auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
                auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
                if (std::abs(ab[3] - bb[3]) > 0.01f) return ab[3] > bb[3];
                if (std::abs(ab[0] - bb[0]) > 0.01f) return ab[0] < bb[0];
                return ab[1] > bb[1];
            });
        }
        json out = json::array();
        for (size_t idx = 0; idx < blks.size(); ++idx) {
            const auto* bp = blks[idx];
            std::string txt;
            for (const auto& line : bp->lines) {
                const auto spans = BuildLineSpans(line);
                for (const auto& sp : spans) for (const auto* ch : sp.chars) if (ch) AppendUtf8Codepoint(txt, ch->c);
                txt += "\n";
            }
            auto bb = fz_transform_rect({bp->bbox.x0, bp->bbox.y0, bp->bbox.x1, bp->bbox.y1}, ctm);
            json bd;
            bd["bbox"] = {bb[0], bb[1], bb[2], bb[3]};
            bd["text"] = txt;
            bd["type"] = (bp->type == WinExtract::BlockType::TEXT) ? 0 : 1;
            bd["block_no"] = idx;
            out.push_back(std::move(bd));
        }
        return out.dump();
    }

    std::string extractText(int i) const {
        checkPage(i);
        const auto ep = ExtractTextPage(doc_, i);
        WinExtract::WinTextExtractor dev;
        return dev.get_text(ep.page);
    }

    // ĐƠN LUỒNG
    std::string getAllText() const {
        int n = doc_->count_pages();
        std::string final_res;
        final_res.reserve(n * 2048);
        for (int i = 0; i < n; ++i) {
            final_res += ExtractTextPlain(doc_, i, false);
            if (i < n - 1) final_res += "\x0C";
        }
        return final_res;
    }

    std::string getJson(int i, bool include_chars = false, bool sort = false) const {
        checkPage(i);
        return ExtractedPageToJson(ExtractTextPage(doc_, i, sort), i, include_chars, sort).dump();
    }

    // ĐƠN LUỒNG
    std::string getAllDictsJson(bool include_chars = false, bool sort = false) const {
        int n = doc_->count_pages();
        std::string final_res = "[";
        for (int i = 0; i < n; ++i) {
            final_res += ExtractedPageToJson(ExtractTextPage(doc_, i, sort), i, include_chars, sort).dump();
            if (i < n - 1) final_res += ",";
        }
        final_res += "]";
        return final_res;
    }

    std::string getPageFontBasenames(int i) const {
        checkPage(i);
        const auto mm = doc_->get_page_font_vertical_metrics_map(i);
        json out = json::object();
        for (const auto& kv : mm) if (!kv.second.base_font.empty()) out[kv.first] = kv.second.base_font;
        return out.dump();
    }

    emscripten::val getDrawings(int i) const {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
        // PDFium requires init
        static bool pdfium_initialized = false;
        if (!pdfium_initialized) {
            FPDF_InitLibrary();
            pdfium_initialized = true;
        }

        FPDF_DOCUMENT pdfdoc = FPDF_LoadMemDocument64(mem_data_.data(), mem_data_.size(), "");
        if (!pdfdoc) return emscripten::val::array();

        FPDF_PAGE page = FPDF_LoadPage(pdfdoc, i);
        if (!page) { FPDF_CloseDocument(pdfdoc); return emscripten::val::array(); }

        const auto drawings = Winnerz::GetDrawingsFromPdfium(page);
        emscripten::val out = emscripten::val::array();
        size_t idx = 0;
        for (const auto& d : drawings) {
            emscripten::val item = emscripten::val::object();
            item.set("type", d.type);
            item.set("scissor_clip", emscripten::val::array(std::vector<float>{d.scissor_clip.x0, d.scissor_clip.y0, d.scissor_clip.x1, d.scissor_clip.y1}));
            
            if (!d.fill_color.components.empty()) {
                emscripten::val fc = emscripten::val::object();
                fc.set("components", emscripten::val::array(d.fill_color.components));
                fc.set("alpha", d.fill_color.alpha);
                item.set("fill_color", fc);
            }
            if (!d.stroke_color.components.empty()) {
                emscripten::val sc = emscripten::val::object();
                sc.set("components", emscripten::val::array(d.stroke_color.components));
                sc.set("alpha", d.stroke_color.alpha);
                item.set("stroke_color", sc);
            }
            out.set(idx++, item);
        }
        FPDF_ClosePage(page);
        FPDF_CloseDocument(pdfdoc);
        return out;
#else
        (void)i;
        return emscripten::val::array();
#endif
    }

    emscripten::val renderPage(int page_index, float scale, emscripten::val clip_val) {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
        if (scale <= 0.0f) throw std::runtime_error("scale must be > 0");
        std::array<float, 4> clip = {0, 0, 0, 0};
        const std::array<float, 4>* clip_ptr = nullptr;
        if (!clip_val.isUndefined() && !clip_val.isNull()) {
            auto vec = emscripten::vecFromJSArray<float>(clip_val);
            if(vec.size() >= 4) { clip = {vec[0], vec[1], vec[2], vec[3]}; clip_ptr = &clip; }
        }

        winnerz::PreviewImage preview;
        std::string error_message;
        if (!winnerz::RenderPdfPagePreview(path_, mem_data_, page_index, scale, clip_ptr, preview, &error_message)) {
            throw std::runtime_error("renderPage failed: " + error_message);
        }
        
        emscripten::val out = emscripten::val::object();
        out.set("width", preview.width);
        out.set("height", preview.height);
        out.set("channels", preview.channels);
        out.set("stride", preview.stride);
        out.set("samples", VecToVal(preview.rgba));
        return out;
#else
        (void)page_index; (void)scale; (void)clip_val;
        throw std::runtime_error("renderPage requires PDFium");
#endif
    }

    // ĐƠN LUỒNG
    emscripten::val redactPagesBytes(const std::string& page_rects_json) const {
        json j;
        try { j = json::parse(page_rects_json); }
        catch(const std::exception& e) { throw std::runtime_error(std::string("JSON parse error: ") + e.what()); }

        std::map<int, std::vector<uint8_t>> pages_streams;
        std::map<int, std::vector<uint8_t>> updated_xobjects;

        for (auto& [key, rects_arr] : j.items()) {
            int page_index = std::stoi(key);
            
            std::vector<winnerz::WinRedactZone_TopDown> zones;
            for (const auto& r : rects_arr) {
                winnerz::WinRedactZone_TopDown z;
                z.rect = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
                zones.push_back(z);
            }

            fz_matrix ctm = ComputePageCTM(doc_->get_page_geometry(page_index));
            float ctm_arr[6] = {ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f};
            std::map<int, std::vector<uint8_t>> page_updated_xobjects;
            const auto filtered = winnerz::WinnerZ_RedactPage(*doc_, page_index, zones, page_updated_xobjects, ctm_arr);

            pages_streams[page_index] = filtered;
            for (const auto& kv : page_updated_xobjects) updated_xobjects[kv.first] = kv.second;
        }

        auto out_bytes = doc_->save_multiple_pages_content_incremental_to_bytes(pages_streams, updated_xobjects);
        if (out_bytes.empty()) throw std::runtime_error("Redaction failed");
        return VecToVal(out_bytes);
    }

    emscripten::val redactRects(int page_index, const std::string& rects_json) const {
        checkPage(page_index);
        json j;
        try { j = json::parse(rects_json); }
        catch(const std::exception& e) { throw std::runtime_error(std::string("JSON parse error: ") + e.what()); }

        std::vector<winnerz::WinRedactZone_TopDown> zones;
        for (const auto& r : j) {
            winnerz::WinRedactZone_TopDown z;
            z.rect = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
            zones.push_back(z);
        }

        fz_matrix ctm = ComputePageCTM(doc_->get_page_geometry(page_index));
        float ctm_arr[6] = {ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f};
        std::map<int, std::vector<uint8_t>> updated_xobjects;
        const auto filtered = winnerz::WinnerZ_RedactPage(*doc_, page_index, zones, updated_xobjects, ctm_arr);
        
        std::map<int, std::vector<uint8_t>> pages_streams;
        pages_streams[page_index] = filtered;
        auto out_bytes = doc_->save_multiple_pages_content_incremental_to_bytes(pages_streams, updated_xobjects);
        return VecToVal(out_bytes);
    }

    emscripten::val insertTextToPagesJson(const std::string& json_str, const std::string& fonts_dir) const {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
        std::map<int, std::vector<Winnerz::WinInsertTextTask>> pages_tasks;
        try {
            json j = json::parse(json_str);
            for (auto& item : j.items()) {
                int page_index = std::stoi(item.key());
                std::vector<Winnerz::WinInsertTextTask> tasks;
                for (auto& task_item : item.value()) {
                    Winnerz::WinInsertTextTask task;
                    task.text = task_item.value("text", "");
                    auto rect = task_item["rect"];
                    task.x0 = rect[0].get<double>(); task.y0 = rect[1].get<double>();
                    task.x1 = rect[2].get<double>(); task.y1 = rect[3].get<double>();
                    task.font_size = task_item.value("size", 12.0);
                    auto color = task_item["color"];
                    task.r = color[0].get<int>(); task.g = color[1].get<int>(); task.b = color[2].get<int>();
                    task.bold = task_item.value("bold", false);
                    task.italic = task_item.value("italic", false);
                    task.multiline = task_item.value("multiline", false);
                    task.font_family = task_item.value("font_family", "");
                    tasks.push_back(task);
                }
                pages_tasks[page_index] = tasks;
            }
        } catch (const std::exception& e) { throw std::runtime_error(std::string("JSON parse error: ") + e.what()); }

        std::vector<uint8_t> merged_bytes = Winnerz::InsertTextToMultiplePages(doc_.get(), pages_tasks, fonts_dir, nullptr, 0);
        if (merged_bytes.empty()) return VecToVal(mem_data_); // Fallback to original
        return VecToVal(merged_bytes);
#else
        (void)json_str; (void)fonts_dir;
        throw std::runtime_error("insertTextToPagesJson requires PDFium");
#endif
    }

    emscripten::val insertTextToPagesFitSpacingJson(const std::string& json_str, const std::string& fonts_dir) const {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
        // Same logic as above, just calling InsertTextToMultiplePagesFitSpacing
        std::map<int, std::vector<Winnerz::WinInsertTextTask>> pages_tasks;
        try {
            json j = json::parse(json_str);
            for (auto& item : j.items()) {
                int page_index = std::stoi(item.key());
                std::vector<Winnerz::WinInsertTextTask> tasks;
                for (auto& task_item : item.value()) {
                    Winnerz::WinInsertTextTask task;
                    task.text = task_item.value("text", "");
                    auto rect = task_item["rect"];
                    task.x0 = rect[0].get<double>(); task.y0 = rect[1].get<double>();
                    task.x1 = rect[2].get<double>(); task.y1 = rect[3].get<double>();
                    task.font_size = task_item.value("size", 12.0);
                    auto color = task_item["color"];
                    task.r = color[0].get<int>(); task.g = color[1].get<int>(); task.b = color[2].get<int>();
                    task.bold = task_item.value("bold", false);
                    task.italic = task_item.value("italic", false);
                    task.multiline = task_item.value("multiline", false);
                    task.font_family = task_item.value("font_family", "");
                    tasks.push_back(task);
                }
                pages_tasks[page_index] = tasks;
            }
        } catch (const std::exception& e) { throw std::runtime_error(std::string("JSON parse error: ") + e.what()); }

        std::vector<uint8_t> merged_bytes = Winnerz::InsertTextToMultiplePagesFitSpacing(doc_.get(), pages_tasks, fonts_dir, nullptr, 0);
        if (merged_bytes.empty()) return VecToVal(mem_data_);
        return VecToVal(merged_bytes);
#else
        (void)json_str; (void)fonts_dir;
        throw std::runtime_error("insertTextToPagesFitSpacingJson requires PDFium");
#endif
    }

    emscripten::val insertRectsToPagesJson(const std::string& json_str) const {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
        std::map<int, std::vector<Winnerz::WinInsertRectTask>> pages_tasks;
        try {
            json j = json::parse(json_str);
            for (auto& item : j.items()) {
                int page_index = std::stoi(item.key());
                std::vector<Winnerz::WinInsertRectTask> tasks;
                for (auto& task_item : item.value()) {
                    Winnerz::WinInsertRectTask task;
                    auto rect = task_item["rect"];
                    float pad = task_item.value("pad", 2.0f);
                    task.x0 = rect[0].get<float>() - pad; task.y0 = rect[1].get<float>() - pad;
                    task.x1 = rect[2].get<float>() + pad; task.y1 = rect[3].get<float>() + pad;
                    auto color = task_item["color"];
                    task.r = color[0].get<int>(); task.g = color[1].get<int>(); task.b = color[2].get<int>();
                    tasks.push_back(task);
                }
                pages_tasks[page_index] = tasks;
            }
        } catch (const std::exception& e) { throw std::runtime_error(std::string("JSON parse error: ") + e.what()); }

        std::vector<uint8_t> merged_bytes = Winnerz::InsertRectsToMultiplePages(doc_.get(), pages_tasks, nullptr, 0);
        if (merged_bytes.empty()) return emscripten::val::null();
        return VecToVal(merged_bytes);
#else
        (void)json_str;
        throw std::runtime_error("insertRectsToPagesJson requires PDFium");
#endif
    }

private:
    void checkPage(int i) const {
        if (i < 0 || i >= doc_->count_pages()) throw std::runtime_error("page_index out of range");
    }
};

float MeasureTextWidthWasm(const std::string& text, const std::string& font_path, float font_size, bool is_bold, bool is_italic) {
    return Winnerz::MeasureTextWidth(text, font_path, font_size, is_bold, is_italic);
}

} // namespace

EMSCRIPTEN_BINDINGS(winnerz) {
    function("measureTextWidth", &MeasureTextWidthWasm);

    class_<WasmDocument>("Document")
        .constructor<emscripten::val>()
        .function("pageCount", &WasmDocument::pageCount)
        .function("isEncrypted", &WasmDocument::isEncrypted)
        .function("clearPageCache", &WasmDocument::clearPageCache)
        .function("pageRect", &WasmDocument::pageRect)
        .function("getTextPlain", &WasmDocument::getTextPlain)
        .function("getDict", &WasmDocument::getDict)
        .function("getRawDict", &WasmDocument::getRawDict)
        .function("getBlocks", &WasmDocument::getBlocks)
        .function("extractText", &WasmDocument::extractText)
        .function("getAllText", &WasmDocument::getAllText)
        .function("getJson", &WasmDocument::getJson)
        .function("getAllDictsJson", &WasmDocument::getAllDictsJson)
        .function("getPageFontBasenames", &WasmDocument::getPageFontBasenames)
        .function("getDrawings", &WasmDocument::getDrawings)
        .function("renderPage", &WasmDocument::renderPage)
        .function("redactPagesBytes", &WasmDocument::redactPagesBytes)
        .function("redactRects", &WasmDocument::redactRects)
        .function("insertTextToPagesJson", &WasmDocument::insertTextToPagesJson)
        .function("insertTextToPagesFitSpacingJson", &WasmDocument::insertTextToPagesFitSpacingJson)
        .function("insertRectsToPagesJson", &WasmDocument::insertRectsToPagesJson);
}

#endif // __EMSCRIPTEN__