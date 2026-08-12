# WinnerZ Python Library Documentation

## Overview
The `winnerz` library is a robust Python wrapper designed for processing, rendering, and manipulating PDF documents. It relies on a high-performance, multi-threaded C++ core extension (`winnerz_core`) for intensive operations while providing seamless fallback mechanisms and caching strategies in Python.

The architecture emphasizes blistering-fast text extraction, reliability, and fault-tolerance, specifically in handling binary dependencies, dynamic core library loading, and flexible preview rendering via PDFium.

## Architecture

The system is divided into several conceptual layers:
1.  **Core Loader & Diagnostics**: Handles the dynamic importing of the C++ binary (`winnerz_core`), including binary size verification, truncation repair, and Windows DLL directory management.
2.  **Document Object Model**: Provides Pythonic abstractions (`Document`, `Page`) to interact with PDF files, managing resources and state safely.
3.  **Thread-Safe Interpreter Pipeline**: A C++ native, thread-safe PDF token interpreter that leverages `std::async` for parallel multi-page text extraction, eliminating GIL bottlenecks.
4.  **Micro-OCR Fallback Engine**: A pure C++ built-in OCR engine that activates automatically when encountering corrupted or missing `ToUnicode` tables. It uses 64-bit bitwise packing and hardware `POPCOUNT` for blazing fast template matching without external dependencies like Tesseract.
5.  **Rendering & Editing Pipeline**: A native C++ rendering and editing engine utilizing integrated PDFium capabilities, completely independent of external Python pip packages.
6.  **Geometry & Data Structures**: Implements domain-specific types (`Rect`, `Matrix`, `Pixmap`) to standardize data flow between the C++ layer and Python runtime.

## Core Loading Mechanism

The library initializes the C++ binary through `_load_core()`. This system provides the following safety guarantees:
*   **Thread Safety**: Uses `threading.Lock()` to ensure the core is initialized exactly once.
*   **Retry Logic**: Implements a retry loop (`_CORE_IMPORT_RETRIES = 3`) to mitigate transient filesystem or OS-level loading issues.
*   **Self-Healing**: If a truncated binary is detected (e.g., due to an interrupted build or copy), `_try_repair_truncated_core_binary()` attempts to restore it from other valid candidate binaries in the directory.
*   **Diagnostic Reporting**: Generates detailed error messages specifying binary ABI mismatches (e.g., `GLIBC` mismatches) or binary sizes to accelerate debugging.

## Environment Variables

*   `WINNERZ_PREVIEW_BACKEND`: Controls the backend used for rendering preview data when the C++ core returns placeholder data.
    *   Valid values: `auto` (default), `pdfium`.
    *   Resolution order for `auto`: Uses PDFium when available.

## Advanced Features

### Micro-OCR Anti-Obfuscation
WinnerZ includes a built-in, lightweight Micro-OCR engine written entirely in C++. When a PDF intentionally hides its text by removing the `ToUnicode` table or scrambling encodings, the engine automatically falls back to rendering the vector glyphs and performing Image-over-Union (IoU) template matching.
*   **Broad Language Support**: Contains 2170+ built-in templates covering English, Vietnamese, Latin Extended, Cyrillic, Greek, and Thai.
*   **Hardware Accelerated**: Uses 64-bit Bitwise Packing and CPU `__popcnt64` instructions to evaluate millions of pixel comparisons in milliseconds.
*   **Zero Dependencies**: Does not require Tesseract, ONNX, or any heavy AI models.

## Concurrency & Multi-threading Architecture

WinnerZ features a sophisticated threading model designed to maximize hardware utilization while complying with Python's Global Interpreter Lock (GIL). Functions are grouped into three categories:

### Group 1: Native C++ Multi-threading (Fully Automatic)
These functions automatically spawn C++ threads to utilize all CPU cores. They bypass the Python GIL and run at maximum hardware speeds.
*   `doc.get_all_text()`: Extracts **plain text only** from all pages simultaneously. Extremely fast, but strips out all spatial coordinates and font data.
*   `doc.get_all_dicts_json(include_chars=False, sort=False)`: High-performance multi-threaded extraction of rich text structures (bbox, fonts, colors, ascender, descender). Returns a JSON string of the entire document's data in seconds. Parse with `json.loads()` to bypass GIL object-creation overhead.
*   `doc.insert_text_json()`: Batch inserts text tasks across hundreds of pages in parallel.
*   `doc.redact_pages_bytes()`: Performs batch redaction across multiple pages simultaneously.

