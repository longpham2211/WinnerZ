#include "geometry.hpp"
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>

void DrawRectPath(FPDF_PAGE page, const Rect& bbox, int r, int g, int b, float line_width) {
    if (!page) return;

    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(bbox.x0, bbox.y0);

    FPDFPath_LineTo(path, bbox.x1, bbox.y0);
    FPDFPath_LineTo(path, bbox.x1, bbox.y1); 
    FPDFPath_LineTo(path, bbox.x0, bbox.y1); 
    FPDFPath_Close(path);                    
    FPDFPageObj_SetStrokeColor(path, r, g, b, 255);
    FPDFPageObj_SetStrokeWidth(path, line_width);
    FPDFPath_SetDrawMode(path, 0, 1);
    FPDFPage_InsertObject(page, path);
    FPDFPage_GenerateContent(page);
}


void AddSquareAnnotation(FPDF_PAGE page, const Rect& bbox, int r, int g, int b) {
    if (!page) return;

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE);
    if (!annot) return;
    FS_RECTF rect = { bbox.x0, bbox.y0, bbox.x1, bbox.y1 };
    FPDFAnnot_SetRect(annot, &rect);

    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);

    FPDFPage_CloseAnnot(annot);
}