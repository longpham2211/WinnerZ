import json
import importlib
import importlib.machinery
import io
import math
import os
import shutil
import sys
import threading
import time
from pathlib import Path
import contextlib
import logging

logger = logging.getLogger("winnerz")
if not logger.handlers:
    handler = logging.StreamHandler()
    formatter = logging.Formatter("%(levelname)s - %(name)s - %(message)s")
    handler.setFormatter(formatter)
    logger.addHandler(handler)
    logger.setLevel(logging.WARNING)


@contextlib.contextmanager
def _silence_c_stderr():
    sys.stderr.flush()
    try:
        old_stderr_fd = os.dup(2)
        devnull_fd = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull_fd, 2)
    except Exception:
        yield
        return
    try:
        yield
    finally:
        try:
            sys.stderr.flush()
            os.dup2(old_stderr_fd, 2)
            os.close(old_stderr_fd)
            os.close(devnull_fd)
        except Exception:
            pass


_DLL_DIR_HANDLES = []
_DLL_DIRS_READY = False
_CORE_IMPORT_RETRIES = 3
_CORE_MIN_VALID_BYTES = 64 * 1024
_CORE_MODULE = None
_CORE_MODULE_LOCK = threading.Lock()

_DOC_CACHE_LOCK = threading.Lock()
_DOC_CACHES = {}  # {path: (size, mtime_ns, doc, last_access_time)}
_MAX_CACHE_ENTRIES = 10

_PREVIEW_DOC_CACHE_LOCK = threading.Lock()
_PREVIEW_DOC_CACHE_PATH = None
_PREVIEW_DOC_CACHE_SIZE = 0
_PREVIEW_DOC_CACHE_MTIME_NS = 0
_PREVIEW_DOC_CACHE_DOC = None

_PREVIEW_BACKEND = os.environ.get("WINNERZ_PREVIEW_BACKEND", "auto").strip().lower()


def _read_file_signature(path):
    try:
        st = os.stat(path)
    except OSError:
        return 0, 0

    mtime_ns = getattr(st, "st_mtime_ns", None)
    if mtime_ns is None:
        mtime_ns = int(float(st.st_mtime) * 1_000_000_000)
    return int(st.st_size), int(mtime_ns)


def _ensure_windows_dll_dirs():
    global _DLL_DIRS_READY
    if os.name != "nt":
        return
    if not hasattr(os, "add_dll_directory"):
        return
    if _DLL_DIRS_READY:
        return

    pkg_dir = Path(__file__).resolve().parent
    candidates = [
        pkg_dir,
    ]

    for p in candidates:
        if p.exists():
            _DLL_DIR_HANDLES.append(os.add_dll_directory(str(p)))

    _DLL_DIRS_READY = True


def _dependency_error_message(exc):
    text = str(exc)
    if "GLIBCXX_" in text or "GLIBC_" in text:
        return (
            "Binary ABI mismatch while importing winnerz_core: {}. "
            "Rebuild winnerz_core in the SAME Linux runtime image/container where app-hf runs "
            "(same glibc + libstdc++), then replace winnerz/winnerz_core*.so. "
        ).format(text)
    return None


def _iter_core_binary_candidates():
    pkg_dir = Path(__file__).resolve().parent
    seen = set()

    for suffix in importlib.machinery.EXTENSION_SUFFIXES:
        candidate = (pkg_dir / ("winnerz_core" + suffix)).resolve()
        key = str(candidate)
        if key in seen:
            continue
        seen.add(key)
        yield candidate

    for pattern in ("winnerz_core*.so", "winnerz_core*.pyd"):
        for candidate in pkg_dir.glob(pattern):
            candidate = candidate.resolve()
            key = str(candidate)
            if key in seen:
                continue
            seen.add(key)
            yield candidate


def _core_binary_size(path):
    try:
        return path.stat().st_size
    except OSError:
        return None


def _is_valid_core_binary(path):
    size = _core_binary_size(path)
    return size is not None and size >= _CORE_MIN_VALID_BYTES


