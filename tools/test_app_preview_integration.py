from pathlib import Path
import importlib.util

import winnerz


ROOT = Path(__file__).resolve().parents[1]
PDF_PATH = ROOT / "data" / "ieee-format.pdf"
APP_PATH = ROOT / "app-winnerz.py"


def load_app_module():
    spec = importlib.util.spec_from_file_location("app_winnerz", APP_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("Cannot load app-winnerz.py module spec")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    if not PDF_PATH.exists():
        raise FileNotFoundError(f"Missing input PDF: {PDF_PATH}")

    app_mod = load_app_module()

    img = app_mod._render_library_page_pil(str(PDF_PATH), 5, scale=1.0)
    assert img.width > 0 and img.height > 0

    b64_pages = app_mod._render_background_pages_as_base64(str(PDF_PATH), scale=1.0)
    page_count = len(winnerz.open(str(PDF_PATH)))
    assert len(b64_pages) == page_count
    assert all(isinstance(s, str) and len(s) > 1000 for s in b64_pages)

    print("PASS")
    print("preview_image", img.width, img.height)
    print("background_pages", len(b64_pages))


if __name__ == "__main__":
    main()
