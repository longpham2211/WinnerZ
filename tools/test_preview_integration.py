from pathlib import Path
import random

import winnerz
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
PDF_PATH = ROOT / "data" / "ieee-format.pdf"
OUT_DIR = ROOT / "temp_uploads" / "preview_integration_test"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def non_white_ratio(pix, sample_count=4000):
    total_pixels = pix.width * pix.height
    if total_pixels <= 0:
        return 0.0

    rnd = random.Random(12345)
    non_white = 0
    count = min(sample_count, total_pixels)

    for _ in range(count):
        x = rnd.randrange(0, pix.width)
        y = rnd.randrange(0, pix.height)
        r, g, b, *_ = pix.pixel(x, y)
        if r < 250 or g < 250 or b < 250:
            non_white += 1

    return non_white / float(count)


def save_pixmap_png(pix, path):
    img = Image.frombytes("RGBA", (pix.width, pix.height), pix.samples)
    img.save(str(path), format="PNG")


def main():
    if not PDF_PATH.exists():
        raise FileNotFoundError(f"Missing input PDF: {PDF_PATH}")

    doc = winnerz.open(str(PDF_PATH))
    page = doc[5]

    full_pix = page.get_pixmap(matrix=winnerz.Matrix(1.0, 1.0))
    clip_rect = winnerz.Rect(100, 120, 300, 360)
    clip_pix = page.get_pixmap(matrix=winnerz.Matrix(1.0, 1.0), clip=clip_rect)
    zoom_pix = page.get_pixmap(matrix=winnerz.Matrix(2.0, 2.0))

    full_ratio = non_white_ratio(full_pix)
    clip_ratio = non_white_ratio(clip_pix)
    zoom_ratio = non_white_ratio(zoom_pix)

    save_pixmap_png(full_pix, OUT_DIR / "page6_full.png")
    save_pixmap_png(clip_pix, OUT_DIR / "page6_clip.png")
    save_pixmap_png(zoom_pix, OUT_DIR / "page6_zoom2x.png")

    assert full_pix.width > 0 and full_pix.height > 0
    assert clip_pix.width > 0 and clip_pix.height > 0
    assert zoom_pix.width > full_pix.width and zoom_pix.height > full_pix.height

    # The preview should not be a fully white placeholder image.
    assert full_ratio > 0.005, f"Full preview looks blank (ratio={full_ratio:.6f})"
    assert clip_ratio > 0.005, f"Clip preview looks blank (ratio={clip_ratio:.6f})"
    assert zoom_ratio > 0.005, f"Zoom preview looks blank (ratio={zoom_ratio:.6f})"

    print("PASS")
    print("full", full_pix.width, full_pix.height, f"non_white_ratio={full_ratio:.4f}")
    print("clip", clip_pix.width, clip_pix.height, f"non_white_ratio={clip_ratio:.4f}")
    print("zoom", zoom_pix.width, zoom_pix.height, f"non_white_ratio={zoom_ratio:.4f}")
    print("artifacts", OUT_DIR)


if __name__ == "__main__":
    main()