def _try_repair_truncated_core_binary():
    existing = [p for p in _iter_core_binary_candidates() if p.exists()]
    if not existing:
        return False

    valid = [p for p in existing if _is_valid_core_binary(p)]
    truncated = [p for p in existing if not _is_valid_core_binary(p)]

    if not valid or not truncated:
        return False

    valid.sort(key=lambda p: _core_binary_size(p) or 0, reverse=True)
    source = valid[0]
    repaired = False

    for target in truncated:
        if target == source:
            continue

        tmp_target = target.with_name(target.name + ".tmp-repair")
        try:
            shutil.copy2(source, tmp_target)
            os.replace(tmp_target, target)
            repaired = True
        except OSError:
            pass
        finally:
            try:
                if tmp_target.exists():
                    tmp_target.unlink()
            except OSError:
                pass

    return repaired


def _core_binary_diagnostics():
    parts = []
    for p in _iter_core_binary_candidates():
        if not p.exists():
            continue
        size = _core_binary_size(p)
        if size is None:
            parts.append("{}:unreadable".format(p))
        else:
            parts.append("{}:{}B".format(p, size))

    if not parts:
        return "no winnerz_core binary found in package directory"
    return ", ".join(parts)


def _load_core():
    global _CORE_MODULE

    if _CORE_MODULE is not None:
        return _CORE_MODULE

    with _CORE_MODULE_LOCK:
        if _CORE_MODULE is not None:
            return _CORE_MODULE

        _ensure_windows_dll_dirs()
        last_exc = None

        for attempt in range(_CORE_IMPORT_RETRIES):
            try:
                _CORE_MODULE = importlib.import_module("winnerz.winnerz_core")
                return _CORE_MODULE
            except ModuleNotFoundError as exc:
                if exc.name == "winnerz.winnerz_core":
                    raise RuntimeError(
                        "winnerz_core module is not built yet. Build with WINNERZ_BUILD_PYTHON_WRAPPER=ON."
                    ) from exc
                raise RuntimeError(
                    "Failed to import winnerz_core: {}".format(exc)
                ) from exc
            except ImportError as exc:
                dep_msg = _dependency_error_message(exc)
                if dep_msg:
                    raise RuntimeError(dep_msg) from exc

                text = str(exc)
                if "file too short" in text:
                    last_exc = exc
                    repaired = _try_repair_truncated_core_binary()
                    _CORE_MODULE = None
                    sys.modules.pop("winnerz.winnerz_core", None)
                    importlib.invalidate_caches()

                    if repaired or attempt + 1 < _CORE_IMPORT_RETRIES:
                        time.sleep(0.2 * (attempt + 1))
                        continue

                    raise RuntimeError(
                        "winnerz_core binary appears truncated ('file too short'). "
                        "Detected binaries: {}. Rebuild winnerz_core and deploy by atomic replace.".format(
                            _core_binary_diagnostics()
                        )
                    ) from exc

                raise RuntimeError(
                    "Failed to import winnerz_core: {}".format(exc)
                ) from exc
            except Exception as exc:
                raise RuntimeError(
                    "Failed to import winnerz_core: {}".format(exc)
                ) from exc

        if last_exc is not None:
            raise RuntimeError(
                "Failed to import winnerz_core after retries: {}. Detected binaries: {}".format(
                    last_exc, _core_binary_diagnostics()
                )
            ) from last_exc

        raise RuntimeError("Failed to import winnerz_core after retries.")


def _call_text_json_compat(func, page_index, sort):
    try:
        return func(page_index, bool(sort))
    except TypeError as exc:
        # Backward compatibility: older winnerz_core builds only expose
        # (page_index) without the 'sort' argument.
        if "incompatible function arguments" not in str(exc):
            raise
        return func(page_index)


def _render_data_is_placeholder(data):
    return False


def _compute_crop_box_pixels(clip_tuple, page_w, page_h, scale):
    full_w = max(1, int(round(page_w * scale)))
    full_h = max(1, int(round(page_h * scale)))

    if clip_tuple is None:
        return (0, 0, full_w, full_h)

    x0, y0, x1, y1 = [float(v) for v in clip_tuple]

    x0 = max(0.0, min(x0, page_w))
    x1 = max(0.0, min(x1, page_w))
    y0 = max(0.0, min(y0, page_h))
    y1 = max(0.0, min(y1, page_h))

    rx0 = min(x0, x1)
    rx1 = max(x0, x1)
    ry0 = min(y0, y1)
    ry1 = max(y0, y1)

    left = max(0, int(math.floor(rx0 * scale)))
    top = max(0, int(math.floor(ry0 * scale)))
    right = min(full_w, int(math.ceil(rx1 * scale)))
    bottom = min(full_h, int(math.ceil(ry1 * scale)))

    if right <= left:
        right = min(full_w, left + 1)
    if bottom <= top:
        bottom = min(full_h, top + 1)

    return (left, top, right, bottom)