### Group 2: GIL-Released Methods (Python ThreadPool Ready)
These functions process a single page at a time but explicitly release the Python GIL (`py::gil_scoped_release`) during computation. **To achieve multi-threading, you must wrap them in Python's `concurrent.futures.ThreadPoolExecutor`.** This is the optimal architecture for extracting complex Python objects (like dicts) concurrently without lock contention.
*   `page.get_text("dict")`: Extracts text along with precise spatial coordinates (bbox), fonts, and colors.
*   `page.get_text("blocks")`, `page.get_text("rawdict")`
*   `page.get_text("text")`

### Group 3: Single-threaded (GIL Locked)
These functions execute sequentially.
*   `page.get_drawings()`
*   `page.get_pixmap()`

## Module-Level Functions

*   `winnerz.open(path_or_bytes)`: The recommended global method to open a PDF. It utilizes an intelligent global caching mechanism to prevent redundant initializations and file I/O overhead.
*   `winnerz.preload_fonts(fonts_dir)`: Pre-loads fonts into memory from a specified directory to accelerate bulk text-insertion tasks.
*   `winnerz.measure_text_width(text, font_path, font_size, is_bold=False, is_italic=False)`: Uses HarfBuzz and FreeType in C++ to precisely measure the horizontal pixel width of a text string. Crucial for calculating word-wrap and alignments before insertion.

## Class Reference

### Document
Represents a PDF document instance. It manages the lifecycle of the underlying file, featuring a lazy-loading fallback architecture: it natively opens the PDF via C++ by default and only invokes the native C++ PDFium backend for decryption or file repair if the document is encrypted or structurally corrupted.

**Constructor:**
*   `Document(path_or_bytes)`: Resolves the file path or raw memory `bytes` (Zero-Disk mode) and instantly initializes the C++ core. If encryption is detected (e.g. RC4/AES), it falls back to an automatic decryption routine seamlessly in RAM or via a temporary file.

**Methods:**
*   `__getitem__(index)`: Retrieves a `Page` object at the specified 0-based index. Supports negative indexing.
*   `__len__()`: Returns the total number of pages in the document.
*   `get_all_text()`: A highly optimized utility that utilizes C++ multi-threading to extract **plain text only** from all pages. It scales automatically with CPU cores and bypasses the Python GIL. **Note: This function discards all spatial coordinates (bboxes) and font data.**
*   `get_all_dicts_json(include_chars=False, sort=False)`: Extracts rich structural data (similar to `get_text("dict")`) for all pages simultaneously using native C++ threads. Returns a massive JSON string. Use `json.loads()` to parse it instantly into a list of Python dictionaries, completely bypassing the single-threaded PyBind11 object creation overhead.
*   `tobytes()`: (Zero-Disk) Returns the finalized PDF as a raw byte array directly from RAM, avoiding any disk I/O.
*   `insert_text_json(json_str, fonts_dir="", progress_cb=None)`: High-speed native multi-threaded C++ text insertion. Renders a JSON string of tasks directly into the PDF document in RAM in parallel. Bypasses intermediate rendering pipelines and significantly boosts performance. Evaluates fallback fonts in `fonts_dir` on a per-character basis. Supports `"multiline": true` in JSON to enable Word-wrap mode within the bounding box.
*   `insert_text_fit_spacing_json(json_str, fonts_dir="", progress_cb=None)`: Native C++ parallel text insertion similar to `insert_text_json`, but with a highly specialized **Letter Spacing Compression (Bóp Dẹt Chữ)** engine. When text exceeds the bounding box width, it intelligently compresses the `Tm` horizontal matrix and glyph advance width without shrinking the font size. Perfect for translating documents where single-line spatial integrity is required without breaking into multiple lines.
*   `insert_rects_json(json_str)`: Native C++ parallel rendering for colored rectangles (useful for background patching/redaction). Maps page indices to bounding boxes and RGB colors via JSON. Injects raw vector PDF streams (`re`, `f`) directly, bypassing slow PyMuPDF loops and Pillow image conversions. Highly recommended to pad bounding boxes by ~2 points to ensure full artifact coverage.
*   `redact_pages_bytes(page_rects_map)`: (Native C++) Performs parallel Block Redaction across multiple pages and returns the cleaned PDF as `bytes` directly in RAM. Use with caution on very large files to avoid memory pressure.
*   `redact_pages(output_path, page_rects_map)`: Performs parallel block redaction and saves directly to a file on disk.
*   `redact_rects(output_path, page_index, rects)`: Applies block redaction to a single specific page and saves directly to disk.
*   `get_page_font_basenames(page_index=0)`: Extracts the true BaseFont name (e.g., 'TimesNewRomanPS-BoldMT') from internal PDF resource identifiers (e.g., 'R14') by automatically stripping subset prefixes ('ABCDEF+'). Crucial for accurate font mapping and reconstruction.
*   `clear_page_cache()`: Flushes the in-memory cache of previously processed pages. Highly recommended when iterating through thousands of pages to prevent RAM exhaustion.
*   `save(path)`: Persists any changes made to the document (e.g., text insertions or overlays) directly to a file on disk.
*   `close()`: Cleans up temporary resources, such as decrypted temporary files and in-memory editing buffers.

