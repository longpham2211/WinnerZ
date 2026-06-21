#pragma once

#include <vector>
#include <string>
#include "path.hpp"
#include "fpdfview.h"

namespace Winnerz {

struct WzMatrix { 
    float a, b, c, d, e, f; 
};

struct WzColor {
    std::vector<float> components;
    float alpha = 1.0f;
};

struct WzDrawingItem {
    std::string type;
    WzPath path;
    WzMatrix ctm;

    WzColor fill_color;
    WzColor stroke_color;
    WzStrokeState stroke;

    WzRect scissor_clip;
};

class WzDevice {
public:
    virtual ~WzDevice() = default;

    virtual void FillPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzColor& color) = 0;

    virtual void StrokePath(const WzPath& path, const WzStrokeState& stroke, const WzMatrix& ctm, const WzColor& color) = 0;

    virtual void ClipPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzRect& scissor) = 0;
};

class WzDrawingExtractor : public WzDevice {
public:
    std::vector<WzDrawingItem> drawings;

    void FillPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzColor& color) override {
        WzDrawingItem item;
        item.type = "fill";
        item.path = path;
        item.ctm = ctm;
        item.fill_color = color;
        drawings.push_back(item);
    }

    void StrokePath(const WzPath& path, const WzStrokeState& stroke, const WzMatrix& ctm, const WzColor& color) override {
        WzDrawingItem item;
        item.type = "stroke";
        item.path = path;
        item.ctm = ctm;
        item.stroke_color = color;
        item.stroke = stroke;
        drawings.push_back(item);
    }

    void ClipPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzRect& scissor) override {
        WzDrawingItem item;
        item.type = "clip";
        item.path = path;
        item.ctm = ctm;
        item.scissor_clip = scissor;
        drawings.push_back(item);
    }
};

std::vector<WzDrawingItem> GetDrawingsFromPdfium(FPDF_PAGE page);

} // namespace Winnerz