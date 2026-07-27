// winnerz_wasm_embind.cpp
// Emscripten/Embind wrapper for WinnerZ WASM build.
// Compiled ONLY when EMSCRIPTEN is defined (emcmake cmake).
//
// APIs exposed (mirrors Python winnerz API as closely as possible):
//   WasmDocument:
//     - pageCount(), isEncrypted(), pageRect(i)
//     - getTextPlain(i, sort), getJson(i, includeChars, sort)
//     - getRawJson(i, sort), getBlocksJson(i, sort)
//     - getAllText(), getAllDictsJson(includeChars, sort)
//     - getPageFontBasenames(i) -> JSON
//     - redactPage(i, rectsJson) -> Uint8Array bytes
//     - redactPages(pageRectsMapJson) -> Uint8Array bytes
//     - insertTextJson(jsonStr, fontsBytesMapJson) -> Uint8Array bytes
//     - insertRectsJson(jsonStr) -> Uint8Array bytes
//     - clearPageCache()
//
// Build (in WSL after source ~/emsdk/emsdk_env.sh):
//   mkdir -p build-wasm && cd build-wasm
//   emcmake cmake .. -DWINNERZ_BUILD_PYTHON_WRAPPER=OFF
//   emmake make winnerz_wasm -j$(nproc)

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

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "extract_text/extractor_logic.hpp"
#include "extract_text/pdf_engine.hpp"
#include "redactor/redactor.hpp"

// insert.hpp includes fpdfview.h only for its header types, but insert.cpp
// never calls any FPDF_* function, so we can safely include it in WASM builds.
#define WINNERZ_WASM_BUILD 1
#include "insert_text/insert.hpp"

#include "nlohmann/json.hpp"

using json = nlohmann::json;
using namespace emscripten;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers – fz_matrix (same as Python wrapper, no pybind11 needed)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct fz_matrix { float a, b, c, d, e, f; };

static fz_matrix fz_scale(float sx, float sy) { return {sx,0,0,sy,0,0}; }

static fz_matrix fz_pre_rotate(fz_matrix m, float deg) {
    float a = deg * 3.14159265358979323846f / 180.0f;
    float s = std::sin(a), c = std::cos(a);
    fz_matrix r = {c,s,-s,c,0,0};
    return { r.a*m.a+r.b*m.c, r.a*m.b+r.b*m.d,
             r.c*m.a+r.d*m.c, r.c*m.b+r.d*m.d, m.e, m.f };
}

static fz_matrix fz_translate(float tx, float ty) { return {1,0,0,1,tx,ty}; }

static fz_matrix fz_concat(fz_matrix l, fz_matrix r) {
    return { l.a*r.a+l.b*r.c, l.a*r.b+l.b*r.d,
             l.c*r.a+l.d*r.c, l.c*r.b+l.d*r.d,
             l.e*r.a+l.f*r.c+r.e, l.e*r.b+l.f*r.d+r.f };
}

static std::array<float,4> fz_transform_rect(const std::array<float,4>& r, fz_matrix m) {
    float pts[4][2] = {
        {r[0]*m.a+r[1]*m.c+m.e, r[0]*m.b+r[1]*m.d+m.f},
        {r[2]*m.a+r[1]*m.c+m.e, r[2]*m.b+r[1]*m.d+m.f},
        {r[0]*m.a+r[3]*m.c+m.e, r[0]*m.b+r[3]*m.d+m.f},
        {r[2]*m.a+r[3]*m.c+m.e, r[2]*m.b+r[3]*m.d+m.f},
    };
    float nx0=pts[0][0], ny0=pts[0][1], nx1=pts[0][0], ny1=pts[0][1];
    for (int i=1;i<4;++i) {
        nx0=std::min(nx0,pts[i][0]); ny0=std::min(ny0,pts[i][1]);
        nx1=std::max(nx1,pts[i][0]); ny1=std::max(ny1,pts[i][1]);
    }
    return {nx0,ny0,nx1,ny1};
}

