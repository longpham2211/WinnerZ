import argparse
import json
import math
from typing import Any, Dict, List, Tuple

import fitz
import winnerz


def _rect_tuple(r: Any) -> Tuple[float, float, float, float]:
    if hasattr(r, "x0"):
        return (float(r.x0), float(r.y0), float(r.x1), float(r.y1))
    return (float(r[0]), float(r[1]), float(r[2]), float(r[3]))


def _process_pdf_hybrid_engine(page: Any) -> List[Dict[str, Any]]:
    data = page.get_text("dict", sort=True)
    groups: List[Dict[str, Any]] = []
    line_id = 0

    for block in data.get("blocks", []):
        if block.get("type") != 0:
            continue

        block_lines: List[Dict[str, Any]] = []
        for line in block.get("lines", []):
            spans = line.get("spans", [])
            if not spans:
                continue

            text = "".join((s.get("text") or "") for s in spans).strip()
            if not text:
                continue

            bbox = line.get("bbox")
            if bbox and len(bbox) >= 4:
                x0, y0, x1, y1 = [float(v) for v in bbox[:4]]
            else:
                x0 = min(float(s.get("bbox", [0, 0, 0, 0])[0]) for s in spans)
                y0 = min(float(s.get("bbox", [0, 0, 0, 0])[1]) for s in spans)
                x1 = max(float(s.get("bbox", [0, 0, 0, 0])[2]) for s in spans)
                y1 = max(float(s.get("bbox", [0, 0, 0, 0])[3]) for s in spans)

            sizes = [float(s.get("size", 0.0)) for s in spans if s.get("size") is not None]
            f_size = max(sizes) if sizes else max(1.0, y1 - y0)

            colors: List[int] = []
            for s in spans:
                try:
                    colors.append(int(s.get("color", 0)))
                except Exception:
                    pass
            dominant_color = max(set(colors), key=colors.count) if colors else 0
            r = (dominant_color >> 16) & 0xFF
            g = (dominant_color >> 8) & 0xFF
            b = dominant_color & 0xFF

            block_lines.append(
                {
                    "id": line_id,
                    "bbox": (x0, y0, x1, y1),
                    "text": text,
                    "f_size": float(f_size),
                    "color": f"#{r:02x}{g:02x}{b:02x}",
                }
            )
            line_id += 1

        if block_lines:
            groups.append({"is_para": len(block_lines) > 1, "lines": block_lines})

    return groups


def _detect_text_colors(page: Any, groups: List[Dict[str, Any]], matrix_ctor: Any, rect_ctor: Any) -> None:
    filled_rects: List[Dict[str, Any]] = []
    try:
        for drawing in page.get_drawings():
            fill = drawing.get("fill")
            if fill is None:
                continue
            rect = drawing.get("rect")
            if rect is None:
                continue
            r, g, b = float(fill[0]), float(fill[1]), float(fill[2])
            brightness = (r * 299 + g * 587 + b * 114) * 255 / 1000
            rx0, ry0, rx1, ry1 = _rect_tuple(rect)
            area = (rx1 - rx0) * (ry1 - ry0)
            filled_rects.append({"rect": rect, "brightness": brightness, "area": area})
        filled_rects.sort(key=lambda x: -x["area"])
    except Exception:
        filled_rects = []

    pix_cache: Dict[str, Any] = {}

    def get_pixel_brightness(bx0: float, by0: float, bx1: float, by1: float) -> float:
        if "pix" not in pix_cache:
            mat = matrix_ctor(0.5, 0.5)
            pix_cache["pix"] = page.get_pixmap(matrix=mat, clip=page.rect)
            pix_cache["x0"] = float(page.rect.x0)
            pix_cache["y0"] = float(page.rect.y0)
        pix = pix_cache["pix"]
        x0_off = pix_cache["x0"]
        y0_off = pix_cache["y0"]
        vals: List[float] = []
        for fx in (0.15, 0.5, 0.85):
            for fy in (0.2, 0.8):
                px = max(0, min(int((bx0 + (bx1 - bx0) * fx - x0_off) * 0.5), pix.width - 1))
                py = max(0, min(int((by0 + (by1 - by0) * fy - y0_off) * 0.5), pix.height - 1))
                r, g, b = pix.pixel(px, py)[:3]
                vals.append((r * 299 + g * 587 + b * 114) / 1000)
        vals.sort()
        return vals[len(vals) // 2]

    for group in groups:
        for line in group["lines"]:
            bx0, by0, bx1, by1 = line["bbox"]
            line_rect = rect_ctor(bx0, by0, bx1, by1)
            line_area = max((bx1 - bx0) * (by1 - by0), 1.0)

            brightness = None
            best_overlap = 0.0
            for fr in filled_rects:
                inter = line_rect & fr["rect"]
                if inter.is_empty:
                    continue
                overlap = (inter.x1 - inter.x0) * (inter.y1 - inter.y0)
                overlap_ratio = overlap / line_area
                if overlap_ratio > best_overlap:
                    best_overlap = overlap_ratio
                    brightness = fr["brightness"]

            if brightness is None or best_overlap < 0.3:
                brightness = get_pixel_brightness(bx0, by0, bx1, by1)

            if brightness < 128:
                line["color"] = "#ffffff"


def _flatten_groups(groups: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for g in groups:
        for line in g["lines"]:
            out.append(line)
    return out


def _bbox_delta(a: Tuple[float, float, float, float], b: Tuple[float, float, float, float]) -> float:
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]), abs(a[3] - b[3]))