### Page
Represents a single page within a `Document`.

**Methods:**
*   `get_text(mode="dict", sort=False)`: Extracts text content. 
    *   `mode`: Can be `dict`, `rawdict`, `blocks`, `text`, `json`, or `rawjson`.
*   `get_drawings()`: Extracts vector drawings and graphics, mapping them to structured dictionaries containing `rect`, `fill`, and `stroke` properties.
*   `get_pixmap(matrix=None, clip=None)`: Renders the page to a bitmap image (`Pixmap`) using the C++ core engine.
*   `redact_text(rects, output_path, min_overlap_ratio=0.0)`: (Legacy C++ Core) Applies text-only redaction to the specified rectangles and saves the output to a new PDF file.
*   `clean_contents()`: Completely wipes out the vector graphics and text layer of the current page.
*   `insert_image(rect, stream=None)`: Inserts an image (from bytes) into the specified rectangle. It handles internal PDF matrix transformations automatically.
*   `show_pdf_page(rect, doc_src, page_idx, overlay=True, keep_proportion=True)`: Queues a complex overlay operation. It places a page from another document (`doc_src`) onto the current page, scaling it to fit `rect` while optionally keeping aspect ratio via `keep_proportion`. The actual merge is executed efficiently during `doc.save()`.
*   `rect` (Property): Retrieves the bounding box of the page as a `Rect`.
*   `parent` (Property): Returns the parent `Document` instance containing this page.
*   `number` (Property): Retrieves the 1-based index (actual page number) of the page.

### Pixmap
Represents an uncompressed image buffer containing pixel data.

**Properties:**
*   `width`, `height`: Dimensions in pixels.
*   `n`: Number of channels (e.g., 4 for RGBA).
*   `stride`: Number of bytes per row.
*   `samples`: Raw byte array of pixel data.

**Methods:**
*   `pixel(x, y)`: Returns a tuple representing the pixel color at the specified coordinates.
*   `tobytes(fmt="raw")`: Encodes the pixmap to the requested format. Supported formats include `raw`, `rgba`, `png`, `jpg`, and `jpeg`. Output formats other than raw require the `Pillow` library.

### Geometry Classes

*   **Rect(x0, y0, x1, y1)**: Represents a rectangle. Provides properties for `width`, `height`, and `is_empty`. Overloads the `&` operator to compute the intersection of two rectangles.
*   **Matrix(sx=1.0, sy=1.0)**: Represents a 2D scaling matrix.

## Caching Strategy

The module implements file-based caching for document instances to minimize redundant initialization and file I/O operations.
*   **Global Document Cache**: Managed via `winnerz.open(path)`. Validates cache hits using file signature metrics (file size and modification time in nanoseconds).
    > [!TIP]
    > If you need to open multiple copies of the same file concurrently or bypass this cache (e.g., in background workers), initialize the document directly using `winnerz.Document(path)` instead of `winnerz.open()`.
*   **Preview Document Cache**: A separate caching layer for rendering backends to keep the preview document context alive across multiple page renders.
*   **C++ Thread-Safe Font Cache**: The C++ core utilizes a lock-guarded (`std::recursive_mutex`) internal cache for Unicode, Width, and CodeSpace maps to prevent data races during parallel text extraction.

## Logging

WinnerZ uses standard Python `logging` under the `winnerz` logger namespace. Error and debug messages are routed seamlessly to this logger, allowing you to configure professional logging streams similar to `pymupdf`.
```python
import logging
logging.getLogger("winnerz").setLevel(logging.DEBUG)
```

## Performance Benchmark

Thanks to the native C++ multi-threading pipeline and persistent object caching, `WinnerZ` outperforms established industry standards like `PyMuPDF` (fitz) significantly in bulk text extraction tasks.

*Tested on a standard 185-page PDF file (Plain Text Extraction):*
*  PyMuPDF (`fitz`): **~0.44s**
*  WinnerZ (`get_all_text()`): **~0.18s** (2.5x Faster)

*Tested on a 22MB Russian Chemistry Book (Structured Dictionary/JSON Extraction):*
*  PyMuPDF Single-Thread (`get_text("json")`): **~24.2s**
*  WinnerZ Native Multi-Thread (`get_all_dicts_json()`): **~11.4s** (> 2x Faster)

### C++ Micro-OCR Benchmark
*Tested on a 100% text-obfuscated PDF file (Forcing the system to Micro-OCR all characters):*
* Traditional OCR (Tesseract): **~3 - 5 seconds / page**
* WinnerZ Micro-OCR (Bitwise Optimized): **~0.33 seconds / page** (~15x Faster)