def _open_preview_pdfium_doc(pdf_path):
    raise RuntimeError("pdfium backend unavailable (handled by C++ core)")


def _render_page_with_pdfium(pdf_path, page_index, scale, clip_tuple):
    raise RuntimeError("pdfium backend unavailable (handled by C++ core)")


def _render_page_with_playwright(pdf_path, page_index, scale, clip_tuple, page_rect):
    try:
        from PIL import Image
    except Exception as exc:
        raise RuntimeError("Pillow is required for Playwright preview backend") from exc

    try:
        playwright_sync = importlib.import_module("playwright.sync_api")
        sync_playwright = getattr(playwright_sync, "sync_playwright")
    except Exception as exc:
        raise RuntimeError("Playwright preview backend unavailable") from exc

    page_w = max(1.0, float(page_rect.width))
    page_h = max(1.0, float(page_rect.height))
    full_w = max(1, int(round(page_w * float(scale))))
    full_h = max(1, int(round(page_h * float(scale))))

    pdf_uri = Path(pdf_path).resolve().as_uri() + "#page={}".format(int(page_index) + 1)
    html = """<!DOCTYPE html>
<html>
<head>
<meta charset=\"UTF-8\" />
<style>
html, body {{ margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; background: #ffffff; }}
embed {{ width: 100%; height: 100%; border: 0; display: block; }}
</style>
</head>
<body>
<embed src=\"{}\" type=\"application/pdf\" />
</body>
</html>
""".format(
        pdf_uri
    )

    png_bytes = None
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True, args=["--no-sandbox"])
        context = browser.new_context(
            viewport={"width": full_w, "height": full_h}, device_scale_factor=1.0
        )
        page = context.new_page()
        page.set_content(html, wait_until="networkidle")
        png_bytes = page.screenshot(full_page=False, type="png")
        context.close()
        browser.close()

    image = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    left, top, right, bottom = _compute_crop_box_pixels(
        clip_tuple, page_w, page_h, float(scale)
    )
    image = image.crop((left, top, right, bottom))
    width, height = image.size
    rgba = image.tobytes("raw", "RGBA")
    return {
        "width": int(width),
        "height": int(height),
        "channels": 4,
        "stride": int(width) * 4,
        "samples": rgba,
    }


def _render_page_preview_data(pdf_path, page_index, scale, clip_tuple, page_rect):
    backend = _PREVIEW_BACKEND
    errors = []

    if backend in ("auto", "pdfium"):
        try:
            return _render_page_with_pdfium(pdf_path, page_index, scale, clip_tuple)
        except Exception as exc:
            errors.append("pdfium: {}".format(exc))
            if backend == "pdfium":
                raise

    if backend in ("auto", "playwright"):
        try:
            return _render_page_with_playwright(
                pdf_path, page_index, scale, clip_tuple, page_rect
            )
        except Exception as exc:
            errors.append("playwright: {}".format(exc))
            if backend == "playwright":
                raise

    raise RuntimeError(
        "No preview backend available ({})".format(
            "; ".join(errors) if errors else backend
        )
    )


class Rect:
    def __init__(self, x0, y0, x1, y1):
        self.x0 = float(x0)
        self.y0 = float(y0)
        self.x1 = float(x1)
        self.y1 = float(y1)

    def as_tuple(self):
        return (self.x0, self.y0, self.x1, self.y1)

    @property
    def width(self):
        return self.x1 - self.x0

    @property
    def height(self):
        return self.y1 - self.y0

    @property
    def is_empty(self):
        return self.width <= 0 or self.height <= 0

    def __and__(self, other):
        return Rect(
            max(self.x0, other.x0),
            max(self.y0, other.y0),
            min(self.x1, other.x1),
            min(self.y1, other.y1),
        )


class Matrix:
    def __init__(self, sx=1.0, sy=1.0):
        self.sx = float(sx)
        self.sy = float(sy)


