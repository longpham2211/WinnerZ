from pathlib import Path
import shutil
import json

import fitz
import winnerz
import pypdfium2 as pdfium


ROOT = Path(__file__).resolve().parents[1]
INPUT_PDF = ROOT / "data" / "ieee-format.pdf"
OUT_DIR = ROOT / "temp_uploads" / "debug_redact_ieee"
OUT_DIR.mkdir(parents=True, exist_ok=True)

WINNERZ_OUT = OUT_DIR / "ieee_winnerz_redacted.pdf"
FITZ_FROM_WINNERZ_OUT = OUT_DIR / "ieee_fitz_redacted_from_winnerz_rects.pdf"
FITZ_NATIVE_OUT = OUT_DIR / "ieee_fitz_redacted_native_rects.pdf"
SUMMARY_JSON = OUT_DIR / "summary.json"

PAGES_TO_RENDER = [4, 5, 6, 7]  # zero-based: pages 5-8
SCALE = 2.0


def extract_text_rects_from_blocks(blocks):
    rects = []
    for block in blocks:
        bbox = None
        block_type = 0

        if isinstance(block, dict):
            bbox = block.get("bbox")
            block_type = int(block.get("type", 0))
        elif isinstance(block, (list, tuple)):
            if len(block) >= 4:
                bbox = block[:4]
            if len(block) >= 7:
                block_type = int(block[6])

        if block_type != 0 or not bbox or len(bbox) < 4:
            continue

        rects.append((float(bbox[0]), float(bbox[1]), float(bbox[2]), float(bbox[3])))
    return rects


def get_rects_by_page_winnerz(pdf_path: Path):
    doc = winnerz.open(str(pdf_path))
    rects_by_page = []
    for page_idx in range(len(doc)):
        blocks = doc[page_idx].get_text("blocks")
        rects_by_page.append(extract_text_rects_from_blocks(blocks))
    return rects_by_page


def get_rects_by_page_fitz(pdf_path: Path):
    doc = fitz.open(str(pdf_path))
    try:
        rects_by_page = []
        for page in doc:
            blocks = page.get_text("blocks")
            rects = []
            for b in blocks:
                if len(b) >= 7 and int(b[6]) == 0:
                    rects.append((float(b[0]), float(b[1]), float(b[2]), float(b[3])))
            rects_by_page.append(rects)
        return rects_by_page
    finally:
        doc.close()


def redact_with_winnerz(input_pdf: Path, output_pdf: Path, rects_by_page):
    shutil.copyfile(input_pdf, output_pdf)
    for page_idx, rects in enumerate(rects_by_page):
        if not rects:
            continue
        # Keep this behavior identical to app-winnerz.
        doc = winnerz.open(str(output_pdf))
        doc[page_idx].redact_text(rects, str(output_pdf), min_overlap_ratio=0.0)


def redact_with_fitz(input_pdf: Path, output_pdf: Path, rects_by_page):
    doc = fitz.open(str(input_pdf))
    try:
        for page_idx, rects in enumerate(rects_by_page):
            if not rects:
                continue
            page = doc[page_idx]
            for r in rects:
                page.add_redact_annot(fitz.Rect(*r), fill=None)
            page.apply_redactions(images=0, graphics=0)
        doc.save(str(output_pdf), clean=True, deflate=True)
    finally:
        doc.close()


def render_pages_fitz(pdf_path: Path, prefix: str):
    doc = fitz.open(str(pdf_path))
    out = []
    try:
        for page_idx in PAGES_TO_RENDER:
            if page_idx >= len(doc):
                continue
            page = doc[page_idx]
            pix = page.get_pixmap(matrix=fitz.Matrix(SCALE, SCALE), alpha=False)
            out_path = OUT_DIR / f"{prefix}_fitz_p{page_idx + 1}.png"
            pix.save(str(out_path))
            out.append(str(out_path))

            rect = page.rect
            clip = fitz.Rect(rect.x0 + rect.width * 0.65, rect.y0, rect.x1, rect.y1)
            pix_clip = page.get_pixmap(matrix=fitz.Matrix(SCALE, SCALE), clip=clip, alpha=False)
            clip_path = OUT_DIR / f"{prefix}_fitz_p{page_idx + 1}_right.png"
            pix_clip.save(str(clip_path))
            out.append(str(clip_path))
    finally:
        doc.close()
    return out


def render_pages_pdfium(pdf_path: Path, prefix: str):
    out = []
    pdf = pdfium.PdfDocument(str(pdf_path))
    try:
        for page_idx in PAGES_TO_RENDER:
            if page_idx >= len(pdf):
                continue
            page = pdf[page_idx]
            try:
                bmp = page.render(scale=SCALE)
                pil = bmp.to_pil().convert("RGB")
                out_path = OUT_DIR / f"{prefix}_pdfium_p{page_idx + 1}.png"
                pil.save(str(out_path))
                out.append(str(out_path))

                w, h = pil.size
                crop = pil.crop((int(w * 0.65), 0, w, h))
                crop_path = OUT_DIR / f"{prefix}_pdfium_p{page_idx + 1}_right.png"
                crop.save(str(crop_path))
                out.append(str(crop_path))
            finally:
                page.close()
    finally:
        pdf.close()
    return out


def count_copyable_text(pdf_path: Path):
    doc = fitz.open(str(pdf_path))
    try:
        per_page = []
        for page in doc:
            txt = page.get_text("text")
            per_page.append(len(txt.strip()))
        return per_page
    finally:
        doc.close()


def main():
    if not INPUT_PDF.exists():
        raise FileNotFoundError(f"Missing input file: {INPUT_PDF}")

    rects_winnerz = get_rects_by_page_winnerz(INPUT_PDF)
    rects_fitz = get_rects_by_page_fitz(INPUT_PDF)

    redact_with_winnerz(INPUT_PDF, WINNERZ_OUT, rects_winnerz)
    redact_with_fitz(INPUT_PDF, FITZ_FROM_WINNERZ_OUT, rects_winnerz)
    redact_with_fitz(INPUT_PDF, FITZ_NATIVE_OUT, rects_fitz)

    rendered = {
        "winnerz_fitz_render": render_pages_fitz(WINNERZ_OUT, "winnerz"),
        "winnerz_pdfium_render": render_pages_pdfium(WINNERZ_OUT, "winnerz"),
        "fitz_same_rects_render": render_pages_fitz(FITZ_FROM_WINNERZ_OUT, "fitz_from_winnerz"),
        "fitz_native_render": render_pages_fitz(FITZ_NATIVE_OUT, "fitz_native"),
    }

    summary = {
        "input": str(INPUT_PDF),
        "outputs": {
            "winnerz": str(WINNERZ_OUT),
            "fitz_from_winnerz_rects": str(FITZ_FROM_WINNERZ_OUT),
            "fitz_native": str(FITZ_NATIVE_OUT),
        },
        "rect_counts": {
            "winnerz": [len(r) for r in rects_winnerz],
            "fitz": [len(r) for r in rects_fitz],
        },
        "copyable_text_char_count_per_page": {
            "winnerz": count_copyable_text(WINNERZ_OUT),
            "fitz_from_winnerz_rects": count_copyable_text(FITZ_FROM_WINNERZ_OUT),
            "fitz_native": count_copyable_text(FITZ_NATIVE_OUT),
        },
        "rendered_images": rendered,
    }

    SUMMARY_JSON.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
