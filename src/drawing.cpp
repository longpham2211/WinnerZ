#include "drawing.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "fpdf_edit.h"

namespace Winnerz {
namespace {

WzLineCap MapLineCap(int line_cap) {
    switch (line_cap) {
        case FPDF_LINECAP_ROUND:
            return WzLineCap::Round;
        case FPDF_LINECAP_PROJECTING_SQUARE:
            return WzLineCap::Square;
        case FPDF_LINECAP_BUTT:
        default:
            return WzLineCap::Butt;
    }
}

WzLineJoin MapLineJoin(int line_join) {
    switch (line_join) {
        case FPDF_LINEJOIN_ROUND:
            return WzLineJoin::Round;
        case FPDF_LINEJOIN_BEVEL:
            return WzLineJoin::Bevel;
        case FPDF_LINEJOIN_MITER:
        default:
            return WzLineJoin::Miter;
    }
}

WzColor MakeColor(unsigned int r, unsigned int g, unsigned int b, unsigned int a) {
    WzColor color;
    color.components = {
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
    };
    color.alpha = static_cast<float>(a) / 255.0f;
    return color;
}

WzRect ConvertBoundsToTopDown(float left, float bottom, float right, float top, float page_height) {
    WzRect rect{};
    rect.x0 = left;
    rect.x1 = right;
    rect.y0 = page_height - top;
    rect.y1 = page_height - bottom;
    if (rect.y0 > rect.y1) {
        std::swap(rect.y0, rect.y1);
    }
    return rect;
}

WzPath ExtractPath(FPDF_PAGEOBJECT path_obj, float page_height) {
    WzPath path;

    const int segment_count = FPDFPath_CountSegments(path_obj);
    if (segment_count <= 0) {
        return path;
    }

    std::array<WzPoint, 3> bezier_pts{};
    int bezier_count = 0;

    for (int i = 0; i < segment_count; ++i) {
        FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(path_obj, i);
        if (!seg) {
            continue;
        }

        float x = 0.0f;
        float y_pdf = 0.0f;
        if (!FPDFPathSegment_GetPoint(seg, &x, &y_pdf)) {
            continue;
        }
        const float y = page_height - y_pdf;

        const int seg_type = FPDFPathSegment_GetType(seg);
        switch (seg_type) {
            case FPDF_SEGMENT_MOVETO:
                bezier_count = 0;
                path.MoveTo(x, y);
                break;
            case FPDF_SEGMENT_LINETO:
                bezier_count = 0;
                path.LineTo(x, y);
                break;
            case FPDF_SEGMENT_BEZIERTO:
                if (bezier_count < 3) {
                    bezier_pts[bezier_count++] = {x, y};
                }
                if (bezier_count == 3) {
                    path.CurveTo(
                        bezier_pts[0].x, bezier_pts[0].y,
                        bezier_pts[1].x, bezier_pts[1].y,
                        bezier_pts[2].x, bezier_pts[2].y);
                    bezier_count = 0;
                }
                break;
            default:
                bezier_count = 0;
                break;
        }

        if (FPDFPathSegment_GetClose(seg)) {
            path.ClosePath();
            bezier_count = 0;
        }
    }

    return path;
}

WzStrokeState ExtractStrokeState(FPDF_PAGEOBJECT obj) {
    WzStrokeState stroke;

    float width = 1.0f;
    if (FPDFPageObj_GetStrokeWidth(obj, &width)) {
        stroke.line_width = width;
    }

    const int line_cap = FPDFPageObj_GetLineCap(obj);
    const WzLineCap cap = MapLineCap(line_cap);
    stroke.start_cap = cap;
    stroke.dash_cap = cap;
    stroke.end_cap = cap;

    const int line_join = FPDFPageObj_GetLineJoin(obj);
    stroke.line_join = MapLineJoin(line_join);

    float phase = 0.0f;
    if (FPDFPageObj_GetDashPhase(obj, &phase)) {
        stroke.dash_phase = phase;
    }

    const int dash_count = FPDFPageObj_GetDashCount(obj);
    if (dash_count > 0) {
        stroke.dashes.resize(static_cast<size_t>(dash_count));
        if (!FPDFPageObj_GetDashArray(obj, stroke.dashes.data(), stroke.dashes.size())) {
            stroke.dashes.clear();
        }
    }

    return stroke;
}

WzMatrix ExtractMatrix(FPDF_PAGEOBJECT obj) {
    WzMatrix ctm{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    FS_MATRIX mtx{};
    if (FPDFPageObj_GetMatrix(obj, &mtx)) {
        ctm.a = mtx.a;
        ctm.b = mtx.b;
        ctm.c = mtx.c;
        ctm.d = mtx.d;
        ctm.e = mtx.e;
        ctm.f = mtx.f;
    }
    return ctm;
}

}  // namespace

std::vector<WzDrawingItem> GetDrawingsFromPdfium(FPDF_PAGE page) {
    std::vector<WzDrawingItem> drawings;
    if (!page) {
        return drawings;
    }

    const float page_height = FPDF_GetPageHeightF(page);
    const int object_count = FPDFPage_CountObjects(page);
    if (object_count <= 0) {
        return drawings;
    }

    drawings.reserve(static_cast<size_t>(object_count));

    for (int i = 0; i < object_count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj) {
            continue;
        }

        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) {
            continue;
        }

        float left = 0.0f;
        float bottom = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        if (!FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top)) {
            continue;
        }

        const WzRect bounds = ConvertBoundsToTopDown(left, bottom, right, top, page_height);
        const WzPath path = ExtractPath(obj, page_height);
        const WzMatrix ctm = ExtractMatrix(obj);

        int fill_mode = FPDF_FILLMODE_NONE;
        FPDF_BOOL stroke_enabled = 0;
        FPDFPath_GetDrawMode(obj, &fill_mode, &stroke_enabled);

        unsigned int fr = 0, fg = 0, fb = 0, fa = 0;
        const FPDF_BOOL has_fill = FPDFPageObj_GetFillColor(obj, &fr, &fg, &fb, &fa);

        unsigned int sr = 0, sg = 0, sb = 0, sa = 0;
        const FPDF_BOOL has_stroke = FPDFPageObj_GetStrokeColor(obj, &sr, &sg, &sb, &sa);

        if (fill_mode != FPDF_FILLMODE_NONE && has_fill && fa > 0) {
            WzDrawingItem item;
            item.type = "fill";
            item.path = path;
            item.ctm = ctm;
            item.fill_color = MakeColor(fr, fg, fb, fa);
            item.scissor_clip = bounds;
            drawings.push_back(std::move(item));
        }

        if (stroke_enabled && has_stroke && sa > 0) {
            WzDrawingItem item;
            item.type = "stroke";
            item.path = path;
            item.ctm = ctm;
            item.stroke_color = MakeColor(sr, sg, sb, sa);
            item.stroke = ExtractStrokeState(obj);
            item.scissor_clip = bounds;
            drawings.push_back(std::move(item));
        }
    }

    return drawings;
}

}  // namespace Winnerz
