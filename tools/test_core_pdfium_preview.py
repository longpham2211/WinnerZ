from pathlib import Path
import random

from PIL import Image
import winnerz.winnerz_core as core


ROOT = Path(__file__).resolve().parents[1]
PDF_PATH = ROOT / "data" / "ieee-format.pdf"
OUT_DIR = ROOT / "temp_uploads" / "core_pdfium_preview_test"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def non_white_ratio(samples, width, height, stride, sample_count=5000):
    if width <= 0 or height <= 0:
        return 0.0
    rnd = random.Random(123)
    total = min(sample_count, width * height)
    non_white = 0
    for _ in range(total):
        x = rnd.randrange(0, width)
        y = rnd.randrange(0, height)
        off = y * stride + x * 4
        r = samples[off + 0]
        g = samples[off + 1]
        b = samples[off + 2]
        if r < 250 or g < 250 or b < 250:
            non_white += 1
    return non_white / float(total)


def save_png(data, path):
    image = Image.frombytes(
        "RGBA",
        (int(data["width"]), int(data["height"])),
        bytes(data["samples"]),
    )
    image.save(str(path), format="PNG")


def main():
    if not PDF_PATH.exists():
        raise FileNotFoundError(PDF_PATH)

    full = core.render_page_to_bytes(str(PDF_PATH), 5, 1.0, None)
    clip = core.render_page_to_bytes(str(PDF_PATH), 5, 1.0, (100.0, 120.0, 300.0, 360.0))

    save_png(full, OUT_DIR / "page6_full_core.png")
    save_png(clip, OUT_DIR / "page6_clip_core.png")

    full_samples = bytes(full["samples"])
    clip_samples = bytes(clip["samples"])
    full_ratio = non_white_ratio(full_samples, int(full["width"]), int(full["height"]), int(full["stride"]))
    clip_ratio = non_white_ratio(clip_samples, int(clip["width"]), int(clip["height"]), int(clip["stride"]))

    assert int(full["width"]) > 0 and int(full["height"]) > 0
    assert int(clip["width"]) > 0 and int(clip["height"]) > 0
    assert full_ratio > 0.005, f"core full preview looks blank: ratio={full_ratio:.6f}"
    assert clip_ratio > 0.005, f"core clip preview looks blank: ratio={clip_ratio:.6f}"

    print("PASS")
    print("full", int(full["width"]), int(full["height"]), f"non_white_ratio={full_ratio:.4f}")
    print("clip", int(clip["width"]), int(clip["height"]), f"non_white_ratio={clip_ratio:.4f}")
    print("artifacts", OUT_DIR)


if __name__ == "__main__":
    main()