class Pixmap:
    def __init__(self, width, height, channels, stride, samples):
        self.width = int(width)
        self.height = int(height)
        self.n = int(channels)
        self.stride = int(stride)
        self.samples = bytes(samples)

    def pixel(self, x, y):
        xi = int(x)
        yi = int(y)
        if xi < 0 or yi < 0 or xi >= self.width or yi >= self.height:
            raise IndexError("pixel out of range")
        off = yi * self.stride + xi * self.n
        return tuple(self.samples[off : off + self.n])

    def tobytes(self, fmt="raw"):
        f = fmt.lower()
        if f in ("raw", "rgba"):
            return self.samples

        try:
            image_module = importlib.import_module("PIL.Image")
        except Exception as exc:
            raise RuntimeError("Pillow is required for encoded output formats") from exc

        img = image_module.frombytes("RGBA", (self.width, self.height), self.samples)
        out = io.BytesIO()
        if f in ("jpg", "jpeg"):
            img.convert("RGB").save(out, format="JPEG")
        elif f == "png":
            img.save(out, format="PNG")
        else:
            raise ValueError("Unsupported pixmap format: {}".format(fmt))
        return out.getvalue()


class Page:
    def __init__(self, doc, index):
        self.doc = doc
        self.index = int(index)
        self._rect_cache = None

    @property
    def parent(self):
        return self.doc

    @property
    def number(self):
        return self.index

    def get_text(self, mode="dict", sort=False):
        mode = mode.lower()

        if mode == "dict":
            return _call_text_json_compat(self.doc._core_doc.get_dict, self.index, sort)
        if mode == "rawdict":
            return _call_text_json_compat(
                self.doc._core_doc.get_rawdict, self.index, sort
            )
        if mode == "json":
            if hasattr(self.doc._core_doc, "get_json"):
                return self.doc._core_doc.get_json(self.index, False, sort)
            import json

            return json.dumps(self.get_text("dict", sort=sort))
        if mode == "rawjson":
            if hasattr(self.doc._core_doc, "get_json"):
                return self.doc._core_doc.get_json(self.index, True, sort)
            import json

            return json.dumps(self.get_text("rawdict", sort=sort))
        if mode == "blocks":
            return _call_text_json_compat(
                self.doc._core_doc.get_blocks, self.index, sort
            )
        if mode == "text":
            if hasattr(self.doc._core_doc, "get_text_plain"):
                return _call_text_json_compat(
                    self.doc._core_doc.get_text_plain, self.index, sort
                )
            blocks = _call_text_json_compat(
                self.doc._core_doc.get_blocks, self.index, sort
            )
            return "".join(block.get("text", "") for block in blocks)
        raise ValueError("Unsupported text mode: {}".format(mode))

    def get_drawings(self):
        raw = self.doc._core_doc.get_drawings(self.index)
        mapped = []
        for d in raw:
            item = dict(d)
            rect_data = d.get("scissor_clip")
            if rect_data and len(rect_data) == 4:
                item["rect"] = Rect(*rect_data)

            fill = d.get("fill_color", {}).get("components", [])
            if fill:
                item["fill"] = tuple(fill)

            stroke = d.get("stroke_color", {}).get("components", [])
            if stroke:
                item["stroke"] = tuple(stroke)

            mapped.append(item)
        return mapped

    @property
    def rect(self):
        if self._rect_cache is None:
            x0, y0, x1, y1 = self.doc._core_doc.page_rect(self.index)
            self._rect_cache = Rect(x0, y0, x1, y1)
        return self._rect_cache

    def get_pixmap(self, matrix=None, clip=None, **kwargs):
        if matrix is None:
            scale = 1.0
        elif isinstance(matrix, Matrix):
            scale = max(matrix.sx, matrix.sy)
        else:
            sx = getattr(matrix, "sx", getattr(matrix, "a", getattr(matrix, "x", 1.0)))
            sy = getattr(matrix, "sy", getattr(matrix, "d", getattr(matrix, "y", sx)))
            scale = max(float(sx), float(sy))

        clip_tuple = None
        if clip is not None:
            if isinstance(clip, Rect):
                clip_tuple = clip.as_tuple()
            else:
                clip_tuple = (
                    float(clip[0]),
                    float(clip[1]),
                    float(clip[2]),
                    float(clip[3]),
                )

        data = None
        try:
            core_data = self.doc._core_doc.render_page(
                self.index, float(scale), clip_tuple
            )
            if not _render_data_is_placeholder(core_data):
                data = core_data
        except Exception as e:
            logger.debug("C++ PDFium render_page_to_bytes failed: %s", e)
            data = None

        if data is None:
            data = _render_page_preview_data(
                self.doc.path, self.index, float(scale), clip_tuple, self.rect
            )

        return Pixmap(
            data["width"],
            data["height"],
            data["channels"],
            data["stride"],
            data["samples"],
        )

    def redact_text(self, rects, output_path, min_overlap_ratio=0.0):
        serializable_rects = []
        for r in rects:
            if isinstance(r, Rect):
                serializable_rects.append(list(r.as_tuple()))
            else:
                serializable_rects.append(
                    [float(r[0]), float(r[1]), float(r[2]), float(r[3])]
                )

        return self.doc._core_doc.redact_rects(
            output_path,
            self.index,
            serializable_rects,
            float(min_overlap_ratio),
        )

    def clean_contents(self):
        pdfium_doc = self.doc._get_pdfium_edit_doc()
        pdfium_doc.clean_contents(self.index)

    def insert_image(self, rect, stream=None):
        pdfium_doc = self.doc._get_pdfium_edit_doc()
        if stream:
            import io
            from PIL import Image

            img = Image.open(io.BytesIO(stream)).convert("RGBA")
            width, height = img.size
            rgba = img.tobytes("raw", "RGBA")
        else:
            raise ValueError("stream is required")

        if hasattr(rect, "as_tuple"):
            x0, y0, x1, y1 = rect.as_tuple()
        else:
            x0, y0, x1, y1 = rect[0], rect[1], rect[2], rect[3]

        pdfium_doc.insert_image_rgba(self.index, width, height, rgba, [x0, y0, x1, y1])

    def show_pdf_page(
        self, rect, doc_src, page_idx, overlay=True, keep_proportion=True
    ):
        pdfium_doc = self.doc._get_pdfium_edit_doc()
        src_pdfium_doc = doc_src._get_pdfium_edit_doc()

        if hasattr(rect, "as_tuple"):
            x0, y0, x1, y1 = rect.as_tuple()
        else:
            x0, y0, x1, y1 = rect[0], rect[1], rect[2], rect[3]

        pdfium_doc.show_pdf_page(
            self.index, src_pdfium_doc, page_idx, [x0, y0, x1, y1], keep_proportion
        )


