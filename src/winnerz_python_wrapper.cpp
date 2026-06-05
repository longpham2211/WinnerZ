#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<pybind11/pybind11.h>) && __has_include(<pybind11/stl.h>)
#define WINNERZ_PYBIND_AVAILABLE 1
#else
#define WINNERZ_PYBIND_AVAILABLE 0
#endif
#else
#define WINNERZ_PYBIND_AVAILABLE 0
#endif

#if WINNERZ_PYBIND_AVAILABLE

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <future>
#include <iostream>

#include "redactor.hpp"
#include "preview_pdf.hpp"
#include "extract_text/extractor_logic.hpp"
#include "extract_text/pdf_engine.hpp"

#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
#include "drawing.hpp"
#include "fpdfview.h"
#endif

namespace py = pybind11;

namespace {

// Some modern libc toolchains redirect strtol to __isoc23_strtol, which can
// force GLIBC_2.38 at runtime. Provide a local implementation so the module
// stays compatible with older glibc runtimes.
extern "C" long __isoc23_strtol(const char* nptr, char** endptr, int base) {
    if (!nptr) {
        if (endptr) {
            *endptr = nullptr;
        }
        return 0;
    }

    const char* p = nptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
        ++p;
    }

    int sign = 1;
    if (*p == '-') {
        sign = -1;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }
    }

    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = const_cast<char*>(nptr);
        }
        return 0;
    }

    unsigned long value = 0;
    bool any_digit = false;

    for (;; ++p) {
        int digit = -1;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'z') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'Z') {
            digit = *p - 'A' + 10;
        }

        if (digit < 0 || digit >= base) {
            break;
        }

        any_digit = true;
        if (value > (ULONG_MAX - static_cast<unsigned long>(digit)) / static_cast<unsigned long>(base)) {
            value = ULONG_MAX;
            while (true) {
                const char c = *++p;
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
                if (d < 0 || d >= base) break;
            }
            break;
        }

        value = value * static_cast<unsigned long>(base) + static_cast<unsigned long>(digit);
    }

    if (endptr) {
        *endptr = const_cast<char*>(any_digit ? p : nptr);
    }

    if (!any_digit) {
        return 0;
    }

    if (sign < 0) {
        if (value > static_cast<unsigned long>(LONG_MAX) + 1UL) {
            return LONG_MIN;
        }
        if (value == static_cast<unsigned long>(LONG_MAX) + 1UL) {
            return LONG_MIN;
        }
        return -static_cast<long>(value);
    }

    if (value > static_cast<unsigned long>(LONG_MAX)) {
        return LONG_MAX;
    }
    return static_cast<long>(value);
}

struct ExtractedPage {
    WinExtract::WinPage page;
    WinExtract::WinPageGeometry geo;
};