static std::array<float,2> fz_transform_point(float x, float y, fz_matrix m) {
    return {x*m.a+y*m.c+m.e, x*m.b+y*m.d+m.f};
}

static std::array<float,4> fz_intersect_rect(const std::array<float,4>& a, const std::array<float,4>& b) {
    float x0=std::max(a[0],b[0]), y0=std::max(a[1],b[1]);
    float x1=std::min(a[2],b[2]), y1=std::min(a[3],b[3]);
    if (x1<x0||y1<y0) return {0,0,0,0};
    return {x0,y0,x1,y1};
}

static std::array<float,4> QuadToBBox(const WinExtract::Quad& q) {
    return { std::min({q.ul.x,q.ur.x,q.ll.x,q.lr.x}),
             std::min({q.ul.y,q.ur.y,q.ll.y,q.lr.y}),
             std::max({q.ul.x,q.ur.x,q.ll.x,q.lr.x}),
             std::max({q.ul.y,q.ur.y,q.ll.y,q.lr.y}) };
}

static fz_matrix ComputePageCTM(const WinExtract::WinPageGeometry& geo) {
    std::array<float,4> mb={geo.mediabox.x0,geo.mediabox.y0,geo.mediabox.x1,geo.mediabox.y1};
    std::array<float,4> cb={geo.cropbox.x0, geo.cropbox.y0, geo.cropbox.x1, geo.cropbox.y1};

    int rot = geo.rotate;
    if (rot<0) rot=360-((-rot)%360);
    if (rot>=360) rot=rot%360;
    rot=90*((rot+45)/90);
    if (rot>=360) rot=0;

    fz_matrix ctm = fz_scale(1.0f,-1.0f);
    ctm = fz_pre_rotate(ctm, -static_cast<float>(rot));
    cb  = fz_intersect_rect(cb, mb);
    if (cb[2]-cb[0]<1||cb[3]-cb[1]<1) cb={0,0,1,1};
    auto tcb = fz_transform_rect(cb, ctm);
    ctm = fz_concat(ctm, fz_translate(-tcb[0], -tcb[1]));
    return ctm;
}

// ─────────────────────────────────────────────────────────────────────────────
// UTF-8 helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string Utf8FromCP(int cp) {
    std::string o;
    if (cp<=0||cp>0x10FFFF) return o;
    uint32_t u=static_cast<uint32_t>(cp);
    if (u<=0x7F) { o.push_back(u); }
    else if (u<=0x7FF) { o.push_back(0xC0|((u>>6)&0x1F)); o.push_back(0x80|(u&0x3F)); }
    else if (u<=0xFFFF) { o.push_back(0xE0|((u>>12)&0x0F)); o.push_back(0x80|((u>>6)&0x3F)); o.push_back(0x80|(u&0x3F)); }
    else { o.push_back(0xF0|((u>>18)&0x07)); o.push_back(0x80|((u>>12)&0x3F)); o.push_back(0x80|((u>>6)&0x3F)); o.push_back(0x80|(u&0x3F)); }
    return o;
}

static void AppendUTF8(std::string& o, int cp) {
    if (cp==0x2028||cp==0x2029) cp='\n';
    if (cp<=0||cp>0x10FFFF) return;
    o+=Utf8FromCP(cp);
}

// ─────────────────────────────────────────────────────────────────────────────
// Span logic
// ─────────────────────────────────────────────────────────────────────────────

struct SpanCompat {
    std::string font_name;
    float font_size=0, ascender=0.8f, descender=-0.2f;
    uint32_t color=0;
    bool is_bold=false, is_italic=false, is_serif=false, is_mono=false;
    int wmode=0;
    WinExtract::Rect bbox{0,0,0,0};
    std::vector<const WinExtract::WinChar*> chars;
};