def _compare_page(fitz_page: Any, winnerz_page: Any) -> Dict[str, Any]:
    fitz_groups = _process_pdf_hybrid_engine(fitz_page)
    winnerz_groups = _process_pdf_hybrid_engine(winnerz_page)

    _detect_text_colors(fitz_page, fitz_groups, fitz.Matrix, fitz.Rect)
    _detect_text_colors(winnerz_page, winnerz_groups, winnerz.Matrix, winnerz.Rect)

    fitz_lines = _flatten_groups(fitz_groups)
    winnerz_lines = _flatten_groups(winnerz_groups)

    line_count_mismatch = len(fitz_lines) - len(winnerz_lines)
    compare_len = min(len(fitz_lines), len(winnerz_lines))

    text_mismatches = 0
    color_mismatches = 0
    max_bbox_delta = 0.0

    for i in range(compare_len):
        fl = fitz_lines[i]
        wl = winnerz_lines[i]
        if fl["text"] != wl["text"]:
            text_mismatches += 1
        if fl["color"] != wl["color"]:
            color_mismatches += 1
        max_bbox_delta = max(max_bbox_delta, _bbox_delta(fl["bbox"], wl["bbox"]))

    fitz_rect = _rect_tuple(fitz_page.rect)
    winnerz_rect = _rect_tuple(winnerz_page.rect)

    return {
        "fitz_line_count": len(fitz_lines),
        "winnerz_line_count": len(winnerz_lines),
        "line_count_delta": line_count_mismatch,
        "text_mismatches": text_mismatches,
        "color_mismatches": color_mismatches,
        "max_bbox_delta": max_bbox_delta,
        "rect_delta": _bbox_delta(fitz_rect, winnerz_rect),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare winnerz output against fitz for app-hf flow")
    parser.add_argument("pdf", help="PDF path")
    parser.add_argument("--pages", default="all", help="Page index or 'all'")
    parser.add_argument("--json", dest="json_out", default="", help="Optional JSON report path")
    parser.add_argument("--bbox-eps", type=float, default=1e-4, help="Allowed bbox delta epsilon")
    args = parser.parse_args()

    fitz_doc = fitz.open(args.pdf)
    winnerz_doc = winnerz.open(args.pdf)

    if len(fitz_doc) != len(winnerz_doc):
        print(json.dumps({"error": "page_count_mismatch", "fitz": len(fitz_doc), "winnerz": len(winnerz_doc)}, indent=2))
        return 2

    if args.pages == "all":
        page_indices = list(range(len(fitz_doc)))
    else:
        page_indices = [int(args.pages)]

    report: Dict[str, Any] = {"pdf": args.pdf, "pages": {}}
    total_text_mismatches = 0
    total_color_mismatches = 0
    total_line_delta = 0
    max_bbox_delta = 0.0
    max_rect_delta = 0.0

    for i in page_indices:
        page_report = _compare_page(fitz_doc[i], winnerz_doc[i])
        report["pages"][str(i)] = page_report
        total_text_mismatches += page_report["text_mismatches"]
        total_color_mismatches += page_report["color_mismatches"]
        total_line_delta += abs(page_report["line_count_delta"])
        max_bbox_delta = max(max_bbox_delta, page_report["max_bbox_delta"])
        max_rect_delta = max(max_rect_delta, page_report["rect_delta"])

    report["summary"] = {
        "total_text_mismatches": total_text_mismatches,
        "total_color_mismatches": total_color_mismatches,
        "total_line_delta": total_line_delta,
        "max_bbox_delta": max_bbox_delta,
        "max_rect_delta": max_rect_delta,
        "bbox_epsilon": float(args.bbox_eps),
    }

    rendered = json.dumps(report, indent=2)
    print(rendered)

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fp:
            fp.write(rendered)

    # Strict mode: any mismatch is failure.
    if (
        total_text_mismatches != 0
        or total_color_mismatches != 0
        or total_line_delta != 0
        or max_bbox_delta > float(args.bbox_eps)
        or max_rect_delta != 0.0
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
