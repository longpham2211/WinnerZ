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
5.  **Rendering Pipeline**: Integrates the C++ rendering engine with a fallback Python-based preview engine using `pypdfium2`.
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

## Class Reference

### Document
Represents a PDF document instance. It manages the lifecycle of the underlying file, featuring a lazy-loading fallback architecture: it natively opens the PDF via C++ by default and only invokes PDFium for decryption or file repair if the document is encrypted or structurally corrupted.

**Constructor:**
*   `Document(path_or_bytes)`: Resolves the file path or raw memory `bytes` (Zero-Disk mode) and instantly initializes the C++ core. If encryption is detected (e.g. RC4/AES), it falls back to an automatic decryption routine seamlessly in RAM or via a temporary file.

**Methods:**
*   `__getitem__(index)`: Retrieves a `Page` object at the specified 0-based index. Supports negative indexing.
*   `__len__()`: Returns the total number of pages in the document.
*   `get_all_text()`: A highly optimized utility that utilizes C++ multi-threading to extract text from all pages. It uses a **dynamic hardware-concurrency batching** mechanism to process pages in chunks (scaling automatically with the number of CPU cores). This entirely bypasses the Python GIL and prevents thread-exhaustion (`EAGAIN`) on massive 5000+ page PDFs.
*   `tobytes()`: (Zero-Disk) Returns the finalized PDF as a raw byte array directly from RAM, avoiding any disk I/O.
*   `redact_text_multiple_pages_to_bytes(page_rects_map)`: (Native C++) Performs parallel Block Redaction across multiple pages and returns the cleaned PDF as `bytes` directly in RAM. Use with caution on very large files to avoid memory pressure.
*   `close()`: Cleans up temporary resources, such as decrypted temporary files and in-memory editing buffers.

### Page
Represents a single page within a `Document`.

**Methods:**
*   `get_text(mode="dict", sort=False)`: Extracts text content. 
    *   `mode`: Can be `dict`, `rawdict`, `blocks`, or `text`.
*   `get_drawings()`: Extracts vector drawings and graphics, mapping them to structured dictionaries containing `rect`, `fill`, and `stroke` properties.
*   `get_pixmap(matrix=None, clip=None)`: Renders the page to a bitmap image (`Pixmap`). It attempts to render using the C++ core; if that fails or returns a placeholder, it falls back to the PDFium preview backend.
*   `redact_text(rects, output_path, min_overlap_ratio=0.0)`: (Legacy C++ Core) Applies text-only redaction to the specified rectangles and saves the output to a new PDF file.
*   `clean_contents()`: Completely wipes out the vector graphics and text layer of the current page.
*   `insert_image(rect, stream=None)`: Inserts an image (from bytes) into the specified rectangle. It handles internal PDF matrix transformations automatically.
*   `show_pdf_page(rect, doc_src, page_idx, overlay=True, keep_proportion=True)`: Queues a complex overlay operation. It places a page from another document (`doc_src`) onto the current page, scaling it to fit `rect` while optionally keeping aspect ratio via `keep_proportion`. The actual merge is executed efficiently during `doc.save()`.
*   `rect` (Property): Retrieves the bounding box of the page as a `Rect`.

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
*   **Preview Document Cache**: A separate caching layer strictly for the `pypdfium2` rendering backend to keep the preview document context alive across multiple page renders.
*   **C++ Thread-Safe Font Cache**: The C++ core utilizes a lock-guarded (`std::recursive_mutex`) internal cache for Unicode, Width, and CodeSpace maps to prevent data races during parallel text extraction.

## Logging

WinnerZ uses standard Python `logging` under the `winnerz` logger namespace. Error and debug messages are routed seamlessly to this logger, allowing you to configure professional logging streams similar to `pymupdf`.
```python
import logging
logging.getLogger("winnerz").setLevel(logging.DEBUG)
```

## Performance Benchmark

Thanks to the native C++ multi-threading pipeline and persistent object caching, `WinnerZ` outperforms established industry standards like `PyMuPDF` (fitz) significantly in bulk text extraction tasks.

*Tested trên file 185 trang PDF chuẩn:*
*   ⏱️ PyMuPDF (`fitz`): **~0.44s**
*   🚀 WinnerZ (`get_all_text()`): **~0.18s** (2.5x Faster)

### C++ Micro-OCR Benchmark
*Tested trên file PDF bị mã hóa 100% chữ (Ép hệ thống quét Micro-OCR toàn bộ ký tự):*
*   🐢 OCR truyền thống (Tesseract): **~3 - 5 giây / trang**
*   ⚡ WinnerZ Micro-OCR (Bitwise Optimized): **~0.33 giây / trang** (Nhanh gấp ~15 lần)

## Dependencies

*   `pypdfium2`: Optional but highly recommended. Used for decryption, primary preview rendering, and all In-Memory editing/redaction operations (including high-speed C-level XObject merging).