static std::vector<SpanCompat> BuildLineSpans(const WinExtract::WinLine& line) {
    std::vector<SpanCompat> spans;
    for (const auto& ch : line.chars) {
        bool ns = spans.empty()
            || spans.back().font_name!=ch.font_name
            || std::abs(spans.back().font_size-ch.size)>0.01f
            || spans.back().color!=ch.color
            || spans.back().is_bold!=ch.is_bold
            || spans.back().is_italic!=ch.is_italic
            || spans.back().is_serif!=ch.is_serif
            || spans.back().is_mono!=ch.is_mono
            || spans.back().wmode!=line.wmode;
        if (ns) {
            SpanCompat sp;
            sp.font_name=ch.font_name; sp.font_size=ch.size; sp.color=ch.color;
            sp.is_bold=ch.is_bold; sp.is_italic=ch.is_italic;
            sp.is_serif=ch.is_serif; sp.is_mono=ch.is_mono;
            sp.ascender=ch.ascender; sp.descender=ch.descender; sp.wmode=line.wmode;
            spans.push_back(std::move(sp));
        }
        auto& sp = spans.back();
        sp.chars.push_back(&ch);
        auto cb = QuadToBBox(ch.quad);
        WinExtract::Rect cr{cb[0],cb[1],cb[2],cb[3]};
        if (sp.chars.size()==1) { sp.bbox=cr; }
        else {
            sp.bbox.x0=std::min(sp.bbox.x0,cr.x0); sp.bbox.y0=std::min(sp.bbox.y0,cr.y0);
            sp.bbox.x1=std::max(sp.bbox.x1,cr.x1); sp.bbox.y1=std::max(sp.bbox.y1,cr.y1);
        }
    }
    return spans;
}

static int SpanFlags(const SpanCompat& s) {
    int f=0;
    if (s.is_italic) f|=2; if (s.is_serif) f|=4; if (s.is_mono) f|=8; if (s.is_bold) f|=16;
    return f;
}

