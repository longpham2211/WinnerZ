#ifndef PDF_GEOMETRY_HPP
#define PDF_GEOMETRY_HPP

#include <algorithm> // Để dùng std::min, std::max

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

struct Point {
    float x, y;
};
struct Rect {
    float x0, y0; 
    float x1, y1;

    float Width() const { return x1 - x0; }
    float Height() const { return y1 - y0; }
    float Area() const { return Width() * Height(); }
    bool Overlaps(const Rect& other) const {
        if (x0 >= other.x1 || x1 <= other.x0) return false;
        if (y0 >= other.y1 || y1 <= other.y0) return false;
        return true;
    }


    Rect Intersect(const Rect& other) const {
        float nx0 = std::max(x0, other.x0);
        float ny0 = std::max(y0, other.y0);
        float nx1 = std::min(x1, other.x1);
        float ny1 = std::min(y1, other.y1);
        if (nx0 >= nx1 || ny0 >= ny1) return {0, 0, 0, 0}; 
        return {nx0, ny0, nx1, ny1};
    }

    
    Rect Union(const Rect& other) const {
        return {
            std::min(x0, other.x0),
            std::min(y0, other.y0),
            std::max(x1, other.x1),
            std::max(y1, other.y1)
        };
    }

    bool Contains(const Point& p) const {
        return (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1);
    }
};

struct Quad {
    Point ul; // Upper-left 
    Point ur; // Upper-right 
    Point ll; // Lower-left 
    Point lr; // Lower-right 

    Rect ToRect() const {
        return {
            std::min({ul.x, ur.x, ll.x, lr.x}),
            std::min({ul.y, ur.y, ll.y, lr.y}),
            std::max({ul.x, ur.x, ll.x, lr.x}),
            std::max({ul.y, ur.y, ll.y, lr.y})
        };
    }
};

struct Matrix {
    float a, b, c, d, e, f;

    Point Transform(const Point& p) const {
        return {
            p.x * a + p.y * c + e,
            p.x * b + p.y * d + f
        };
    }

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