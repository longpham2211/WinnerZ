/**
 * winnerz_wrapper.js
 * Friendly JavaScript wrapper around the WinnerZ WASM module.
 *
 * This module handles:
 *  - Loading and initializing the WASM binary
 *  - Converting Uint8Array ↔ binary string for Embind
 *  - Automatic memory cleanup (delete() calls)
 *  - Parsing JSON responses back to JS objects
 *
 * APIs (mirrors Python winnerz API):
 *   Winnerz.open(pdfBytes) → Winnerz
 *   pdf.pageCount, pdf.isEncrypted
 *   pdf.pageRect(i), pdf.getText(i, mode, sort)
 *   pdf.getTextPlain(i, sort), pdf.getDict(i, sort), pdf.getRawDict(i, sort)
 *   pdf.getBlocks(i, sort), pdf.getJson(i, includeChars, sort)
 *   pdf.getAllText(), pdf.getAllDicts(includeChars, sort)
 *   pdf.getPageFontBasenames(i)
 *   pdf.redactPage(i, rects) → Uint8Array     (pure C++, no PDFium)
 *   pdf.redactPages(pageRectsMap) → Uint8Array (pure C++, no PDFium)
 *   pdf.insertRects(pagesTasksMap) → Uint8Array (pure C++, no PDFium)
 *   pdf.insertText(pagesTasksMap, fontsDir) → Uint8Array (FreeType+HarfBuzz)
 *   pdf.clearPageCache(), pdf.close()
 *
 * Usage (browser, ESM):
 *   import { winnerz } from './winnerz_wrapper.js';
 *   const pdf = await winnerz.open(pdfBytes);
 *   const text = pdf.getTextPlain(0);
 *   const page = pdf.getDict(0);
 *   pdf.close();
 */

// ─── Module loader (singleton) ────────────────────────────────────────────────

let _modulePromise = null;
let _wasmModule = null;

/**
 * Load (or return cached) WinnerZ WASM module.
 * @param {string} [wasmPath] - Optional path prefix for locating winnerz_wasm.wasm
 * @returns {Promise<object>} The Emscripten module object
 */
export async function loadWinnerzModule(wasmPath = '') {
  if (_wasmModule) return _wasmModule;
  if (_modulePromise) return _modulePromise;

  _modulePromise = (async () => {
    // Dynamic import works in both browser and Node.js
    const factory = (await import('./winnerz_wasm.js')).default;

    const opts = {};
    if (wasmPath) {
      opts.locateFile = (file, prefix) => {
        if (file.endsWith('.wasm')) return wasmPath + file;
        return prefix + file;
      };
    }

    _wasmModule = await factory(opts);
    return _wasmModule;
  })();

  return _modulePromise;
}

// ─── Helpers: Uint8Array ↔ binary string (Embind std::string) ──────────────────

/**
 * Convert a Uint8Array to a binary string for Embind.
 * Embind maps std::string as a binary string in JavaScript.
 * @param {Uint8Array} bytes
 * @returns {string}
 */
