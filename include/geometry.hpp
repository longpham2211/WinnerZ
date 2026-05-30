#ifndef PDF_GEOMETRY_HPP
#define PDF_GEOMETRY_HPP

#include <algorithm> // Để dùng std::min, std::max

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

// 1. POINT: Điểm trên trục tọa độ 2D
struct Point {
    float x, y;
};

// 2. RECT: Khung bao chữ nhật (Quan trọng nhất cho cả Redact và Dict)
// Luôn đảm bảo x0 <= x1 và y0 <= y1
struct Rect {
    float x0, y0; // Góc trên-trái (hoặc dưới-trái tùy hệ tọa độ)
    float x1, y1; // Góc dưới-phải (hoặc trên-phải)

    float Width() const { return x1 - x0; }
    float Height() const { return y1 - y0; }
    float Area() const { return Width() * Height(); }

    // Tinh hoa 1: Kiểm tra xem 2 hình chữ nhật có ĐÈ LÊN NHAU không?
    // Dùng cho hàm IsOverlap() trong class Redactor của bạn
    bool Overlaps(const Rect& other) const {
        if (x0 >= other.x1 || x1 <= other.x0) return false;
        if (y0 >= other.y1 || y1 <= other.y0) return false;
        return true;
    }

    // Tinh hoa 2: Tìm phần giao nhau của 2 hình chữ nhật
    // Dùng để tính % diện tích bị che (Partial Overlap trong Redact)
    Rect Intersect(const Rect& other) const {
        float nx0 = std::max(x0, other.x0);
        float ny0 = std::max(y0, other.y0);
        float nx1 = std::min(x1, other.x1);
        float ny1 = std::min(y1, other.y1);
        if (nx0 >= nx1 || ny0 >= ny1) return {0, 0, 0, 0}; // Không giao nhau
        return {nx0, ny0, nx1, ny1};
    }

    // Tinh hoa 3: Gộp 2 hình chữ nhật lại thành 1 hình to hơn
    // Dùng cho get("dict"): Khi bạn có 2 chữ "H" và "e", bạn Union 2 Rect lại sẽ ra Rect của chữ "He"
    Rect Union(const Rect& other) const {
        return {
            std::min(x0, other.x0),
            std::min(y0, other.y0),
            std::max(x1, other.x1),
            std::max(y1, other.y1)
        };
    }

    // Kiểm tra 1 điểm có nằm trong hộp không
    bool Contains(const Point& p) const {
        return (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1);
    }
};

// 3. QUAD: Tứ giác (Dùng cho vùng Redact nghiêng hoặc Text bị xoay)
// PDF không phải lúc nào cũng là hình chữ nhật thẳng đứng.
struct Quad {
    Point ul; // Upper-left (Trên-trái)
    Point ur; // Upper-right (Trên-phải)
    Point ll; // Lower-left (Dưới-trái)
    Point lr; // Lower-right (Dưới-phải)

    // Chuyển tứ giác bị nghiêng thành Bounding Box (Khung chữ nhật bao quanh)
    Rect ToRect() const {
        return {
            std::min({ul.x, ur.x, ll.x, lr.x}),
            std::min({ul.y, ur.y, ll.y, lr.y}),
            std::max({ul.x, ur.x, ll.x, lr.x}),
            std::max({ul.y, ur.y, ll.y, lr.y})
        };
    }
};

// 4. MATRIX: Ma trận Transform 3x3 (A B C D E F)
// Dùng khi trang PDF bị xoay (Rotate = 90, 180, 270) hoặc scale.
// Cực kỳ quan trọng để quy đổi tọa độ.
struct Matrix {
    float a, b, c, d, e, f;

    // Biến đổi tọa độ của 1 điểm theo ma trận (Nhân ma trận)
    Point Transform(const Point& p) const {
        return {
            p.x * a + p.y * c + e,
            p.x * b + p.y * d + f
        };
    }

    // Biến đổi toàn bộ Rect
    Rect Transform(const Rect& r) const {
        Point p1{r.x0, r.y0};
        Point p2{r.x1, r.y0};
        Point p3{r.x0, r.y1};
        Point p4{r.x1, r.y1};
        p1 = Transform(p1);
        p2 = Transform(p2);
        p3 = Transform(p3);
        p4 = Transform(p4);
        return {
            std::min({p1.x, p2.x, p3.x, p4.x}),
            std::min({p1.y, p2.y, p3.y, p4.y}),
            std::max({p1.x, p2.x, p3.x, p4.x}),
            std::max({p1.y, p2.y, p3.y, p4.y})
        };
    }
};

#endif // PDF_GEOMETRY_HPP