class Document:
    """Represents a PDF document."""

    def __init__(self, path_or_bytes):
        winnerz_core = _load_core()

        if isinstance(path_or_bytes, bytes):
            self.path = "<memory>"
            self._temp_decrypted_path = None
            try:
                self._core_doc = winnerz_core.Document(path_or_bytes)
                if getattr(self._core_doc, "is_encrypted", lambda: False)():
                    raise RuntimeError("File is encrypted, falling back to PDFium")
            except Exception:
                path_or_bytes = self._rebuild_with_pdfium_bytes(path_or_bytes)
                self._core_doc = winnerz_core.Document(path_or_bytes)

            if hasattr(winnerz_core, "PdfiumEditorDoc"):
                self._pdfium_doc = winnerz_core.PdfiumEditorDoc(path_or_bytes)
            else:
                self._pdfium_doc = None
        else:
            self.original_path = path_or_bytes
            self.path = path_or_bytes
            self._temp_decrypted_path = None
            try:
                self._core_doc = winnerz_core.Document(self.path)
                if getattr(self._core_doc, "is_encrypted", lambda: False)():
                    raise RuntimeError("File is encrypted, falling back to PDFium")
            except Exception as e:
                self._rebuild_with_pdfium(self.path)
                self._core_doc = winnerz_core.Document(self.path)

            if hasattr(winnerz_core, "PdfiumEditorDoc"):
                self._pdfium_doc = winnerz_core.PdfiumEditorDoc(self.path)
            else:
                self._pdfium_doc = None

        self._page_count = int(self._core_doc.page_count())
        self._pages = [None] * self._page_count

    def _rebuild_with_pdfium(self, path):
        import uuid

        winnerz_core = _load_core()

        temp_dir = Path("temp_uploads").resolve()
        temp_dir.mkdir(exist_ok=True)
        self._temp_decrypted_path = str(temp_dir / f"decrypted_{uuid.uuid4().hex}.pdf")

        import builtins

        f = builtins.open(path, "rb")
        if hasattr(winnerz_core, "PdfiumEditorDoc"):
            src_doc = winnerz_core.PdfiumEditorDoc(f.read())
            dst_doc = winnerz_core.PdfiumEditorDoc()
            dst_doc.import_pages(src_doc, "")
            dst_doc.save_to_file(self._temp_decrypted_path)
        else:
            raise RuntimeError("Cannot decrypt: PdfiumEditorDoc not available")
        f.close()
        self.path = self._temp_decrypted_path

    def _rebuild_with_pdfium_bytes(self, raw_bytes):
        winnerz_core = _load_core()
        if hasattr(winnerz_core, "PdfiumEditorDoc"):
            src_doc = winnerz_core.PdfiumEditorDoc(raw_bytes)
            dst_doc = winnerz_core.PdfiumEditorDoc()
            dst_doc.import_pages(src_doc, "")
            return dst_doc.save()
        else:
            raise RuntimeError("Cannot decrypt bytes: PdfiumEditorDoc not available")

    def __getitem__(self, index):
        if index < 0 or index >= self._page_count:
            raise IndexError("Page index out of range")
        if self._pages[index] is None:
            self._pages[index] = Page(self, index)
        return self._pages[index]

    def __len__(self):
        return self._page_count

    def redact_rects(self, output_path, page_index, rects):
        return self._core_doc.redact_rects(output_path, page_index, rects, 0.0)

    def redact_pages_bytes(self, page_rects_map):
        return self._core_doc.redact_pages_bytes(page_rects_map, 0.0)

    def redact_pages(self, output_path, page_rects_map):
        return self._core_doc.redact_pages(output_path, page_rects_map, 0.0)

    def clear_page_cache(self):
        return self._core_doc.clear_page_cache()

    def get_all_text(self):
        """
        Extract text from all pages concurrently using C++ threads.
        This bypasses the Python GIL and scales with the number of CPU cores.
        """
        return self._core_doc.get_all_text()

    def get_all_dicts_json(self, include_chars=False, sort=False):
        """
        Extract detailed dict structure (like get_text('dict')) from ALL pages
        concurrently using C++ threads, returning a single JSON string.
        This bypasses the Python GIL and is much faster than looping in Python.
        """
        if hasattr(self._core_doc, "get_all_dicts_json"):
            return self._core_doc.get_all_dicts_json(include_chars, sort)
        raise NotImplementedError("get_all_dicts_json is not supported by the core")

    def insert_text_json(self, json_str, fonts_dir="", progress_cb=None):
        """
        Insert text into multiple pages concurrently using C++.
        Input format: JSON string mapping page_index (int) -> list of tasks.
        Example:
            {
                "0": [
                    {"text": "Hello", "rect": [10, 10, 100, 30], "size": 12, "color": [255, 0, 0], "font_family": "TimesNewRoman-Bold"}
                ]
            }
        """
        if hasattr(self._core_doc, "insert_text_to_pages_json"):
            return self._core_doc.insert_text_to_pages_json(json_str, fonts_dir)
        if hasattr(self._core_doc, "insert_text_to_pages"):
            import json

            tasks_dict = json.loads(json_str)
            return self._core_doc.insert_text_to_pages(tasks_dict, fonts_dir)
        raise NotImplementedError("insert_text_json is not supported by the core")

    def insert_rects_json(self, json_str):
        """
        Insert colored rectangles into multiple pages concurrently using C++.
        Input format: JSON string mapping page_index (int) -> list of tasks.
        Example:
            {
                "0": [
                    {"rect": [10, 10, 100, 30], "color": [255, 0, 0]}
                ]
            }
        Note: It is recommended to pad the bounding boxes by 2-3 points (e.g., rect[0]-2, rect[1]-2, rect[2]+2, rect[3]+2) 
        to ensure full coverage of text artifacts since PyMuPDF font bounding boxes can be very tight.
        """
        if hasattr(self._core_doc, "insert_rects_to_pages_json"):
            return self._core_doc.insert_rects_to_pages_json(json_str)
        raise NotImplementedError("insert_rects_json is not supported by the core")

    def get_page_font_basenames(self, page_index=0):
        """
        Returns a dict mapping PDF resource names (e.g. 'R14') to the real
        BaseFont name (e.g. 'TimesNewRomanPS-BoldMT') for a given page.
        Use this to get the true font name when get_text('dict') only gives R14, R16, etc.
        """
        if hasattr(self._core_doc, "get_page_font_basenames"):
            return self._core_doc.get_page_font_basenames(page_index)
        return {}

    def save(self, path):
        with _silence_c_stderr():
            doc = self._get_pdfium_edit_doc()
            doc.save_to_file(path)

    def tobytes(self):
        with _silence_c_stderr():
            doc = self._get_pdfium_edit_doc()
            return doc.save()

    def _get_pdfium_edit_doc(self):
        if not hasattr(self, "_pdfium_edit_doc") or self._pdfium_edit_doc is None:
            winnerz_core = _load_core()
            if not hasattr(winnerz_core, "PdfiumEditorDoc"):
                raise NotImplementedError(
                    "PdfiumEditorDoc is not built on this platform"
                )
            if self.path == "<memory>":
                self._pdfium_edit_doc = self._pdfium_doc
            else:
                self._pdfium_edit_doc = winnerz_core.PdfiumEditorDoc(self.path)
        return self._pdfium_edit_doc

    def close(self):
        self._is_closed = True  # Mark as closed for cache invalidation
        for i in range(self._page_count):
            if self._pages[i] is not None:
                self._pages[i] = None

        if hasattr(self, "_pdfium_doc") and self._pdfium_doc is not None:
            try:
                self._pdfium_doc.close()
            except Exception:
                pass
            self._pdfium_doc = None

        if hasattr(self, "_pdfium_edit_doc") and self._pdfium_edit_doc is not None:
            try:
                self._pdfium_edit_doc.close()
            except Exception:
                pass
            self._pdfium_edit_doc = None

        if hasattr(self, "_core_doc"):
            self._core_doc = None

        if hasattr(self, "_temp_decrypted_path") and self._temp_decrypted_path:
            try:
                if os.path.exists(self._temp_decrypted_path):
                    os.remove(self._temp_decrypted_path)
            except Exception:
                pass
            self._temp_decrypted_path = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