static std::string SpanText(const SpanCompat& s) {
    std::string t;
    for (const auto* ch : s.chars) if (ch) AppendUTF8(t, ch->c);
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Page extraction
// ─────────────────────────────────────────────────────────────────────────────

struct ExtractedPage { WinExtract::WinPage page; WinExtract::WinPageGeometry geo; };

static ExtractedPage DoExtractPage(
    const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int idx, bool /*sort*/)
{
    auto stream         = doc->get_page_content(idx);
    auto fu             = doc->get_page_font_unicode_map(idx);
    auto fw             = doc->get_page_font_width_map(idx);
    auto fcb            = doc->get_page_font_code_bytes_map(idx);
    auto fcs            = doc->get_page_font_codespace_map(idx);
    auto fm             = doc->get_page_font_matrix_map(idx);
    auto fv             = doc->get_page_font_vertical_metrics_map(idx);
    auto fw2            = doc->get_page_font_w2_map(idx);
    auto fc             = doc->get_page_color_space_map(idx);
    auto fx             = doc->get_page_form_xobject_map(idx);
    auto geo            = doc->get_page_geometry(idx);

    WinExtract::WinTextExtractor dev;
    dev.begin_page(geo.mediabox.x1-geo.mediabox.x0, geo.mediabox.y1-geo.mediabox.y0);
    WinExtract::WinPdfInterpreter::run(
        stream, dev, fu, fw, fcb, fcs, fm, fv, fw2, fc, fx,
        nullptr, 0, &geo.mediabox, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    );
    ExtractedPage res;
    res.page = dev.finish_page();
    res.geo  = geo;
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON serialisation
// ─────────────────────────────────────────────────────────────────────────────

static std::string PageToJsonStr(
    const ExtractedPage& ep, int idx, bool include_chars, bool sort)
{
    fz_matrix ctm = ComputePageCTM(ep.geo);
    std::array<float,4> ca={ep.geo.cropbox.x0,ep.geo.cropbox.y0,ep.geo.cropbox.x1,ep.geo.cropbox.y1};
    auto pb = fz_transform_rect(ca, ctm);
    float W=std::max(0.0f,pb[2]-pb[0]), H=std::max(0.0f,pb[3]-pb[1]);

    json out; out["page_num"]=idx; out["width"]=W; out["height"]=H;

    std::vector<const WinExtract::WinBlock*> blks;
    for (const auto& b:ep.page.blocks) blks.push_back(&b);
    if (sort) {
        std::stable_sort(blks.begin(), blks.end(),
            [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b){
                auto ab=fz_transform_rect({a->bbox.x0,a->bbox.y0,a->bbox.x1,a->bbox.y1},ctm);
                auto bb=fz_transform_rect({b->bbox.x0,b->bbox.y0,b->bbox.x1,b->bbox.y1},ctm);
                if (ab[3]!=bb[3]) return ab[3]<bb[3];
                if (ab[0]!=bb[0]) return ab[0]<bb[0];
                return false;
            });
    }

    json ba=json::array();
    for (const auto* bp : blks) {
        const auto& blk=*bp;
        json bd; bd["type"]=(blk.type==WinExtract::BlockType::TEXT)?0:1;
        auto bb=fz_transform_rect({blk.bbox.x0,blk.bbox.y0,blk.bbox.x1,blk.bbox.y1},ctm);
        bd["bbox"]={bb[0],bb[1],bb[2],bb[3]};
        json la=json::array();
        for (const auto& line:blk.lines) {
            json ld;
            auto lb=fz_transform_rect({line.bbox.x0,line.bbox.y0,line.bbox.x1,line.bbox.y1},ctm);
            ld["bbox"]={lb[0],lb[1],lb[2],lb[3]}; ld["wmode"]=line.wmode;
            auto td=fz_transform_point(line.dir.x,line.dir.y,ctm);
            auto to=fz_transform_point(0,0,ctm);
            ld["dir"]={td[0]-to[0],td[1]-to[1]};
            const auto spans=BuildLineSpans(line);
            json sa=json::array();
            for (const auto& sp:spans) {
                json sd; sd["text"]=SpanText(sp);
                auto sb=fz_transform_rect({sp.bbox.x0,sp.bbox.y0,sp.bbox.x1,sp.bbox.y1},ctm);
                sd["bbox"]={sb[0],sb[1],sb[2],sb[3]};
                std::array<float,2> orig;
                if (!sp.chars.empty()) orig=fz_transform_point(sp.chars.front()->origin.x,sp.chars.front()->origin.y,ctm);
                else orig=fz_transform_point(sp.bbox.x0,sp.bbox.y0,ctm);
                sd["origin"]={orig[0],orig[1]};
                sd["font"]=sp.font_name; sd["size"]=sp.font_size;
                sd["ascender"]=sp.ascender; sd["descender"]=sp.descender;
                sd["color"]=sp.color; sd["flags"]=SpanFlags(sp);
                if (include_chars) {
                    json ca2=json::array();
                    for (const auto* ch:sp.chars) {
                        if (!ch) continue;
                        json cd; cd["c"]=Utf8FromCP(ch->c); cd["u"]=ch->c;
                        auto co=fz_transform_point(ch->origin.x,ch->origin.y,ctm);
                        cd["origin"]={co[0],co[1]};
                        auto q=QuadToBBox(ch->quad);
                        auto cb2=fz_transform_rect(q,ctm);
                        cd["bbox"]={cb2[0],cb2[1],cb2[2],cb2[3]};
                        cd["bidi"]=ch->bidi; cd["wmode"]=sp.wmode; cd["flags"]=SpanFlags(sp);
                        ca2.push_back(std::move(cd));
                    }
                    sd["chars"]=std::move(ca2);
                }
                sa.push_back(std::move(sd));
            }
            ld["spans"]=std::move(sa);
            la.push_back(std::move(ld));
        }
        bd["lines"]=std::move(la);
        ba.push_back(std::move(bd));
    }
    out["blocks"]=std::move(ba);
    return out.dump();
}

static std::string ExtractTextPlain(
    const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int idx, bool sort)
{
    const auto ep = DoExtractPage(doc, idx, sort);
    fz_matrix ctm = ComputePageCTM(ep.geo);
    std::vector<const WinExtract::WinBlock*> blks;
    for (const auto& b:ep.page.blocks) blks.push_back(&b);
    if (sort) {
        std::stable_sort(blks.begin(), blks.end(),
            [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b){
                auto ab=fz_transform_rect({a->bbox.x0,a->bbox.y0,a->bbox.x1,a->bbox.y1},ctm);
                auto bb=fz_transform_rect({b->bbox.x0,b->bbox.y0,b->bbox.x1,b->bbox.y1},ctm);
                if (ab[3]!=bb[3]) return ab[3]<bb[3];
                if (ab[0]!=bb[0]) return ab[0]<bb[0];
                return false;
            });
    }
    std::string out;
    for (const auto* bp:blks) {
        const auto& blk=*bp;
        if (blk.type!=WinExtract::BlockType::TEXT) continue;
        for (const auto& line:blk.lines) {
            for (size_t ci=0;ci<line.chars.size();++ci) {
                const auto& ch=line.chars[ci];
                if (line.joined&&ci==line.chars.size()-1&&ch.c=='-') continue;
                AppendUTF8(out,ch.c);
            }
            if (!line.joined) out+="\n";
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// std::string ↔ std::vector<uint8_t> helpers for WASM binary transfer
// Embind maps std::string as binary string in JS
// ─────────────────────────────────────────────────────────────────────────────

static std::string VecToStr(const std::vector<uint8_t>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

static std::vector<uint8_t> StrToVec(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// WasmDocument – main class exposed to JS
// ─────────────────────────────────────────────────────────────────────────────

class WasmDocument {
public:
    explicit WasmDocument(const std::string& pdf_bytes_str) {
        mem_ = StrToVec(pdf_bytes_str);
        doc_ = WinExtract::WinPdfDocument::open_from_memory(mem_);
        if (!doc_) throw std::runtime_error("Cannot open PDF: invalid file or xref not found");
    }

    // ── Basic info ────────────────────────────────────────────────────────────
    int  page_count()  const { return doc_->count_pages(); }
    bool is_encrypted()const { return doc_->is_encrypted(); }

    // Returns "[x0, y0, x1, y1]" JSON
    std::string page_rect(int i) const {
        chk(i);
        const auto geo=doc_->get_page_geometry(i);
        fz_matrix ctm=ComputePageCTM(geo);
        auto cb=fz_transform_rect({geo.cropbox.x0,geo.cropbox.y0,geo.cropbox.x1,geo.cropbox.y1},ctm);
        float w=std::max(0.0f,cb[2]-cb[0]), h=std::max(0.0f,cb[3]-cb[1]);
        return json{0.0f,0.0f,w,h}.dump();
    }

    // ── Text extraction ───────────────────────────────────────────────────────
    std::string get_text_plain(int i, bool sort=false) const {
        chk(i); return ExtractTextPlain(doc_, i, sort);
    }

    // Python: page.get_text("json") / page.get_text("rawjson")
    std::string get_json(int i, bool include_chars=false, bool sort=false) const {
        chk(i);
        const auto ep=DoExtractPage(doc_, i, sort);
        return PageToJsonStr(ep, i, include_chars, sort);
    }
    std::string get_rawjson(int i, bool sort=false) const { return get_json(i, true, sort); }

    // Python: page.get_text("blocks")
    std::string get_blocks_json(int i, bool sort=false) const {
        chk(i);
        const auto ep=DoExtractPage(doc_, i, sort);
        fz_matrix ctm=ComputePageCTM(ep.geo);
        std::vector<const WinExtract::WinBlock*> blks;
        for (const auto& b:ep.page.blocks) blks.push_back(&b);
        if (sort) {
            std::stable_sort(blks.begin(),blks.end(),
                [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b){
                    auto ab=fz_transform_rect({a->bbox.x0,a->bbox.y0,a->bbox.x1,a->bbox.y1},ctm);
                    auto bb=fz_transform_rect({b->bbox.x0,b->bbox.y0,b->bbox.x1,b->bbox.y1},ctm);
                    if (std::abs(ab[3]-bb[3])>0.01f) return ab[3]>bb[3];
                    if (std::abs(ab[0]-bb[0])>0.01f) return ab[0]<bb[0];
                    return ab[1]>bb[1];
                });
        }
        json out=json::array();
        for (const auto* bp:blks) {
            const auto& blk=*bp;
            std::string txt;
            for (const auto& line:blk.lines) {
                const auto spans=BuildLineSpans(line);
                for (const auto& sp:spans) for (const auto* ch:sp.chars) if (ch) AppendUTF8(txt,ch->c);
                txt+="\n";
            }
            auto bb=fz_transform_rect({blk.bbox.x0,blk.bbox.y0,blk.bbox.x1,blk.bbox.y1},ctm);
            json bd; bd["bbox"]={bb[0],bb[1],bb[2],bb[3]}; bd["text"]=txt;
            bd["type"]=(blk.type==WinExtract::BlockType::TEXT)?0:1;
            out.push_back(std::move(bd));
        }
        return out.dump();
    }

    // Python: doc.get_all_text()
    std::string get_all_text() const {
        int n=doc_->count_pages(); std::string r;
        for (int i=0;i<n;++i) { r+=ExtractTextPlain(doc_,i,false); if (i<n-1) r+="\x0C"; }
        return r;
    }

    // Python: doc.get_all_dicts_json()
    std::string get_all_dicts_json(bool include_chars=false, bool sort=false) const {
        int n=doc_->count_pages(); std::string r="[";
        for (int i=0;i<n;++i) {
            const auto ep=DoExtractPage(doc_,i,sort);
            r+=PageToJsonStr(ep,i,include_chars,sort);
            if (i<n-1) r+=",";
        }
        return r+"]";
    }

    // ── Font info ─────────────────────────────────────────────────────────────
    // Python: doc.get_page_font_basenames(i) → JSON object
    std::string get_page_font_basenames(int i) const {
        chk(i);
        const auto mm=doc_->get_page_font_vertical_metrics_map(i);
        json out=json::object();
        for (const auto& kv:mm) if (!kv.second.base_font.empty()) out[kv.first]=kv.second.base_font;
        return out.dump();
    }

    // ── Redaction – pure C++, no PDFium ─────────────────────────────────────
    // Python: doc.redact_pages_bytes({page_index: [[x0,y0,x1,y1], ...]})
    // JS: pass JSON string: '{"0": [[10,10,200,30]], "1": [[50,50,300,80]]}'
    // Returns binary PDF bytes as std::string (Embind binary string → JS Uint8Array)
    std::string redact_pages_bytes(const std::string& page_rects_json) const {
        json j;
        try { j=json::parse(page_rects_json); }
        catch(const std::exception& e) {
            throw std::runtime_error(std::string("JSON parse error: ")+e.what());
        }

        std::map<int,std::vector<uint8_t>> pages_streams;
        std::map<int,std::vector<uint8_t>> updated_xobjects;

        for (auto& [key, rects_arr] : j.items()) {
            int page_idx = std::stoi(key);
            chk(page_idx);
            const auto geo = doc_->get_page_geometry(page_idx);
            fz_matrix ctm  = ComputePageCTM(geo);
            float ctm_arr[6] = {ctm.a,ctm.b,ctm.c,ctm.d,ctm.e,ctm.f};

            std::vector<winnerz::WinRedactZone_TopDown> zones;
            for (const auto& rect : rects_arr) {
                winnerz::WinRedactZone_TopDown z;
                z.rect = {rect[0].get<float>(), rect[1].get<float>(),
                          rect[2].get<float>(), rect[3].get<float>()};
                zones.push_back(z);
            }

            std::map<int,std::vector<uint8_t>> page_xobj;
            const auto filtered = winnerz::WinnerZ_RedactPage(*doc_, page_idx, zones, page_xobj, ctm_arr);
            pages_streams[page_idx] = filtered;
            for (const auto& kv : page_xobj) updated_xobjects[kv.first]=kv.second;
        }

        auto out = doc_->save_multiple_pages_content_incremental_to_bytes(pages_streams, updated_xobjects);
        if (out.empty()) throw std::runtime_error("Redaction failed: save returned empty bytes");
        return VecToStr(out);
    }

    // Python: doc[i].redact_text(rects, output_path) → but returns bytes in WASM
    std::string redact_page_bytes(int page_idx, const std::string& rects_json) const {
        chk(page_idx);
        json j;
        try { j=json::parse(rects_json); }
        catch(const std::exception& e) { throw std::runtime_error(std::string("JSON parse error: ")+e.what()); }

        const auto geo=doc_->get_page_geometry(page_idx);
        fz_matrix ctm=ComputePageCTM(geo);
        float ctm_arr[6]={ctm.a,ctm.b,ctm.c,ctm.d,ctm.e,ctm.f};

        std::vector<winnerz::WinRedactZone_TopDown> zones;
        for (const auto& rect : j) {
            winnerz::WinRedactZone_TopDown z;
            z.rect={rect[0].get<float>(), rect[1].get<float>(),
                    rect[2].get<float>(), rect[3].get<float>()};
            zones.push_back(z);
        }

        std::map<int,std::vector<uint8_t>> xobj;
        const auto filtered=winnerz::WinnerZ_RedactPage(*doc_, page_idx, zones, xobj, ctm_arr);
        std::map<int,std::vector<uint8_t>> ps; ps[page_idx]=filtered;
        auto out=doc_->save_multiple_pages_content_incremental_to_bytes(ps, xobj);
        if (out.empty()) throw std::runtime_error("Redaction failed");
        return VecToStr(out);
    }

    // ── Insert colored rectangles – pure C++ ─────────────────────────────────
    // Python: doc.insert_rects_json(json_str)
    // JS: pass JSON: '{"0": [{"rect":[10,10,200,30], "color":[255,0,0]}]}'
    std::string insert_rects_bytes(const std::string& json_str) const {
#ifdef WINEXTRACT_USE_FREETYPE
        json j;
        try { j=json::parse(json_str); } catch(const std::exception& e) {
            throw std::runtime_error(std::string("JSON parse error: ")+e.what());
        }

        std::map<int,std::vector<Winnerz::WinInsertRectTask>> pages_tasks;
        for (auto& [key, arr] : j.items()) {
            int pi=std::stoi(key);
            std::vector<Winnerz::WinInsertRectTask> tasks;
            for (auto& item : arr) {
                Winnerz::WinInsertRectTask t;
                float pad = item.value("pad", 2.0f);
                auto rect = item["rect"];
                t.x0=rect[0].get<float>()-pad; t.y0=rect[1].get<float>()-pad;
                t.x1=rect[2].get<float>()+pad; t.y1=rect[3].get<float>()+pad;
                auto color=item["color"];
                t.r=color[0].get<int>(); t.g=color[1].get<int>(); t.b=color[2].get<int>();
                tasks.push_back(t);
            }
            pages_tasks[pi]=tasks;
        }

        auto out=Winnerz::InsertRectsToMultiplePages(doc_.get(), pages_tasks, nullptr, 0);
        if (out.empty()) throw std::runtime_error("insert_rects_bytes failed");
        return VecToStr(out);
#else
        (void)json_str;
        throw std::runtime_error("insert_rects_bytes requires FreeType (WINEXTRACT_USE_FREETYPE)");
        return "";
#endif
    }

    // ── Insert text – pure C++ (FreeType+HarfBuzz, NO PDFium) ───────────────
    // Python: doc.insert_text_json(json_str, fonts_dir)
    // In WASM: fonts are provided as a JSON map { "FontFamily-Bold": "<base64 bytes>" }
    // OR as a fontsBytesMapJson: { "path/to/font.ttf": "<binary string>" }
    //
    // JS usage:
    //   const tasksJson = JSON.stringify({
    //     "0": [{ text:"Hello", rect:[10,10,200,30], size:12,
    //             color:[0,0,0], bold:false, italic:false,
    //             multiline:false, font_family:"" }]
    //   });
    //   const pdfBytes = doc.insertTextBytes(tasksJson, fontsDir);
    //   // fontsDir is a path in WASM virtual FS (you can mount fonts via FS.writeFile)
    std::string insert_text_bytes(const std::string& json_str, const std::string& fonts_dir) const {
#ifdef WINEXTRACT_USE_FREETYPE
        json j;
        try { j=json::parse(json_str); } catch(const std::exception& e) {
            throw std::runtime_error(std::string("JSON parse error: ")+e.what());
        }

        std::map<int,std::vector<Winnerz::WinInsertTextTask>> pages_tasks;
        for (auto& [key, arr] : j.items()) {
            int pi=std::stoi(key);
            std::vector<Winnerz::WinInsertTextTask> tasks;
            for (auto& item : arr) {
                Winnerz::WinInsertTextTask t;
                t.text=item.value("text","");
                auto rect=item["rect"];
                t.x0=rect[0].get<float>(); t.y0=rect[1].get<float>();
                t.x1=rect[2].get<float>(); t.y1=rect[3].get<float>();
                t.font_size=item.value("size",12.0f);
                auto color=item["color"];
                t.r=color[0].get<int>(); t.g=color[1].get<int>(); t.b=color[2].get<int>();
                t.bold=item.value("bold",false);
                t.italic=item.value("italic",false);
                t.multiline=item.value("multiline",false);
                t.font_family=item.value("font_family","");
                tasks.push_back(t);
            }
            pages_tasks[pi]=tasks;
        }

        // single-threaded in WASM (num_threads_opt=1)
        auto out=Winnerz::InsertTextToMultiplePages(doc_.get(), pages_tasks, fonts_dir, nullptr, 1);
        if (out.empty()) throw std::runtime_error("insert_text_bytes failed");
        return VecToStr(out);
#else
        (void)json_str; (void)fonts_dir;
        throw std::runtime_error("insert_text_bytes requires FreeType+HarfBuzz (WINEXTRACT_USE_FREETYPE)");
        return "";
#endif
    }

    // ── Cache management ──────────────────────────────────────────────────────
    void clear_page_cache() { if (doc_) doc_->clear_page_cache(); }

private:
    void chk(int i) const {
        int n=doc_->count_pages();
        if (i<0||i>=n) throw std::runtime_error("page_index out of range");
    }
    std::vector<uint8_t> mem_;
    std::shared_ptr<WinExtract::WinPdfDocument> doc_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Emscripten Embind bindings – exported to JavaScript
// ─────────────────────────────────────────────────────────────────────────────

EMSCRIPTEN_BINDINGS(winnerz) {
    class_<WasmDocument>("WasmDocument")
        .constructor<std::string>()

        // Basic info
        .function("pageCount",            &WasmDocument::page_count)
        .function("isEncrypted",          &WasmDocument::is_encrypted)
        .function("pageRect",             &WasmDocument::page_rect)

        // Text extraction (mirrors Python API)
        .function("getTextPlain",         &WasmDocument::get_text_plain)
        .function("getJson",              &WasmDocument::get_json)
        .function("getRawJson",           &WasmDocument::get_rawjson)
        .function("getBlocksJson",        &WasmDocument::get_blocks_json)
        .function("getAllText",           &WasmDocument::get_all_text)
        .function("getAllDictsJson",      &WasmDocument::get_all_dicts_json)

        // Font info
        .function("getPageFontBasenames", &WasmDocument::get_page_font_basenames)

        // Redaction – pure C++, returns binary PDF bytes as string
        .function("redactPageBytes",      &WasmDocument::redact_page_bytes)
        .function("redactPagesBytes",     &WasmDocument::redact_pages_bytes)

        // Insert colored rectangles – pure C++
        .function("insertRectsBytes",     &WasmDocument::insert_rects_bytes)

        // Insert text – FreeType+HarfBuzz, pure C++, NO PDFium needed
        .function("insertTextBytes",      &WasmDocument::insert_text_bytes)

        // Cache
        .function("clearPageCache",       &WasmDocument::clear_page_cache);
}

#endif // __EMSCRIPTEN__
