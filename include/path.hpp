#pragma once

#include <vector>
#include <cmath>

namespace Winnerz {

// ==========================================
// 1. CẤU TRÚC HÌNH HỌC CƠ BẢN
// ==========================================
struct WzPoint {
    float x, y;
};

struct WzRect {
    float x0, y0, x1, y1;

    // Tiện ích kiểm tra giao thoa ngang (X-Overlap) cho thuật toán Dict sau này
    bool IsHorizontalOverlap(const WzRect& other) const {
        return std::max(x0, other.x0) < std::min(x1, other.x1);
    }
};

// ==========================================
// 2. TRẠNG THÁI NÉT VẼ (STROKE STATE)
// Map 1:1 từ fz_linecap và fz_linejoin
// ==========================================
enum class WzLineCap {
    Butt = 0,       // Đầu bằng (Cắt ngang)
    Round = 1,      // Đầu tròn
    Square = 2,     // Đầu vuông (Lồi ra một chút)
    Triangle = 3    // Đầu nhọn
};

enum class WzLineJoin {
    Miter = 0,      // Góc nhọn (Vát theo góc)
    Round = 1,      // Góc bo tròn
    Bevel = 2       // Góc cắt vát (Bằng)
};

// Map 1:1 từ fz_stroke_state
struct WzStrokeState {
    float line_width = 1.0f;
    WzLineCap start_cap = WzLineCap::Butt;
    WzLineCap dash_cap = WzLineCap::Butt;
    WzLineCap end_cap = WzLineCap::Butt;
    WzLineJoin line_join = WzLineJoin::Miter;
    
    float miter_limit = 10.0f;
    float dash_phase = 0.0f;
    
    // Thay vì dùng mảng C cấp phát động (FZ_FLEXIBLE_ARRAY), ta dùng std::vector cực nhàn
    std::vector<float> dashes; 
};

// ==========================================
// 3. LỆNH VẼ VÀ CẤU TRÚC ĐƯỜNG DẪN (PATH)
// ==========================================
enum class WzPathCmd {
    MoveTo,     // Nhấc bút đến tọa độ mới
    LineTo,     // Kẻ một đường thẳng
    QuadTo,     // Kẻ đường cong Bezier bậc 2 (1 điểm neo)
    CurveTo,    // Kẻ đường cong Bezier bậc 3 (2 điểm neo)
    ClosePath   // Đóng vùng (Kẻ về điểm MoveTo gần nhất)
};

struct WzPathElement {
    WzPathCmd cmd;
    // Mỗi lệnh có tối đa 3 điểm tọa độ (CurveTo cần điểm neo 1, điểm neo 2 và điểm đích).
    // Với MoveTo/LineTo, ta chỉ dùng pts[0].
    WzPoint pts[3]; 
};

// ==========================================
// 4. LỚP WzPath THỰC THỤ
// Đóng vai trò như fz_path nhưng là C++ thuần
// ==========================================
class WzPath {
public:
    WzPath() = default;
    ~WzPath() = default;

    // Các lệnh vẽ cơ bản
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

    // Lệnh "Macro" vẽ hình chữ nhật (Map từ fz_rectto)
    // Tự động bung ra thành 4 lệnh gạch và 1 lệnh đóng
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

    // Các hàm tiện ích
    bool IsEmpty() const {
        return elements.empty();
    }

    WzPoint GetCurrentPoint() const {
        if (elements.empty()) return {0.0f, 0.0f};
        
        // Tìm điểm cuối cùng tùy thuộc vào lệnh trước đó
        const auto& last = elements.back();
        switch (last.cmd) {
            case WzPathCmd::MoveTo:
            case WzPathCmd::LineTo:   return last.pts[0];
            case WzPathCmd::QuadTo:   return last.pts[1];
            case WzPathCmd::CurveTo:  return last.pts[2];
            case WzPathCmd::ClosePath:
                // Nếu là ClosePath thì phải tìm ngược lại lệnh MoveTo gần nhất
                for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
                    if (it->cmd == WzPathCmd::MoveTo) return it->pts[0];
                }
                return {0.0f, 0.0f};
        }
        return {0.0f, 0.0f};
    }

    // Dữ liệu nội bộ: Lưu toàn bộ nét vẽ
    std::vector<WzPathElement> elements;
};

} // namespace Winnerz