function uint8ArrayToString(bytes) {
  // For large files, chunked approach avoids call-stack overflow
  const CHUNK = 65536;
  let result = '';
  for (let i = 0; i < bytes.length; i += CHUNK) {
    result += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return result;
}

/**
 * Convert a binary string (returned from Embind std::string) back to Uint8Array.
 * @param {string} str
 * @returns {Uint8Array}
 */
function stringToUint8Array(str) {
  const out = new Uint8Array(str.length);
  for (let i = 0; i < str.length; ++i) {
    out[i] = str.charCodeAt(i) & 0xff;
  }
  return out;
}

// ─── Winnerz – the friendly API class ─────────────────────────────────────

/**
 * Friendly wrapper around WasmDocument.
 * Mirrors the Python winnerz.Document API as closely as possible.
 */
export class winnerz {
  /**
   * Open a PDF from bytes.
   * @param {Uint8Array|ArrayBuffer} pdfData - Raw PDF bytes
   * @param {string} [wasmPath] - Optional path prefix for locating .wasm file
   * @returns {Promise<winnerz>}
   */
  static async open(pdfData, wasmPath = '') {
    const W = await loadWinnerzModule(wasmPath);
    const bytes = pdfData instanceof Uint8Array ? pdfData : new Uint8Array(pdfData);
    const byteStr = uint8ArrayToString(bytes);
    const wasmDoc = new W.WasmDocument(byteStr);
    return new winnerz(wasmDoc);
  }

  /**
   * @param {object} wasmDoc - The raw WasmDocument Embind object
   */
  constructor(wasmDoc) {
    this._doc = wasmDoc;
    this._closed = false;
  }

  _assertOpen() {
    if (this._closed) throw new Error('Winnerz: document has been closed');
  }

  // ── Basic info ──────────────────────────────────────────────────────────────

  /** @returns {number} Total page count */
  get pageCount() {
    this._assertOpen();
    return this._doc.pageCount();
  }

  /** @returns {boolean} Whether PDF is encrypted */
  get isEncrypted() {
    this._assertOpen();
    return this._doc.isEncrypted();
  }

  /**
   * Get page dimensions.
   * Equivalent to Python: doc[i].rect
   * @param {number} pageIndex
   * @returns {{ x0: number, y0: number, x1: number, y1: number, width: number, height: number }}
   */
  pageRect(pageIndex) {
    this._assertOpen();
    const arr = JSON.parse(this._doc.pageRect(pageIndex));
    return { x0: arr[0], y0: arr[1], x1: arr[2], y1: arr[3], width: arr[2] - arr[0], height: arr[3] - arr[1] };
  }

  // ── Text extraction ─────────────────────────────────────────────────────────

  /**
   * Extract plain text from a page.
   * Equivalent to Python: page.get_text("text")
   * @param {number} pageIndex
   * @param {boolean} [sort=false]
   * @returns {string}
   */
  getTextPlain(pageIndex, sort = false) {
    this._assertOpen();
    return this._doc.getTextPlain(pageIndex, sort);
  }

  /**
   * Extract text as structured dict object.
   * Equivalent to Python: page.get_text("dict")
   * @param {number} pageIndex
   * @param {boolean} [sort=false]
   * @returns {import('./winnerz.d.ts').WasmPageDict}
   */
  getDict(pageIndex, sort = false) {
    this._assertOpen();
    return JSON.parse(this._doc.getJson(pageIndex, false, sort));
  }

  /**
   * Extract text as structured dict with individual character data.
   * Equivalent to Python: page.get_text("rawdict")
   * @param {number} pageIndex
   * @param {boolean} [sort=false]
   * @returns {import('./winnerz.d.ts').WasmPageDict}
   */
  getRawDict(pageIndex, sort = false) {
    this._assertOpen();
    return JSON.parse(this._doc.getJson(pageIndex, true, sort));
  }

  /**
   * Extract text as JSON string (no parsing).
   * Equivalent to Python: page.get_text("json")
   * @param {number} pageIndex
   * @param {boolean} [includeChars=false]
   * @param {boolean} [sort=false]
   * @returns {string}
   */
  getJson(pageIndex, includeChars = false, sort = false) {
    this._assertOpen();
    return this._doc.getJson(pageIndex, includeChars, sort);
  }

  /**
   * Extract simplified text blocks.
   * Equivalent to Python: page.get_text("blocks")
   * @param {number} pageIndex
   * @param {boolean} [sort=false]
   * @returns {import('./winnerz.d.ts').WasmSimpleBlock[]}
   */
  getBlocks(pageIndex, sort = false) {
    this._assertOpen();
    return JSON.parse(this._doc.getBlocksJson(pageIndex, sort));
  }

  /**
   * Extract text using a mode string (like Python API).
   * Equivalent to Python: page.get_text(mode)
   * @param {number} pageIndex
   * @param {'text'|'dict'|'rawdict'|'json'|'rawjson'|'blocks'} [mode='dict']
   * @param {boolean} [sort=false]
   */
  getText(pageIndex, mode = 'dict', sort = false) {
    this._assertOpen();
    switch (mode) {
      case 'text': return this.getTextPlain(pageIndex, sort);
      case 'dict': return this.getDict(pageIndex, sort);
      case 'rawdict': return this.getRawDict(pageIndex, sort);
      case 'json': return this._doc.getJson(pageIndex, false, sort);
      case 'rawjson': return this._doc.getJson(pageIndex, true, sort);
      case 'blocks': return this.getBlocks(pageIndex, sort);
      default: throw new Error(`Winnerz: unknown text mode "${mode}"`);
    }
  }

  /**
   * Extract text from ALL pages at once.
   * Equivalent to Python: doc.get_all_text()
   * @returns {string} Pages joined with \f (form-feed)
   */
  getAllText() {
    this._assertOpen();
    return this._doc.getAllText();
  }

  /**
   * Extract structured JSON for ALL pages.
   * Equivalent to Python: doc.get_all_dicts_json()
   * @param {boolean} [includeChars=false]
   * @param {boolean} [sort=false]
   * @returns {import('./winnerz.d.ts').WasmPageDict[]}
   */
  getAllDicts(includeChars = false, sort = false) {
    this._assertOpen();
    return JSON.parse(this._doc.getAllDictsJson(includeChars, sort));
  }

  // ── Font info ───────────────────────────────────────────────────────────────

  /**
   * Get real font names for a page.
   * Equivalent to Python: doc.get_page_font_basenames(i)
   * @param {number} pageIndex
   * @returns {Record<string, string>} { "R14": "TimesNewRoman-Bold", ... }
   */
  getPageFontBasenames(pageIndex) {
    this._assertOpen();
    return JSON.parse(this._doc.getPageFontBasenames(pageIndex));
  }

  // ── Cache management ────────────────────────────────────────────────────────

  /**
   * Free internal page cache memory.
   * Equivalent to Python: doc.clear_page_cache()
   */
  clearPageCache() {
    this._assertOpen();
    this._doc.clearPageCache();
  }

  // ── Redaction – pure C++, no PDFium ────────────────────────────────────────

  /**
   * Redact (black-out) rectangular areas on one page.
   * Equivalent to Python: doc[i].redact_text(rects, output_path) but returns bytes.
   * @param {number} pageIndex
   * @param {Array<[number,number,number,number]>} rects – [[x0,y0,x1,y1], ...] in top-down coords
   * @returns {Uint8Array} New PDF bytes
   */
  redactPage(pageIndex, rects) {
    this._assertOpen();
    const rectsJson = JSON.stringify(rects);
    const binStr = this._doc.redactPageBytes(pageIndex, rectsJson);
    return stringToUint8Array(binStr);
  }

  /**
   * Redact multiple pages at once.
   * Equivalent to Python: doc.redact_pages_bytes({page_index: [[x0,y0,x1,y1], ...]})
   * @param {Record<number, Array<[number,number,number,number]>>} pageRectsMap
   * @returns {Uint8Array} New PDF bytes
   * @example
   * const pdfBytes = pdf.redactPages({ 0: [[10,10,200,30]], 1: [[50,50,300,80]] });
   */
  redactPages(pageRectsMap) {
    this._assertOpen();
    const json = JSON.stringify(
      Object.fromEntries(
        Object.entries(pageRectsMap).map(([k, v]) => [String(k), v])
      )
    );
    const binStr = this._doc.redactPagesBytes(json);
    return stringToUint8Array(binStr);
  }

  // ── Insert colored rectangles – pure C++ ────────────────────────────────────

  /**
   * Insert colored rectangles into pages.
   * Equivalent to Python: doc.insert_rects_json(json_str)
   * @param {Record<number, Array<{rect:[x0,y0,x1,y1], color:[r,g,b], pad?:number}>>} pageTasksMap
   * @returns {Uint8Array} New PDF bytes
   * @example
   * const pdfBytes = pdf.insertRects({
   *   0: [{ rect: [10,10,200,30], color: [255,0,0] }]
   * });
   */
  insertRects(pageTasksMap) {
    this._assertOpen();
    const json = JSON.stringify(
      Object.fromEntries(
        Object.entries(pageTasksMap).map(([k, v]) => [String(k), v])
      )
    );
    const binStr = this._doc.insertRectsBytes(json);
    return stringToUint8Array(binStr);
  }

  // ── Insert text – FreeType+HarfBuzz, pure C++, NO PDFium ────────────────────

  /**
   * Insert text into pages.
   * Equivalent to Python: doc.insert_text_json(json_str, fonts_dir)
   *
   * In WASM, fonts must be pre-loaded into the virtual filesystem:
   *   const W = await createWinnerzModule();
   *   W.FS.mkdir('/fonts');
   *   W.FS.writeFile('/fonts/MyFont-Bold.ttf', fontBytes);
   *   const pdf = await winnerz.open(pdfBytes);
   *   const out = pdf.insertText({ 0: [{ ... font_family: 'MyFont-Bold' }] }, '/fonts');
   *
   * @param {Record<number, Array<{
   *   text: string,
   *   rect: [x0,y0,x1,y1],
   *   size?: number,
   *   color?: [r,g,b],
   *   bold?: boolean,
   *   italic?: boolean,
   *   multiline?: boolean,
   *   font_family?: string
   * }>>} pageTasksMap
   * @param {string} [fontsDir='/fonts'] Path in WASM virtual FS
   * @returns {Uint8Array} New PDF bytes
   */
  insertText(pageTasksMap, fontsDir = '/fonts') {
    this._assertOpen();
    const json = JSON.stringify(
      Object.fromEntries(
        Object.entries(pageTasksMap).map(([k, tasks]) => [
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
    const binStr = this._doc.insertTextBytes(json, fontsDir);
    return stringToUint8Array(binStr);
  }

  // ── Lifecycle ───────────────────────────────────────────────────────────────

  /**
   * Close and free C++ memory.
   * Equivalent to Python: doc.close()
   * ⚠️ Always call this when done!
   */
  close() {
    if (!this._closed && this._doc) {
      this._doc.delete();
      this._doc = null;
      this._closed = true;
    }
  }

  /**
   * Async-safe cleanup helper for use with try/finally.
   * @example
   * using pdf = await winnerz.open(bytes); // Stage-3 'using' proposal
   */
  [Symbol.dispose]() { this.close(); }
}

// ─── Quick-use helper ─────────────────────────────────────────────────────────

/**
 * One-shot helper: open PDF, extract text from one page, then close.
 * @param {Uint8Array|ArrayBuffer} pdfData
 * @param {number} [pageIndex=0]
 * @param {'text'|'dict'|'rawdict'|'json'|'rawjson'|'blocks'} [mode='text']
 * @param {boolean} [sort=false]
 * @returns {Promise<string|object>}
 */
export async function extractPage(pdfData, pageIndex = 0, mode = 'text', sort = false) {
  const pdf = await winnerz.open(pdfData);
  try {
    return pdf.getText(pageIndex, mode, sort);
  } finally {
    pdf.close();
  }
}
