#pragma once

#include <vector>
#include <string>
#include "path.hpp" // Kế thừa các struct WzPath, WzRect, WzStrokeState từ Task trước
#include "fpdfview.h"

namespace Winnerz {

// ==========================================
// 1. TỌA ĐỘ VÀ MÀU SẮC (Graphic State)
// ==========================================

// Ma trận biến đổi (CTM - Current Transformation Matrix)
// Rất quan trọng để biết hình bị xoay, lật hay thu phóng bao nhiêu
struct WzMatrix { 
    float a, b, c, d, e, f; 
};

// Màu sắc cơ bản 
struct WzColor {
    std::vector<float> components; // Chứa RGB hoặc CMYK
    float alpha = 1.0f; // Độ trong suốt
};

// ==========================================
// 2. CẤU TRÚC ĐẦU RA CỦA GET_DRAWINGS()
// Map 1:1 với cái dictionary mà PyMuPDF trả về
// ==========================================
struct WzDrawingItem {
    std::string type;       // "fill", "stroke", hoặc "clip"
    WzPath path;            // Danh sách các lệnh MoveTo, LineTo...
    WzMatrix ctm;           // Ma trận gốc của hình

    WzColor fill_color;     // Nếu type là "fill" thì có màu này
    WzColor stroke_color;   // Nếu type là "stroke" thì có màu này
    WzStrokeState stroke;   // Độ dày, nét đứt (lấy từ path.hpp)

    WzRect scissor_clip;    // Khung cắt (nếu có)
};

// ==========================================
// 3. LỚP THIẾT BỊ ẢO (VIRTUAL DEVICE)
// Đây là "linh hồn" được clone từ struct fz_device
// ==========================================
class WzDevice {
public:
    virtual ~WzDevice() = default;

    // Bắt lệnh tô màu (Clone từ fill_path)
    virtual void FillPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzColor& color) = 0;

    // Bắt lệnh vẽ viền (Clone từ stroke_path)
    virtual void StrokePath(const WzPath& path, const WzStrokeState& stroke, const WzMatrix& ctm, const WzColor& color) = 0;

    // Bắt lệnh cắt vùng (Clone từ clip_path)
    virtual void ClipPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzRect& scissor) = 0;
};

// ==========================================
// 4. BỘ TRÍCH XUẤT DRAWING (DRAWING EXTRACTOR)
// Chính là lõi C++ của hàm page.get_drawings()
// ==========================================
class WzDrawingExtractor : public WzDevice {
public:
    // Dữ liệu "phẳng" trả về cho đại ca (Y chang List của PyMuPDF)
    std::vector<WzDrawingItem> drawings;

    // Khi PDF engine định "Tô màu", ta chặn lại và lưu vào mảng
    void FillPath(const WzPath& path, bool even_odd, const WzMatrix& ctm, const WzColor& color) override {
        WzDrawingItem item;
        item.type = "fill";
        item.path = path;
        item.ctm = ctm;
        item.fill_color = color;
        drawings.push_back(item);
    }

    // Khi PDF engine định "Vẽ viền", ta cũng chặn lại và lưu vào mảng
    void StrokePath(const WzPath& path, const WzStrokeState& stroke, const WzMatrix& ctm, const WzColor& color) override {
        WzDrawingItem item;
        item.type = "stroke";
        item.path = path;
        item.ctm = ctm;
        item.stroke_color = color;
        item.stroke = stroke; // Lấy được độ dày, nét đứt
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

// ==========================================
// 5. API: Lấy danh sách drawings từ PDFium
// Tương tự page.get_drawings() nhưng trả về kiểu WinnerZ.
// ==========================================
std::vector<WzDrawingItem> GetDrawingsFromPdfium(FPDF_PAGE page);

} // namespace Winnerz