#include "geometry.hpp" // Sử dụng lại Struct Rect của chúng ta
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>

// =========================================================================
// CÁCH 1: VẼ "CHẾT" LÊN TRANG (FLATTENED PATH) - KHUYÊN DÙNG
// Đây là cách tốt nhất để vẽ Bounding Box bao quanh chữ (giống Redact).
// Hình chữ nhật sẽ trở thành một phần của tài liệu, không bị xê dịch.
// =========================================================================
void DrawRectPath(FPDF_PAGE page, const Rect& bbox, int r, int g, int b, float line_width) {
    if (!page) return;

    // 1. Tạo một Path Object mới, bắt đầu từ góc dưới-trái
    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(bbox.x0, bbox.y0);

    // 2. Vẽ 3 đường thẳng nối các góc để tạo thành hình chữ nhật
    FPDFPath_LineTo(path, bbox.x1, bbox.y0); // Kéo sang góc dưới-phải
    FPDFPath_LineTo(path, bbox.x1, bbox.y1); // Kéo lên góc trên-phải
    FPDFPath_LineTo(path, bbox.x0, bbox.y1); // Kéo sang góc trên-trái
    FPDFPath_Close(path);                    // Tự động nối về điểm bắt đầu

    // 3. Thiết lập màu viền (Stroke Color) và Alpha (255 = đục hoàn toàn)
    FPDFPageObj_SetStrokeColor(path, r, g, b, 255);

    // 4. Thiết lập độ dày đường viền
    FPDFPageObj_SetStrokeWidth(path, line_width);

    // 5. Chế độ vẽ: 0 = Không tô nền (No Fill), 1 = Vẽ viền (Stroke)
    FPDFPath_SetDrawMode(path, 0, 1);

    // 6. Nhét Object này vào trang
    FPDFPage_InsertObject(page, path);

    // 7. QUAN TRỌNG NHẤT: Báo cho PDFium biết nội dung trang đã thay đổi để lưu lại
    FPDFPage_GenerateContent(page);
}

// =========================================================================
// CÁCH 2: TẠO SQUARE ANNOTATION (Y HỆT CODE MUPDF Ở TRÊN)
// Tạo một khung chữ nhật nổi trên trang giấy, người dùng có thể click vào.
// =========================================================================
void AddSquareAnnotation(FPDF_PAGE page, const Rect& bbox, int r, int g, int b) {
    if (!page) return;

    // 1. Khởi tạo một Annotation loại SQUARE
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE);
    if (!annot) return;

    // 2. Thiết lập khung Bounding Box cho Annotation
    // Lưu ý: PDFium dùng FS_RECTF {left, bottom, right, top}
    FS_RECTF rect = { bbox.x0, bbox.y0, bbox.x1, bbox.y1 };
    FPDFAnnot_SetRect(annot, &rect);

    // 3. Thiết lập màu sắc (Màu viền)
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);

    // 4. Đóng và lưu Annotation
    FPDFPage_CloseAnnot(annot);
    
    // (Lưu ý: Để thiết lập Border Width cho Annotation trong PDFium, 
    // cần phải can thiệp sâu vào Dictionary của nó qua FPDF_GetAnnotDict, 
    // do API public của PDFium giới hạn việc này. Vì vậy, Cách 1 luôn được ưu tiên hơn).
}