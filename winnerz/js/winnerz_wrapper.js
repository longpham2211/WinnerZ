/**
 * winnerz_wrapper.js
 * JavaScript wrapper for the WinnerZ WASM module. API mirrors Python exactly.
 *
 * Usage (Browser, ESM):
 *   import { open, Document } from './winnerz_wrapper.js';
 *
 *   const doc = await open(pdfBytes);          // Python: winnerz.open(path)
 *   const page = doc[0];                       // Python: doc[0]
 *   const text = page.get_text('text');        // Python: page.get_text('text')
 *   const dicts = doc.get_all_dicts_json(true);// Python: doc.get_all_dicts_json(include_chars=True)
 *   doc.close();
 */

// ─── Module loader (singleton) ────────────────────────────────────────────────

let _modulePromise = null;
let _wasmModule = null;

/**
 * Load (or return cached) WinnerZ WASM module.
 * @param {string} [wasmPath] - Optional path prefix for locating winnerz_wasm.wasm
 * @returns {Promise<object>}
 */
export async function load_winnerz_module(wasmPath = '') {
  if (_wasmModule) return _wasmModule;
  if (_modulePromise) return _modulePromise;

  _modulePromise = (async () => {
    const factory = (await import('./winnerz_wasm.js')).default;
    const opts = {};
    if (wasmPath) {
      opts.locateFile = (file, prefix) =>
        file.endsWith('.wasm') ? wasmPath + file : prefix + file;
    }
    _wasmModule = await factory(opts);
    return _wasmModule;
  })();

  return _modulePromise;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

/**
 * Serialize a page-keyed map to a JSON string with string keys.
 * Embind JSON parsing requires all keys to be strings.
 */
function _pages_map_to_json(pageTasksMap) {
  return JSON.stringify(
    Object.fromEntries(
      Object.entries(pageTasksMap).map(([k, v]) => [String(k), v])
    )
  );
}

// ─── class Page ───────────────────────────────────────────────────────────────

/**
 * Represents a single PDF page. Mirrors Python class Page.
 * Obtain via: const page = doc[0]  or  doc.get_page(0)
 */
export class Page {
  /**
   * @param {Document} doc
   * @param {number} index
   */
  constructor(doc, index) {
    this._doc = doc;
    this.index = index;
  }

  /** 0-based page number. Python: page.number */
  get number() {
    return this.index;
  }

  /** Reference to parent Document. Python: page.parent */
  get parent() {
    return this._doc;
  }

  /**
   * Page bounding box. Python: page.rect
   * @returns {{ x0: number, y0: number, x1: number, y1: number, width: number, height: number }}
   */
  get rect() {
    return this._doc.page_rect(this.index);
  }

  /**
   * Extract text using a mode string. Python: page.get_text(mode, sort)
   * @param {'text'|'dict'|'rawdict'|'json'|'rawjson'|'blocks'} [mode='dict']
   * @param {boolean} [sort=false]
   * @returns {string|object}
   */
  get_text(mode = 'dict', sort = false) {
    const d = this._doc._raw;
    switch (mode.toLowerCase()) {
      case 'text': return d.getTextPlain(this.index, sort);
      case 'dict': return JSON.parse(d.getDict(this.index, sort));
      case 'rawdict': return JSON.parse(d.getRawDict(this.index, sort));
      case 'json': return d.getJson(this.index, false, sort);
      case 'rawjson': return d.getJson(this.index, true, sort);
      case 'blocks': return JSON.parse(d.getBlocks(this.index, sort));
      default: throw new Error(`Winnerz: unsupported text mode: "${mode}"`);
    }
  }

  /**
   * Get real font basenames for this page.
   * Python: doc.get_page_font_basenames(i)
   * @returns {Record<string, string>}
   */
  get_page_font_basenames() {
    return this._doc.get_page_font_basenames(this.index);
  }

  /**
   * Redact (black-out) rectangular areas. Returns new PDF bytes.
   * Python: page.redact_text(rects)
   * @param {Array<[number,number,number,number]>} rects
   * @returns {Uint8Array}
   */
  redact_text(rects) {
    return this._doc.redact_page(this.index, rects);
  }

  /**
   * Get vector drawings. Python: page.get_drawings()
   * Requires PDFium enabled in WASM build.
   */
  get_drawings() {
    return this._doc._raw.getDrawings(this.index);
  }

  /**
   * Render page to image/pixmap. Python: page.get_pixmap()
   * Requires PDFium enabled in WASM build.
   * @param {number} scale Zoom level (default: 1.0)
   * @param {Array<number>|null} clip [x0, y0, x1, y1] crop box
   * @returns {{width: number, height: number, channels: number, stride: number, samples: Uint8Array}}
   */
  get_pixmap(scale = 1.0, clip = null) {
    return this._doc._raw.renderPage(this.index, scale, clip);
  }
}

// ─── class Document ───────────────────────────────────────────────────────────

/**
 * Represents a PDF document. Mirrors Python class Document.
 */
export class Document {
  /** @type {any} Raw WASM WasmDocument Embind object */
  _raw = null;
  _closed = false;
  _page_count = 0;
  _pages = [];
  _originalBytes = null; // Cache tobytes()

  /**
   * Async factory — use instead of new Document().
   * Python equivalent: Document(path_or_bytes)
   * @param {Uint8Array|ArrayBuffer} pdfData
   * @param {string} [wasmPath='']
   * @returns {Promise<Document>}
   */
  static async create(pdfData, wasmPath = '') {
    const W = await load_winnerz_module(wasmPath);
    const bytes = pdfData instanceof Uint8Array ? pdfData : new Uint8Array(pdfData);
    const rawDoc = new W.Document(bytes); // Changed to W.Document per new embind name

    const doc = new Document();
    doc._raw = rawDoc;
    doc._originalBytes = bytes;
    doc._page_count = rawDoc.pageCount();
    doc._pages = new Array(doc._page_count).fill(null);

    // Proxy: support doc[0], doc[1], doc[-1] syntax — same as Python
    return new Proxy(doc, {
      get(target, prop, receiver) {
        if (typeof prop === 'string' && /^-?\d+$/.test(prop)) {
          let idx = parseInt(prop, 10);
          if (idx < 0) idx = target._page_count + idx;
          return target.get_page(idx);
        }
        return Reflect.get(target, prop, receiver);
      }
    });
  }

  _assert_open() {
    if (this._closed) throw new Error('Winnerz: document is closed (close() was already called).');
  }

  // ── Properties ──────────────────────────────────────────────────────────────

  /** Total page count. Python: len(doc) */
  get length() { this._assert_open(); return this._page_count; }

  /** Total page count. Python: doc._page_count */
  get page_count() { this._assert_open(); return this._page_count; }

  /** Whether the PDF is encrypted. Python: doc.is_encrypted */
  get is_encrypted() { this._assert_open(); return this._raw.isEncrypted(); }

  // ── Page access ─────────────────────────────────────────────────────────────

  /**
   * Get a page by index. Python: doc[i]
   * Supports negative indices: doc.get_page(-1) → last page.
   * @param {number} pageIndex
   * @returns {Page}
   */
  get_page(pageIndex) {
    this._assert_open();
    if (pageIndex < 0 || pageIndex >= this._page_count) {
      throw new RangeError(`Page index ${pageIndex} out of range (0..${this._page_count - 1})`);
    }
    if (!this._pages[pageIndex]) {
      this._pages[pageIndex] = new Page(this, pageIndex);
    }
    return this._pages[pageIndex];
  }

  // ── Page rect ───────────────────────────────────────────────────────────────

  /**
   * Get page dimensions. Python: doc[i].rect
   * @param {number} pageIndex
   * @returns {{ x0: number, y0: number, x1: number, y1: number, width: number, height: number }}
   */
  page_rect(pageIndex) {
    this._assert_open();
    const arr = JSON.parse(this._raw.pageRect(pageIndex));
    return { x0: arr[0], y0: arr[1], x1: arr[2], y1: arr[3], width: arr[2] - arr[0], height: arr[3] - arr[1] };
  }

  // ── Text extraction ─────────────────────────────────────────────────────────

  /**
   * Extract plain text from all pages using C++ (Multi-threaded if enabled).
   * Pages are joined with \f (form-feed) characters between them.
   * Python: doc.get_all_text()
   * @returns {string}
   */
  get_all_text() {
    this._assert_open();
    return this._raw.getAllText();
  }

  /**
   * Extract full structured data for all pages as a raw JSON string (WasmPageDict[]).
   * Python: doc.get_all_dicts_json(include_chars=False, sort=False)
   * Use JSON.parse() to obtain the array of page dicts.
   * @param {boolean} [include_chars=false] Include per-character data
   * @param {boolean} [sort=false] Sort output in reading order
   * @returns {string} Raw JSON string
   */
  get_all_dicts_json(include_chars = false, sort = false) {
    this._assert_open();
    return this._raw.getAllDictsJson(include_chars, sort);
  }

  // ── Per-page text (convenience) ─────────────────────────────────────────────

  get_text(pageIndex, mode = 'dict', sort = false) {
    return this.get_page(pageIndex).get_text(mode, sort);
  }

  // ── Font info ───────────────────────────────────────────────────────────────

  get_page_font_basenames(pageIndex = 0) {
    this._assert_open();
    return JSON.parse(this._raw.getPageFontBasenames(pageIndex));
  }

  // ── Cache ───────────────────────────────────────────────────────────────────

  clear_page_cache() {
    this._assert_open();
    this._raw.clearPageCache();
  }

  // ── Redaction ───────────────────────────────────────────────────────────────

  redact_page(pageIndex, rects) {
    this._assert_open();
    // Return Uint8Array directly from WASM C++ wrapper
    return this._raw.redactRects(pageIndex, JSON.stringify(rects));
  }

  redact_pages_bytes(page_rects_map) {
    this._assert_open();
    // Return Uint8Array directly from WASM C++ wrapper
    return this._raw.redactPagesBytes(_pages_map_to_json(page_rects_map));
  }

  // ── Insert rects ────────────────────────────────────────────────────────────

  insert_rects_json(page_tasks_map) {
    this._assert_open();
    // Return Uint8Array directly from WASM C++ wrapper
    return this._raw.insertRectsToPagesJson(_pages_map_to_json(page_tasks_map));
  }

  // ── Original bytes ──────────────────────────────────────────────────────────

  /**
   * Return the original in-memory PDF bytes.
   * Python: doc.tobytes()
   * @returns {Uint8Array}
   */
  tobytes() {
    this._assert_open();
    return this._originalBytes;
  }

  // ── Insert text ─────────────────────────────────────────────────────────────

  insert_text_json(page_tasks_map, fonts_dir = '/fonts') {
    this._assert_open();
    const json = JSON.stringify(
      Object.fromEntries(
        Object.entries(page_tasks_map).map(([k, tasks]) => [
          String(k),
          tasks.map(t => ({
            text: t.text ?? '',
            rect: t.rect,
            size: t.size ?? 12,
            color: t.color ?? [0, 0, 0],
            bold: t.bold ?? false,
            italic: t.italic ?? false,
            multiline: t.multiline ?? false,
            font_family: t.font_family ?? ''
          }))
        ])
      )
    );
    // Return Uint8Array directly from WASM C++ wrapper
    return this._raw.insertTextToPagesJson(json, fonts_dir);
  }

  insert_text_fit_spacing_json(page_tasks_map, fonts_dir = '/fonts') {
    this._assert_open();
    const json = JSON.stringify(
      Object.fromEntries(
        Object.entries(page_tasks_map).map(([k, tasks]) => [
          String(k),
          tasks.map(t => ({
            text: t.text ?? '',
            rect: t.rect,
            size: t.size ?? 12,
            color: t.color ?? [0, 0, 0],
            bold: t.bold ?? false,
            italic: t.italic ?? false,
            multiline: t.multiline ?? false,
            font_family: t.font_family ?? ''
          }))
        ])
      )
    );
    // Return Uint8Array directly from WASM C++ wrapper
    return this._raw.insertTextToPagesFitSpacingJson(json, fonts_dir);
  }

  // ── Lifecycle ──────────────────────────────────────────────────────────

  close() {
    if (!this._closed && this._raw) {
      this._raw.delete(); // Delete WASM heap object
      this._raw = null;
      this._closed = true;
      this._pages = [];
      this._originalBytes = null;
    }
  }

  [Symbol.dispose]() { this.close(); }
}

// ─── open() — top-level helper (Python: winnerz.open) ────────────────────────

export async function open(pdfData, wasmPath = '') {
  return Document.create(pdfData, wasmPath);
}

// ─── extract_page() — one-shot convenience helper ────────────────────────────

export async function extract_page(pdfData, pageIndex = 0, mode = 'text', sort = false) {
  const doc = await open(pdfData);
  try {
    return doc[pageIndex].get_text(mode, sort);
  } finally {
    doc.close();
  }
}

// ─── winnerz namespace — default export ────────────────────────────────────────

const winnerz = {
  open,
  Document: (pdfData, wasmPath = '') => Document.create(pdfData, wasmPath),
  extract_page,
  load_module: load_winnerz_module,
  Page,
};

export default winnerz;