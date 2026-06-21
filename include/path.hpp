#pragma once

#include <vector>
#include <cmath>

namespace Winnerz {


struct WzPoint {
    float x, y;
};

struct WzRect {
    float x0, y0, x1, y1;

    bool IsHorizontalOverlap(const WzRect& other) const {
        return std::max(x0, other.x0) < std::min(x1, other.x1);
    }
};


enum class WzLineCap {
    Butt = 0,       
    Round = 1,      
    Square = 2,     
    Triangle = 3    
};

enum class WzLineJoin {
    Miter = 0,      
    Round = 1,     
    Bevel = 2       
};

struct WzStrokeState {
    float line_width = 1.0f;
    WzLineCap start_cap = WzLineCap::Butt;
    WzLineCap dash_cap = WzLineCap::Butt;
    WzLineCap end_cap = WzLineCap::Butt;
    WzLineJoin line_join = WzLineJoin::Miter;
    
    float miter_limit = 10.0f;
    float dash_phase = 0.0f;
    
    std::vector<float> dashes; 
};


enum class WzPathCmd {
    MoveTo,     
    LineTo,     
    QuadTo,     
    CurveTo,    
    ClosePath   
};

struct WzPathElement {
    WzPathCmd cmd;
    WzPoint pts[3]; 
};


class WzPath {
public:
    WzPath() = default;
    ~WzPath() = default;

    void MoveTo(float x, float y) {
        elements.push_back({ WzPathCmd::MoveTo, { {x, y}, {0,0}, {0,0} } });
    }

    void LineTo(float x, float y) {
        elements.push_back({ WzPathCmd::LineTo, { {x, y}, {0,0}, {0,0} } });
    }

    void QuadTo(float x1, float y1, float x2, float y2) {
        elements.push_back({ WzPathCmd::QuadTo, { {x1, y1}, {x2, y2}, {0,0} } });
    }

    void CurveTo(float x1, float y1, float x2, float y2, float x3, float y3) {
        elements.push_back({ WzPathCmd::CurveTo, { {x1, y1}, {x2, y2}, {x3, y3} } });
    }

    void ClosePath() {
        elements.push_back({ WzPathCmd::ClosePath, { {0,0}, {0,0}, {0,0} } });
    }

    void RectTo(float x0, float y0, float x1, float y1) {
        MoveTo(x0, y0);
        LineTo(x1, y0);
        LineTo(x1, y1);
        LineTo(x0, y1);
        ClosePath();
    }

    void RectTo(const WzRect& rect) {
        RectTo(rect.x0, rect.y0, rect.x1, rect.y1);
    }

    bool IsEmpty() const {
        return elements.empty();
    }

    WzPoint GetCurrentPoint() const {
        if (elements.empty()) return {0.0f, 0.0f};
        
        const auto& last = elements.back();
        switch (last.cmd) {
            case WzPathCmd::MoveTo:
            case WzPathCmd::LineTo:   return last.pts[0];
            case WzPathCmd::QuadTo:   return last.pts[1];
            case WzPathCmd::CurveTo:  return last.pts[2];
            case WzPathCmd::ClosePath:
                for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
                    if (it->cmd == WzPathCmd::MoveTo) return it->pts[0];
                }
                return {0.0f, 0.0f};
        }
        return {0.0f, 0.0f};
    }

    std::vector<WzPathElement> elements;
};

} // namespace Winnerz