def open(path_or_bytes):
    global _DOC_CACHES

    if isinstance(path_or_bytes, bytes):
        return Document(path_or_bytes)

    resolved_path = str(Path(path_or_bytes).resolve())
    size, mtime_ns = _read_file_signature(resolved_path)

    with _DOC_CACHE_LOCK:
        if resolved_path in _DOC_CACHES:
            c_size, c_mtime, c_doc, _ = _DOC_CACHES[resolved_path]
            if c_size == size and c_mtime == mtime_ns:
                if not getattr(c_doc, "_is_closed", False):
                    _DOC_CACHES[resolved_path] = (c_size, c_mtime, c_doc, time.time())
                    return c_doc
                else:
                    _DOC_CACHES.pop(resolved_path, None)  # Evict stale closed doc

    doc = Document(resolved_path)

    with _DOC_CACHE_LOCK:
        _DOC_CACHES[resolved_path] = (size, mtime_ns, doc, time.time())
        if len(_DOC_CACHES) > _MAX_CACHE_ENTRIES:
            oldest_path = min(_DOC_CACHES.keys(), key=lambda k: _DOC_CACHES[k][3])
            old_doc = _DOC_CACHES.pop(oldest_path)[2]
            try:
                old_doc.close()
            except Exception:
                pass

    return doc


def preload_fonts(fonts_dir):
    """
    (Legacy Compatibility)
    Fonts are cached automatically by the C++ core upon first use.
    This function exists to prevent AttributeError in old scripts.
    """
    pass


try:
    _load_core()
except Exception:
    pass

def measure_text_width(text, font_path, font_size, is_bold=False, is_italic=False):
    """
    Measure the horizontal pixel width of a text string using C++ (HarfBuzz and FreeType).
    """
    core = _load_core()
    if hasattr(core, "measure_text_width"):
        return core.measure_text_width(text, font_path, font_size, is_bold, is_italic)
    raise NotImplementedError("measure_text_width is not supported by the core")