static std::string Utf8FromCodepoint(int cp) {
    std::string out;
    if (cp <= 0 || cp > 0x10FFFF) {
        return out;
    }

    const uint32_t ucp = static_cast<uint32_t>(cp);
    if (ucp <= 0x7F) {
        out.push_back(static_cast<char>(ucp));
    } else if (ucp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((ucp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (ucp & 0x3F)));
    } else if (ucp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((ucp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((ucp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ucp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((ucp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((ucp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ucp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ucp & 0x3F)));
    }

    return out;
}

static void AppendUtf8Codepoint(std::string& out, int cp) {
    if (cp == 0x2028 || cp == 0x2029) cp = '\n';
    if (cp <= 0 || cp > 0x10FFFF) {
        return;
    }

    const uint32_t ucp = static_cast<uint32_t>(cp);
    if (ucp <= 0x7F) {
        out.push_back(static_cast<char>(ucp));
    } else if (ucp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((ucp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (ucp & 0x3F)));
    } else if (ucp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((ucp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((ucp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ucp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((ucp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((ucp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ucp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ucp & 0x3F)));
    }
}

static std::string EscapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c < 32) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned int>(c));
            out += hex;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static std::array<float, 4> QuadToBBox(const WinExtract::Quad& quad) {
    const float x0 = (std::min)({quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x});
    const float y0 = (std::min)({quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y});
    const float x1 = (std::max)({quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x});
    const float y1 = (std::max)({quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y});
    return {x0, y0, x1, y1};
}

struct fz_matrix {
    float a, b, c, d, e, f;
};

static fz_matrix fz_scale(float sx, float sy) {
    return {sx, 0, 0, sy, 0, 0};
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
        m.e,
        m.f
    };
}

static fz_matrix fz_translate(float tx, float ty) {
    return {1, 0, 0, 1, tx, ty};
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

static std::array<float, 4> fz_transform_rect(const std::array<float, 4>& r, fz_matrix m) {
    float x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3];
    float t_x0 = x0 * m.a + y0 * m.c + m.e;
    float t_y0 = x0 * m.b + y0 * m.d + m.f;
    float t_x1 = x1 * m.a + y0 * m.c + m.e;
    float t_y1 = x1 * m.b + y0 * m.d + m.f;
    float t_x2 = x0 * m.a + y1 * m.c + m.e;
    float t_y2 = x0 * m.b + y1 * m.d + m.f;
    float t_x3 = x1 * m.a + y1 * m.c + m.e;
    float t_y3 = x1 * m.b + y1 * m.d + m.f;

    float nx0 = std::min({t_x0, t_x1, t_x2, t_x3});
    float ny0 = std::min({t_y0, t_y1, t_y2, t_y3});
    float nx1 = std::max({t_x0, t_x1, t_x2, t_x3});
    float ny1 = std::max({t_y0, t_y1, t_y2, t_y3});
    return {nx0, ny0, nx1, ny1};
}

static std::array<float, 2> fz_transform_point(float x, float y, fz_matrix m) {
    return {
        x * m.a + y * m.c + m.e,
        x * m.b + y * m.d + m.f
    };
}

static std::array<float, 4> fz_intersect_rect(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    float x0 = std::max(a[0], b[0]);
    float y0 = std::max(a[1], b[1]);
    float x1 = std::min(a[2], b[2]);
    float y1 = std::min(a[3], b[3]);
    if (x1 < x0 || y1 < y0) return {0,0,0,0};
    return {x0, y0, x1, y1};
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

    cropbox = fz_intersect_rect(cropbox, mediabox);
    if (cropbox[2] - cropbox[0] < 1 || cropbox[3] - cropbox[1] < 1) {
        cropbox = {0, 0, 1, 1};
    }

    std::array<float, 4> trans_cropbox = fz_transform_rect(cropbox, page_ctm);
    page_ctm = fz_concat(page_ctm, fz_translate(-trans_cropbox[0], -trans_cropbox[1]));
    
    return page_ctm;
}

struct SpanCompat {
    std::string font_name;
    float font_size = 0.0f;
    float ascender = 0.8f;
    float descender = -0.2f;
    uint32_t color = 0;
    bool is_bold = false;
    bool is_italic = false;
    bool is_serif = false;
    bool is_mono = false;
    bool is_synthetic = false;
    bool split_leading_spaces = false;
    int wmode = 0;
    WinExtract::Rect bbox{0, 0, 0, 0};
    std::vector<const WinExtract::WinChar*> chars;
};

static WinExtract::Rect CharBBox(const WinExtract::WinChar& ch) {
    const auto b = QuadToBBox(ch.quad);
    return {b[0], b[1], b[2], b[3]};
}

static void MergeRect(WinExtract::Rect& dst, const WinExtract::Rect& src, bool& has_value) {
    if (!has_value) {
        dst = src;
        has_value = true;
        return;
    }

    dst.x0 = std::min(dst.x0, src.x0);
    dst.y0 = std::min(dst.y0, src.y0);
    dst.x1 = std::max(dst.x1, src.x1);
    dst.y1 = std::max(dst.y1, src.y1);
}

static std::vector<SpanCompat> BuildLineSpans(const WinExtract::WinLine& line) {
    std::vector<SpanCompat> spans;
    spans.reserve(line.chars.size());

    for (const auto& ch : line.chars) {
        bool new_span = spans.empty() ||
                        spans.back().font_name != ch.font_name ||
                        std::abs(spans.back().font_size - ch.size) > 0.01f ||
                        spans.back().color != ch.color ||
                        spans.back().is_bold != ch.is_bold ||
                        spans.back().is_italic != ch.is_italic ||
                        spans.back().is_serif != ch.is_serif ||
                        spans.back().is_mono != ch.is_mono ||
                        spans.back().is_synthetic != ch.is_synthetic ||
                        spans.back().wmode != line.wmode;

        if (!new_span && !spans.empty()) {
            const SpanCompat& prev_span = spans.back();
            if (prev_span.split_leading_spaces &&
                prev_span.chars.size() == 1 &&
                prev_span.chars.front() != nullptr &&
                prev_span.chars.front()->c == ' ' &&
                ch.c != ' ') {
                new_span = true;
            }
        }

        if (new_span) {
            SpanCompat span;
            span.font_name = ch.font_name;
            span.font_size = ch.size;
            span.color = ch.color;
            span.is_bold = ch.is_bold;
            span.is_italic = ch.is_italic;
            span.is_serif = ch.is_serif;
            span.is_mono = ch.is_mono;
            span.is_synthetic = ch.is_synthetic;
            span.ascender = ch.ascender;
            span.descender = ch.descender;
            bool split_leading_space = false;
            if (!spans.empty() && ch.c == ' ' && spans.size() == 1) {
                const SpanCompat& prev_span = spans.back();
                auto is_ascii_digit = [](int c) {
                    return c >= '0' && c <= '9';
                };
                auto is_ascii_alpha = [](int c) {
                    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
                };

                bool all_digits = !prev_span.chars.empty();
                bool single_alpha = (prev_span.chars.size() == 1);

                for (const WinExtract::WinChar* prev_ch : prev_span.chars) {
                    if (prev_ch == nullptr) {
                        all_digits = false;
                        single_alpha = false;
                        break;
                    }

                    if (!is_ascii_digit(prev_ch->c)) {
                        all_digits = false;
                    }
                }

                if (single_alpha) {
                    const WinExtract::WinChar* prev_ch = prev_span.chars.front();
                    single_alpha = prev_ch != nullptr && is_ascii_alpha(prev_ch->c);
                }

                if (all_digits || single_alpha) {
                    split_leading_space = true;
                }
            }
            span.split_leading_spaces = split_leading_space;
            span.wmode = line.wmode;
            spans.push_back(std::move(span));
        }

        SpanCompat& span = spans.back();
        span.chars.push_back(&ch);

        bool has_bbox = false;
        if (!span.chars.empty()) {
            has_bbox = span.chars.size() > 1;
        }
        const WinExtract::Rect cb = CharBBox(ch);
        if (span.chars.size() == 1) {
            span.bbox = cb;
        } else {
            span.bbox.x0 = std::min(span.bbox.x0, cb.x0);
            span.bbox.y0 = std::min(span.bbox.y0, cb.y0);
            span.bbox.x1 = std::max(span.bbox.x1, cb.x1);
            span.bbox.y1 = std::max(span.bbox.y1, cb.y1);
        }
        (void)has_bbox;
    }

    return spans;
}

static int SpanFlags(const SpanCompat& span) {
    int flags = 0;
    if (span.is_italic) {
        flags |= 2;
    }
    if (span.is_serif) {
        flags |= 4;
    }
    if (span.is_mono) {
        flags |= 8;
    }
    if (span.is_bold) {
        flags |= 16;
    }
    return flags;
}

static std::string SpanText(const SpanCompat& span) {
    std::string text;
    text.reserve(span.chars.size() * 2);
    for (const auto* ch : span.chars) {
        if (ch != nullptr) {
            AppendUtf8Codepoint(text, ch->c);
        }
    }
    return text;
}

class PyWinDocument {
public:
    std::string path;
    std::shared_ptr<WinExtract::WinPdfDocument> doc;
    std::vector<uint8_t> mem_data;

    PyWinDocument(const std::string& pdf_path) : path(pdf_path) {
        doc = WinExtract::WinPdfDocument::open(pdf_path);
        if (!doc) {
            throw std::runtime_error("Cannot open PDF file or xref not found");
        }
    }

    PyWinDocument(const py::bytes& pdf_bytes) : path("<memory>") {
        std::string str_bytes = pdf_bytes;
        mem_data = std::vector<uint8_t>(str_bytes.begin(), str_bytes.end());
        doc = WinExtract::WinPdfDocument::open_from_memory(mem_data);
        if (!doc) {
            throw std::runtime_error("Cannot open PDF from bytes or xref not found");
        }
    }

    int page_count() {
        return doc->count_pages();
    }

    bool is_encrypted() {
        return doc->is_encrypted();
    }

    void clear_page_cache() {
        if (doc) doc->clear_page_cache();
    }
};

static void ValidatePageIndex(const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int page_index) {
    const int page_count = doc->count_pages();
    if (page_index < 0 || page_index >= page_count) {
        throw std::runtime_error("page_index out of range");
    }
}



static ExtractedPage ExtractTextPage(const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int page_index, bool sort_output = false) {
    ValidatePageIndex(doc, page_index);

    std::vector<uint8_t> stream = doc->get_page_content(page_index);
    WinExtract::WinFontUnicodeMap font_unicode_map = doc->get_page_font_unicode_map(page_index);
    WinExtract::WinFontWidthMap font_width_map = doc->get_page_font_width_map(page_index);
    WinExtract::WinFontCodeBytesMap font_code_bytes_map = doc->get_page_font_code_bytes_map(page_index);
    WinExtract::WinFontCodeSpaceMap font_codespace_map = doc->get_page_font_codespace_map(page_index);
    WinExtract::WinFontMatrixMap font_matrix_map = doc->get_page_font_matrix_map(page_index);
    WinExtract::WinFontVerticalMetricsMap font_vertical_metrics_map = doc->get_page_font_vertical_metrics_map(page_index);
    WinExtract::WinColorSpaceMap color_space_map = doc->get_page_color_space_map(page_index);
    std::shared_ptr<const WinExtract::WinFormXObjectMap> form_xobject_map = doc->get_page_form_xobject_map(page_index);
    WinExtract::WinPageGeometry geo = doc->get_page_geometry(page_index);

    WinExtract::MuLogicExtractor dev;
    dev.begin_page(geo.mediabox.x1 - geo.mediabox.x0, geo.mediabox.y1 - geo.mediabox.y0);

    WinExtract::WinPdfInterpreter::run(
        stream,
        dev,
        font_unicode_map,
        font_width_map,
        font_code_bytes_map,
        font_codespace_map,
        font_matrix_map,
        font_vertical_metrics_map,
        color_space_map,
        form_xobject_map,
        nullptr,
        0,
        &geo.mediabox,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    ExtractedPage extracted;
    extracted.page = dev.finish_page();
    extracted.geo = geo;

    return extracted;
}

static std::array<float, 4> LoadPageRect(const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int page_index) {
    ValidatePageIndex(doc, page_index);
    const WinExtract::WinPageGeometry geo = doc->get_page_geometry(page_index);
    fz_matrix ctm = ComputePageCTM(geo);
    std::array<float, 4> crop_arr = {geo.cropbox.x0, geo.cropbox.y0, geo.cropbox.x1, geo.cropbox.y1};
    std::array<float, 4> bbox = fz_transform_rect(crop_arr, ctm);
    const float width = std::max(0.0f, bbox[2] - bbox[0]);
    const float height = std::max(0.0f, bbox[3] - bbox[1]);
    return {0.0f, 0.0f, width, height};
}






#include <Python.h>

static py::object DictToRawPyDict(const ExtractedPage& extracted, int page_index, bool include_chars, bool sort_output) {
    fz_matrix ctm = ComputePageCTM(extracted.geo);
    // Dùng CropBox (giống LoadPageRect) để tính width/height — tránh lệch tỉ lệ khi CropBox origin != (0,0)
    std::array<float, 4> crop_arr = {extracted.geo.cropbox.x0, extracted.geo.cropbox.y0, extracted.geo.cropbox.x1, extracted.geo.cropbox.y1};
    std::array<float, 4> page_bbox = fz_transform_rect(crop_arr, ctm);

    const float width = std::max(0.0f, page_bbox[2] - page_bbox[0]);
    const float height = std::max(0.0f, page_bbox[3] - page_bbox[1]);

    PyObject* out = PyDict_New();
    
    PyObject* py_page_index = PyLong_FromLong(page_index);
    PyDict_SetItemString(out, "page_num", py_page_index);
    Py_DECREF(py_page_index);

    PyObject* py_width = PyFloat_FromDouble(width);
    PyDict_SetItemString(out, "width", py_width);
    Py_DECREF(py_width);

    PyObject* py_height = PyFloat_FromDouble(height);
    PyDict_SetItemString(out, "height", py_height);
    Py_DECREF(py_height);

    std::vector<const WinExtract::WinBlock*> sorted_blocks;
    for (const auto& block : extracted.page.blocks) {
        sorted_blocks.push_back(&block);
    }
    if (sort_output) {
        std::stable_sort(sorted_blocks.begin(), sorted_blocks.end(), [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b) {
            auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
            auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
            if (ab[3] != bb[3]) return ab[3] < bb[3];
            if (ab[0] != bb[0]) return ab[0] < bb[0];
            return false;
        });
    }

    PyObject* blocks_list = PyList_New(sorted_blocks.size());
    for (size_t b_idx = 0; b_idx < sorted_blocks.size(); ++b_idx) {
        const auto& block = *sorted_blocks[b_idx];
        PyObject* block_dict = PyDict_New();
        
        PyObject* type_val = PyLong_FromLong(block.type == WinExtract::BlockType::TEXT ? 0 : 1);
        PyDict_SetItemString(block_dict, "type", type_val);
        Py_DECREF(type_val);

        const auto block_bbox = fz_transform_rect({block.bbox.x0, block.bbox.y0, block.bbox.x1, block.bbox.y1}, ctm);
        PyObject* bbox_tuple = PyTuple_New(4);
        PyTuple_SET_ITEM(bbox_tuple, 0, PyFloat_FromDouble(block_bbox[0]));
        PyTuple_SET_ITEM(bbox_tuple, 1, PyFloat_FromDouble(block_bbox[1]));
        PyTuple_SET_ITEM(bbox_tuple, 2, PyFloat_FromDouble(block_bbox[2]));
        PyTuple_SET_ITEM(bbox_tuple, 3, PyFloat_FromDouble(block_bbox[3]));
        PyDict_SetItemString(block_dict, "bbox", bbox_tuple);
        Py_DECREF(bbox_tuple);
        
        PyObject* lines_list = PyList_New(block.lines.size());
        for (size_t l_idx = 0; l_idx < block.lines.size(); ++l_idx) {
            const auto& line = block.lines[l_idx];
            PyObject* line_dict = PyDict_New();
            
            const auto line_bbox = fz_transform_rect({line.bbox.x0, line.bbox.y0, line.bbox.x1, line.bbox.y1}, ctm);
            PyObject* l_bbox_tuple = PyTuple_New(4);
            PyTuple_SET_ITEM(l_bbox_tuple, 0, PyFloat_FromDouble(line_bbox[0]));
            PyTuple_SET_ITEM(l_bbox_tuple, 1, PyFloat_FromDouble(line_bbox[1]));
            PyTuple_SET_ITEM(l_bbox_tuple, 2, PyFloat_FromDouble(line_bbox[2]));
            PyTuple_SET_ITEM(l_bbox_tuple, 3, PyFloat_FromDouble(line_bbox[3]));
            PyDict_SetItemString(line_dict, "bbox", l_bbox_tuple);
            Py_DECREF(l_bbox_tuple);

            PyObject* wmode_val = PyLong_FromLong(line.wmode);
            PyDict_SetItemString(line_dict, "wmode", wmode_val);
            Py_DECREF(wmode_val);

            // Tạm thời bỏ qua transform dir phức tạp, MuPDF có fz_transform_vector nhưng chúng ta giả lập:
            std::array<float, 2> t_dir = fz_transform_point(line.dir.x, line.dir.y, ctm);
            std::array<float, 2> t_orig = fz_transform_point(0, 0, ctm);
            PyObject* dir_tuple = PyTuple_New(2);
            PyTuple_SET_ITEM(dir_tuple, 0, PyFloat_FromDouble(t_dir[0] - t_orig[0]));
            PyTuple_SET_ITEM(dir_tuple, 1, PyFloat_FromDouble(t_dir[1] - t_orig[1]));
            PyDict_SetItemString(line_dict, "dir", dir_tuple);
            Py_DECREF(dir_tuple);

            const auto spans = BuildLineSpans(line);
            PyObject* spans_list = PyList_New(spans.size());
            for (size_t s_idx = 0; s_idx < spans.size(); ++s_idx) {
                const auto& span = spans[s_idx];
                PyObject* span_dict = PyDict_New();
                
                std::string span_text = SpanText(span);
                PyObject* text_val = PyUnicode_FromStringAndSize(span_text.data(), span_text.size());
                PyDict_SetItemString(span_dict, "text", text_val);
                Py_DECREF(text_val);

                const auto span_bbox = fz_transform_rect({span.bbox.x0, span.bbox.y0, span.bbox.x1, span.bbox.y1}, ctm);
                PyObject* s_bbox_tuple = PyTuple_New(4);
                PyTuple_SET_ITEM(s_bbox_tuple, 0, PyFloat_FromDouble(span_bbox[0]));
                PyTuple_SET_ITEM(s_bbox_tuple, 1, PyFloat_FromDouble(span_bbox[1]));
                PyTuple_SET_ITEM(s_bbox_tuple, 2, PyFloat_FromDouble(span_bbox[2]));
                PyTuple_SET_ITEM(s_bbox_tuple, 3, PyFloat_FromDouble(span_bbox[3]));
                PyDict_SetItemString(span_dict, "bbox", s_bbox_tuple);
                Py_DECREF(s_bbox_tuple);

                std::array<float, 2> origin;
                if (!span.chars.empty()) {
                    origin = fz_transform_point(span.chars.front()->origin.x, span.chars.front()->origin.y, ctm);
                } else {
                    origin = fz_transform_point(span.bbox.x0, span.bbox.y0, ctm);
                }
                PyObject* origin_tuple = PyTuple_New(2);
                PyTuple_SET_ITEM(origin_tuple, 0, PyFloat_FromDouble(origin[0]));
                PyTuple_SET_ITEM(origin_tuple, 1, PyFloat_FromDouble(origin[1]));
                PyDict_SetItemString(span_dict, "origin", origin_tuple);
                Py_DECREF(origin_tuple);

                PyObject* font_val = PyUnicode_FromString(span.font_name.c_str());
                PyDict_SetItemString(span_dict, "font", font_val);
                Py_DECREF(font_val);

                PyObject* size_val = PyFloat_FromDouble(span.font_size);
                PyDict_SetItemString(span_dict, "size", size_val);
                Py_DECREF(size_val);

                PyObject* asc_val = PyFloat_FromDouble(span.ascender);
                PyDict_SetItemString(span_dict, "ascender", asc_val);
                Py_DECREF(asc_val);

                PyObject* desc_val = PyFloat_FromDouble(span.descender);
                PyDict_SetItemString(span_dict, "descender", desc_val);
                Py_DECREF(desc_val);

                PyObject* color_val = PyLong_FromLong(span.color);
                PyDict_SetItemString(span_dict, "color", color_val);
                Py_DECREF(color_val);

                int flags = SpanFlags(span);
                PyObject* flags_val = PyLong_FromLong(flags);
                PyDict_SetItemString(span_dict, "flags", flags_val);
                Py_DECREF(flags_val);

                if (include_chars) {
                    size_t num_chars = 0;
                    for (const auto* ch : span.chars) {
                        if (ch != nullptr) num_chars++;
                    }
                    PyObject* chars_list = PyList_New(num_chars);
                    size_t c_idx = 0;
                    for (const auto* ch : span.chars) {
                        if (ch == nullptr) continue;
                        PyObject* ch_dict = PyDict_New();
                        
                        std::string ch_str = Utf8FromCodepoint(ch->c);
                        PyObject* c_val = PyUnicode_FromStringAndSize(ch_str.data(), ch_str.size());
                        PyDict_SetItemString(ch_dict, "c", c_val);
                        Py_DECREF(c_val);

                        PyObject* u_val = PyLong_FromLong(ch->c);
                        PyDict_SetItemString(ch_dict, "u", u_val);
                        Py_DECREF(u_val);

                        const auto ch_origin = fz_transform_point(ch->origin.x, ch->origin.y, ctm);
                        PyObject* ch_origin_tup = PyTuple_New(2);
                        PyTuple_SET_ITEM(ch_origin_tup, 0, PyFloat_FromDouble(ch_origin[0]));
                        PyTuple_SET_ITEM(ch_origin_tup, 1, PyFloat_FromDouble(ch_origin[1]));
                        PyDict_SetItemString(ch_dict, "origin", ch_origin_tup);
                        Py_DECREF(ch_origin_tup);

                        const auto q = QuadToBBox(ch->quad);
                        const auto bbox = fz_transform_rect(q, ctm);
                        PyObject* ch_bbox_tup = PyTuple_New(4);
                        PyTuple_SET_ITEM(ch_bbox_tup, 0, PyFloat_FromDouble(bbox[0]));
                        PyTuple_SET_ITEM(ch_bbox_tup, 1, PyFloat_FromDouble(bbox[1]));
                        PyTuple_SET_ITEM(ch_bbox_tup, 2, PyFloat_FromDouble(bbox[2]));
                        PyTuple_SET_ITEM(ch_bbox_tup, 3, PyFloat_FromDouble(bbox[3]));
                        PyDict_SetItemString(ch_dict, "bbox", ch_bbox_tup);
                        Py_DECREF(ch_bbox_tup);

                        PyObject* bidi_val = PyLong_FromLong(ch->bidi);
                        PyDict_SetItemString(ch_dict, "bidi", bidi_val);
                        Py_DECREF(bidi_val);

                        PyObject* wmode_val2 = PyLong_FromLong(span.wmode);
                        PyDict_SetItemString(ch_dict, "wmode", wmode_val2);
                        Py_DECREF(wmode_val2);

                        PyObject* c_flags_val = PyLong_FromLong(flags);
                        PyDict_SetItemString(ch_dict, "flags", c_flags_val);
                        Py_DECREF(c_flags_val);

                        PyList_SET_ITEM(chars_list, c_idx, ch_dict);
                        c_idx++;
                    }
                    PyDict_SetItemString(span_dict, "chars", chars_list);
                    Py_DECREF(chars_list);
                }

                PyList_SET_ITEM(spans_list, s_idx, span_dict);
            }
            PyDict_SetItemString(line_dict, "spans", spans_list);
            Py_DECREF(spans_list);

            PyList_SET_ITEM(lines_list, l_idx, line_dict);
        }
        PyDict_SetItemString(block_dict, "lines", lines_list);
        Py_DECREF(lines_list);
        
        PyList_SET_ITEM(blocks_list, b_idx, block_dict);
    }
    PyDict_SetItemString(out, "blocks", blocks_list);
    Py_DECREF(blocks_list);

    return py::reinterpret_steal<py::object>(out);
}

static py::object BlocksToRawPyList(const ExtractedPage& extracted, bool sort_output) {
    fz_matrix ctm = ComputePageCTM(extracted.geo);
    struct BlockView {
        const WinExtract::WinBlock* block;
        size_t index;
    };

    std::vector<BlockView> blocks;
    blocks.reserve(extracted.page.blocks.size());
    for (size_t i = 0; i < extracted.page.blocks.size(); ++i) {
        blocks.push_back(BlockView{&extracted.page.blocks[i], i});
    }

    if (sort_output) {
        std::stable_sort(blocks.begin(), blocks.end(), [&ctm](const BlockView& a, const BlockView& b) {
            auto ab = fz_transform_rect({a.block->bbox.x0, a.block->bbox.y0, a.block->bbox.x1, a.block->bbox.y1}, ctm);
            auto bb = fz_transform_rect({b.block->bbox.x0, b.block->bbox.y0, b.block->bbox.x1, b.block->bbox.y1}, ctm);
            if (std::abs(ab[3] - bb[3]) > 0.01f) return ab[3] > bb[3];
            if (std::abs(ab[0] - bb[0]) > 0.01f) return ab[0] < bb[0];
            return ab[1] > bb[1];
        });
    }

    PyObject* out_list = PyList_New(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = *blocks[i].block;

        std::vector<const WinExtract::WinLine*> lines;
        lines.reserve(block.lines.size());
        for (const auto& line : block.lines) {
            lines.push_back(&line);
        }

        if (sort_output) {
            std::stable_sort(lines.begin(), lines.end(), [&ctm](const WinExtract::WinLine* a, const WinExtract::WinLine* b) {
                auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
                auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
                if (std::abs(ab[3] - bb[3]) > 0.01f) return ab[3] > bb[3];
                if (std::abs(ab[0] - bb[0]) > 0.01f) return ab[0] < bb[0];
                return ab[1] > bb[1];
            });
        }

        std::string block_text;
        for (const auto* line : lines) {
            const auto spans = BuildLineSpans(*line);
            for (const auto& span : spans) {
                for (const auto* ch : span.chars) {
                    if (ch != nullptr) {
                        AppendUtf8Codepoint(block_text, ch->c);
                    }
                }
            }
            block_text += "\n";
        }

        PyObject* block_dict = PyDict_New();
        const auto block_bbox = fz_transform_rect({block.bbox.x0, block.bbox.y0, block.bbox.x1, block.bbox.y1}, ctm);
        
        PyObject* bbox_tuple = PyTuple_New(4);
        PyTuple_SET_ITEM(bbox_tuple, 0, PyFloat_FromDouble(block_bbox[0]));
        PyTuple_SET_ITEM(bbox_tuple, 1, PyFloat_FromDouble(block_bbox[1]));
        PyTuple_SET_ITEM(bbox_tuple, 2, PyFloat_FromDouble(block_bbox[2]));
        PyTuple_SET_ITEM(bbox_tuple, 3, PyFloat_FromDouble(block_bbox[3]));
        PyDict_SetItemString(block_dict, "bbox", bbox_tuple);
        Py_DECREF(bbox_tuple);

        PyObject* text_val = PyUnicode_FromStringAndSize(block_text.data(), block_text.size());
        PyDict_SetItemString(block_dict, "text", text_val);
        Py_DECREF(text_val);

        PyObject* block_no_val = PyLong_FromLong(blocks[i].index);
        PyDict_SetItemString(block_dict, "block_no", block_no_val);
        Py_DECREF(block_no_val);

        PyObject* type_val = PyLong_FromLong(block.type == WinExtract::BlockType::TEXT ? 0 : 1);
        PyDict_SetItemString(block_dict, "type", type_val);
        Py_DECREF(type_val);

        PyList_SET_ITEM(out_list, i, block_dict);
    }
    
    return py::reinterpret_steal<py::object>(out_list);
}

static std::string ExtractTextPlain(const std::shared_ptr<WinExtract::WinPdfDocument>& doc, int page_index, bool sort_output) {
    const ExtractedPage extracted = ExtractTextPage(doc, page_index, sort_output);
    std::string text_out;
    text_out.reserve(8192);

    fz_matrix ctm = ComputePageCTM(extracted.geo);

    std::vector<const WinExtract::WinBlock*> sorted_blocks;
    for (const auto& block : extracted.page.blocks) {
        sorted_blocks.push_back(&block);
    }
    if (sort_output) {
        std::stable_sort(sorted_blocks.begin(), sorted_blocks.end(), [&ctm](const WinExtract::WinBlock* a, const WinExtract::WinBlock* b) {
            auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
            auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
            if (ab[3] != bb[3]) return ab[3] < bb[3];
            if (ab[0] != bb[0]) return ab[0] < bb[0];
            return false;
        });
    }

    for (const auto* block_ptr : sorted_blocks) {
        const auto& block = *block_ptr;
        if (block.type != WinExtract::BlockType::TEXT) continue;
        
        std::vector<const WinExtract::WinLine*> lines;
        for (const auto& line : block.lines) lines.push_back(&line);
        
        if (sort_output) {
            std::stable_sort(lines.begin(), lines.end(), [&ctm](const WinExtract::WinLine* a, const WinExtract::WinLine* b) {
                auto ab = fz_transform_rect({a->bbox.x0, a->bbox.y0, a->bbox.x1, a->bbox.y1}, ctm);
                auto bb = fz_transform_rect({b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1}, ctm);
                if (ab[3] != bb[3]) return ab[3] < bb[3];
                if (ab[0] != bb[0]) return ab[0] < bb[0];
                return false;
            });
        }

        for (size_t l_idx = 0; l_idx < lines.size(); ++l_idx) {
            const auto* line = lines[l_idx];
            for (size_t c_idx = 0; c_idx < line->chars.size(); ++c_idx) {
                const auto& ch = line->chars[c_idx];
                if (line->joined && c_idx == line->chars.size() - 1 && ch.c == '-') {
                    continue;
                }
                AppendUtf8Codepoint(text_out, ch.c);
            }
            if (!line->joined) {
                text_out += "\n";
            }
        }
    }
    doc->clear_page_cache();
    return text_out;
}

static py::dict RenderPageToBytes(
    const std::string& pdf_path,
    int page_index,
    float scale,
    py::object clip_obj) {
    if (scale <= 0.0f) {
        throw std::runtime_error("scale must be > 0");
    }

    std::array<float, 4> clip = {0, 0, 0, 0};
    const std::array<float, 4>* clip_ptr = nullptr;
    if (!clip_obj.is_none()) {
        clip = clip_obj.cast<std::array<float, 4>>();
        clip_ptr = &clip;
    }

    winnerz::PreviewImage preview;
    std::string error_message;
    if (!winnerz::RenderPdfPagePreview(pdf_path, page_index, scale, clip_ptr, preview, &error_message)) {
        throw std::runtime_error("render_page_to_bytes failed: " + error_message);
    }

    py::dict out;
    out["width"] = preview.width;
    out["height"] = preview.height;
    out["channels"] = preview.channels;
    out["stride"] = preview.stride;
    out["samples"] = py::bytes(
        reinterpret_cast<const char*>(preview.rgba.data()),
        static_cast<py::ssize_t>(preview.rgba.size()));
    return out;
}

static py::list ExtractDrawingsToPyList(const std::string& pdf_path, int page_index) {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
    static std::once_flag pdfium_init_once;
    static bool pdfium_ready = false;
    std::call_once(pdfium_init_once, []() {
        FPDF_InitLibrary();
        pdfium_ready = true;
    });
    if (!pdfium_ready) {
        return py::list();
    }

    FPDF_DOCUMENT document = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!document) {
        return py::list();
    }

    const int page_count = FPDF_GetPageCount(document);
    if (page_index < 0 || page_index >= page_count) {
        FPDF_CloseDocument(document);
        return py::list();
    }

    FPDF_PAGE page = FPDF_LoadPage(document, page_index);
    if (!page) {
        FPDF_CloseDocument(document);
        return py::list();
    }

    const std::vector<Winnerz::WzDrawingItem> drawings = Winnerz::GetDrawingsFromPdfium(page);
    py::list out;
    for (const auto& d : drawings) {
        py::dict item;
        item["type"] = d.type;
        item["scissor_clip"] = py::make_tuple(d.scissor_clip.x0, d.scissor_clip.y0, d.scissor_clip.x1, d.scissor_clip.y1);
        if (!d.fill_color.components.empty()) {
            py::dict fill_color;
            py::list comps;
            for (auto c : d.fill_color.components) comps.append(c);
            fill_color["components"] = comps;
            fill_color["alpha"] = d.fill_color.alpha;
            item["fill_color"] = fill_color;
        }
        if (!d.stroke_color.components.empty()) {
            py::dict stroke_color;
            py::list comps;
            for (auto c : d.stroke_color.components) comps.append(c);
            stroke_color["components"] = comps;
            stroke_color["alpha"] = d.stroke_color.alpha;
            item["stroke_color"] = stroke_color;
        }
        out.append(item);
    }

    FPDF_ClosePage(page);
    FPDF_CloseDocument(document);
    return out;
#else
    (void)pdf_path;
    (void)page_index;
    return py::list();
#endif
}

}  // namespace

PYBIND11_MODULE(winnerz_core, m) {
    m.doc() = "WinnerZ Python bridge (Pybind11)";

    py::class_<PyWinDocument>(m, "Document")
        .def(py::init<const py::bytes&>())
        .def(py::init<const std::string&>())
        .def("page_count", &PyWinDocument::page_count)
        .def("is_encrypted", &PyWinDocument::is_encrypted)
        .def("clear_page_cache", &PyWinDocument::clear_page_cache,
             "Clear document cache to release memory after processing a page.")
        .def("page_rect",
             [](PyWinDocument& self, int page_index) {
                 return LoadPageRect(self.doc, page_index);
             },
             py::arg("page_index") = 0)
        .def("get_text_plain",
             [](PyWinDocument& self, int page_index, bool sort) {
                 return ExtractTextPlain(self.doc, page_index, sort);
             },
             py::arg("page_index") = 0,
             py::arg("sort") = false)
        .def("get_dict",
             [](PyWinDocument& self, int page_index, bool sort) {
                 const ExtractedPage extracted = ExtractTextPage(self.doc, page_index, sort);
                 auto res = DictToRawPyDict(extracted, page_index, false, sort);
                 self.doc->clear_page_cache();
                 return res;
             },
             py::arg("page_index") = 0,
             py::arg("sort") = false)
        .def("get_rawdict",
             [](PyWinDocument& self, int page_index, bool sort) {
                 const ExtractedPage extracted = ExtractTextPage(self.doc, page_index, sort);
                 auto res = DictToRawPyDict(extracted, page_index, true, sort);
                 self.doc->clear_page_cache();
                 return res;
             },
             py::arg("page_index") = 0,
             py::arg("sort") = false)
        .def("get_blocks",
             [](PyWinDocument& self, int page_index, bool sort) {
                 const ExtractedPage extracted = ExtractTextPage(self.doc, page_index, sort);
                 auto res = BlocksToRawPyList(extracted, sort);
                 self.doc->clear_page_cache();
                 return res;
             },
             py::arg("page_index") = 0,
             py::arg("sort") = false)
        .def("extract_text",
             [](PyWinDocument& self, int page_index) {
                 const ExtractedPage extracted = ExtractTextPage(self.doc, page_index);
                 WinExtract::MuLogicExtractor dev;
                 auto res = dev.get_text(extracted.page);
                 return res;
             },
             py::arg("page_index") = 0)
                                .def("get_all_text",
             [](PyWinDocument& self) {
                 int page_count = self.doc->count_pages();
                 int num_threads = std::thread::hardware_concurrency();
                 if (num_threads <= 0) num_threads = 4;
                 if (num_threads > page_count) num_threads = page_count;

                 std::vector<std::string> page_results(page_count);
                 std::vector<std::thread> threads;
                 std::atomic<int> current_page{0};

                 // SHARED Document for all threads. Xref is already parsed! Zero Disk IO!
                 std::shared_ptr<WinExtract::WinPdfDocument> shared_doc = self.doc;

                 int actual_threads = std::min(num_threads, page_count);
                 for (int t = 0; t < actual_threads; ++t) {
                     threads.emplace_back([shared_doc, page_count, &page_results, &current_page]() {

                         while (true) {
                             int i = current_page.fetch_add(1);
                             if (i >= page_count) break;
                             page_results[i] = ExtractTextPlain(shared_doc, i, false);
                         }
                     });
                 }

                 for (auto& t : threads) {
                     if (t.joinable()) t.join();
                 }

                 std::string result;
                 for (int i = 0; i < page_count; ++i) {
                     result += page_results[i];
                     if (i < page_count - 1) result += "\x0C";
                 }
                 return result;
             },
             py::call_guard<py::gil_scoped_release>())
        .def("get_drawings",
             [](PyWinDocument& self, int page_index) {
                 auto res = ExtractDrawingsToPyList(self.path, page_index);
                 self.doc->clear_page_cache();
                 return res;
             },
             py::arg("page_index") = 0)
        
                .def("redact_pages_bytes",
             [](PyWinDocument& self,
                const std::map<int, std::vector<std::array<float, 4>>>& page_rects_map,
                float min_overlap_ratio) {
                 (void)min_overlap_ratio;
                 std::map<int, std::vector<uint8_t>> pages_streams;

                 for (const auto& kv : page_rects_map) {
                     int page_index = kv.first;
                     ValidatePageIndex(self.doc, page_index);

                     std::vector<winnerz::WinRedactZone_TopDown> zones;
                     zones.reserve(kv.second.size());
                     for (const auto& r : kv.second) {
                         winnerz::WinRedactZone_TopDown zone;
                         zone.rect = {r[0], r[1], r[2], r[3]};
                         zones.push_back(zone);
                     }

                     fz_matrix ctm = ComputePageCTM(self.doc->get_page_geometry(page_index));
                     float ctm_arr[6] = {ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f};
                     const std::vector<uint8_t> filtered = winnerz::WinnerZ_RedactPage(
                         *self.doc, page_index, zones, ctm_arr);
                         
                     pages_streams[page_index] = filtered;
                     self.doc->clear_page_cache();
                 }

                 std::vector<uint8_t> out_bytes = self.doc->save_multiple_pages_content_incremental_to_bytes(pages_streams);
                 if (out_bytes.empty()) {
                     throw std::runtime_error("save_multiple_pages_content_incremental_to_bytes failed");
                 }
                 return py::bytes(reinterpret_cast<const char*>(out_bytes.data()), out_bytes.size());
             },
             py::arg("page_rects_map"),
             py::arg("min_overlap_ratio") = 0.0f)
        .def("redact_pages",
             [](PyWinDocument& self,
                const std::string& output_pdf,
                const std::map<int, std::vector<std::array<float, 4>>>& page_rects_map,
                float min_overlap_ratio) {
                 (void)min_overlap_ratio;
                 std::map<int, std::vector<uint8_t>> pages_streams;

                 for (const auto& kv : page_rects_map) {
                     int page_index = kv.first;
                     ValidatePageIndex(self.doc, page_index);

                     std::vector<winnerz::WinRedactZone_TopDown> zones;
                     zones.reserve(kv.second.size());
                     for (const auto& r : kv.second) {
                         winnerz::WinRedactZone_TopDown zone;
                         zone.rect = {r[0], r[1], r[2], r[3]};
                         zones.push_back(zone);
                     }

                     fz_matrix ctm = ComputePageCTM(self.doc->get_page_geometry(page_index));
                     float ctm_arr[6] = {ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f};
                     const std::vector<uint8_t> filtered = winnerz::WinnerZ_RedactPage(
                         *self.doc, page_index, zones, ctm_arr);
                         
                     pages_streams[page_index] = filtered;
                 }

                 if (!self.doc->save_multiple_pages_content_incremental(pages_streams, output_pdf)) {
                     throw std::runtime_error("save_multiple_pages_content_incremental failed");
                 }

                 py::dict out;
                 return out;
             },
             py::arg("output_pdf"),
             py::arg("page_rects_map"),
             py::arg("min_overlap_ratio") = 0.0f)
.def("redact_rects",
             [](PyWinDocument& self,
                const std::string& output_pdf,
                int page_index,
                const std::vector<std::array<float, 4>>& rects,
                float min_overlap_ratio) {
                 (void)min_overlap_ratio;
                 ValidatePageIndex(self.doc, page_index);

                 std::vector<winnerz::WinRedactZone_TopDown> zones;
                 zones.reserve(rects.size());
                 for (const auto& r : rects) {
                     winnerz::WinRedactZone_TopDown zone;
                     zone.rect = {r[0], r[1], r[2], r[3]};
                     zones.push_back(zone);
                 }

                 fz_matrix ctm = ComputePageCTM(self.doc->get_page_geometry(page_index));
                 float ctm_arr[6] = {ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f};
                 const std::vector<uint8_t> filtered = winnerz::WinnerZ_RedactPage(
                     *self.doc, page_index, zones, ctm_arr);

                 if (!self.doc->save_page_content_incremental(page_index, filtered, output_pdf)) {
                     throw std::runtime_error("save_page_content_incremental failed");
                 }

                 py::dict out;
                 out["scanned_objects"] = static_cast<int>(rects.size());
                 out["removed_text_objects"] = static_cast<int>(rects.size());
                 out["removed_annotations"] = 0;
                 return out;
             },
             py::arg("output_pdf"),
             py::arg("page_index"),
             py::arg("rects"),
             py::arg("min_overlap_ratio") = 0.0f)
        .def("render_page",
             [](PyWinDocument& self,
                int page_index,
                float scale,
                py::object clip) {
                 return RenderPageToBytes(self.path, page_index, scale, clip);
             },
             py::arg("page_index") = 0,
             py::arg("scale") = 1.0f,
             py::arg("clip") = py::none());
}

#else

// Keep translation unit valid when pybind11 headers are not available in editor.
int winnerz_python_wrapper_noop = 0;
#endif
