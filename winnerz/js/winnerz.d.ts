/**
 * winnerz.d.ts
 * TypeScript type declarations for the WinnerZ WASM module.
 *
 * Usage (browser):
 *   import createWinnerzModule from './winnerz_wasm.js';
 *   const W = await createWinnerzModule();
 *   const doc = new W.WasmDocument(pdfBytes);  // pdfBytes: Uint8Array
 *   console.log(doc.pageCount());
 *   doc.delete(); // free C++ memory when done
 *
 * Usage (Node.js / bundler):
 *   const createWinnerzModule = require('./winnerz_wasm.js');
 *   const W = await createWinnerzModule();
 *   ...
 */

// ─── Data structures returned as JSON strings ─────────────────────────────────

/** [x0, y0, x1, y1] bounding box in points */
export type BBox = [number, number, number, number];

/** A single character in a span */
export interface WasmChar {
  /** UTF-8 character string */
  c: string;
  /** Unicode codepoint */
  u: number;
  /** Character origin [x, y] */
  origin: [number, number];
  /** Character bounding box [x0, y0, x1, y1] */
  bbox: BBox;
  /** BiDi level */
  bidi: number;
  /** Writing mode (0=horizontal, 1=vertical) */
  wmode: number;
  /** Font flags bitmask: 2=italic, 4=serif, 8=mono, 16=bold */
  flags: number;
}

/** A text span (run of characters with the same font/size/color) */
export interface WasmSpan {
  /** Extracted plain text of this span */
  text: string;
  /** Span bounding box */
  bbox: BBox;
  /** First character origin */
  origin: [number, number];
  /** Font name (PDF resource name or base font) */
  font: string;
  /** Font size in points */
  size: number;
  /** Ascender value (normalized, e.g. 0.8) */
  ascender: number;
  /** Descender value (normalized, e.g. -0.2) */
  descender: number;
  /** Text color as 0xRRGGBB integer */
  color: number;
  /** Font flags bitmask: 2=italic, 4=serif, 8=mono, 16=bold */
  flags: number;
  /** Individual characters (only present when include_chars=true) */
  chars?: WasmChar[];
}

/** A text line */
export interface WasmLine {
  /** Line bounding box */
  bbox: BBox;
  /** Writing mode (0=horizontal, 1=vertical) */
  wmode: number;
  /** Line direction vector [dx, dy] */
  dir: [number, number];
  /** Spans in this line */
  spans: WasmSpan[];
}

/** A text/image block */
export interface WasmBlock {
  /** 0 = text block, 1 = image block */
  type: 0 | 1;
  /** Block bounding box */
  bbox: BBox;
  /** Lines (only present for text blocks) */
  lines: WasmLine[];
}

/** Full page extraction result */
export interface WasmPageDict {
  /** 0-based page number */
  page_num: number;
  /** Page width in points */
  width: number;
  /** Page height in points */
  height: number;
  /** All blocks on the page */
  blocks: WasmBlock[];
}

/** Simplified block (returned by getBlocksJson) */
export interface WasmSimpleBlock {
  /** Block bounding box */
  bbox: BBox;
  /** All text in the block, lines separated by '\n' */
  text: string;
  /** 0 = text, 1 = image */
  type: 0 | 1;
}

/** Font basename map: { resourceName: baseFontName } */
export type WasmFontBasenameMap = Record<string, string>;

// ─── Main document class ──────────────────────────────────────────────────────

/**
 * WasmDocument – wraps a PDF document in WASM memory.
 *
 * ⚠️ Always call `doc.delete()` when finished to free C++ memory!
 * You can also wrap usage in a try/finally block.
 */
export interface WasmDocument {
  // ── Basic info ──────────────────────────────────────────────────────────────

  /** Total number of pages */
  pageCount(): number;

  /** Whether the PDF is encrypted */
  isEncrypted(): boolean;

  /**
   * Returns the page rectangle as a JSON string: "[x0, y0, x1, y1]"
   * Typically x0=0, y0=0, x1=width, y1=height.
   */
  pageRect(pageIndex: number): string;

  // ── Text extraction ─────────────────────────────────────────────────────────

  /**
   * Extract plain text from a page.
   * Equivalent to Python: page.get_text("text")
   * @param pageIndex 0-based page index
   * @param sort Whether to sort output top-to-bottom, left-to-right
   */
  getTextPlain(pageIndex: number, sort?: boolean): string;

  /**
   * Extract structured text as a JSON string (WasmPageDict).
   * Equivalent to Python: page.get_text("json") / page.get_text("rawjson")
   * @param pageIndex 0-based page index
   * @param includeChars Include individual char data in spans
   * @param sort Whether to sort output
   */
  getJson(pageIndex: number, includeChars?: boolean, sort?: boolean): string;

  /**
   * Same as getJson but always includes character data.
   * Equivalent to Python: page.get_text("rawjson")
   */
  getRawJson(pageIndex: number, sort?: boolean): string;

  /**
   * Extract text blocks as a JSON string (WasmSimpleBlock[]).
   * Equivalent to Python: page.get_text("blocks")
   */
  getBlocksJson(pageIndex: number, sort?: boolean): string;

  /**
   * Extract plain text from ALL pages, joined with form-feed (\f) between pages.
   * Equivalent to Python: doc.get_all_text()
   * Note: In WASM this runs single-threaded (no multithreading).
   */
  getAllText(): string;

  /**
   * Extract structured JSON for ALL pages as a JSON array string (WasmPageDict[]).
   * Equivalent to Python: doc.get_all_dicts_json(include_chars, sort)
   */
  getAllDictsJson(includeChars?: boolean, sort?: boolean): string;

  // ── Font info ───────────────────────────────────────────────────────────────

  /**
   * Get font basename map for a page.
   * Returns a JSON string: { "R14": "TimesNewRoman-Bold", ... }
   * Equivalent to Python: doc.get_page_font_basenames(page_index)
   */
  getPageFontBasenames(pageIndex: number): string;

  // ── Cache management ────────────────────────────────────────────────────────

  /**
   * Clear the internal page cache to free memory.
   * Call this after you finish processing a page on large documents.
   */
  clearPageCache(): void;

  // ── Emscripten object lifetime ──────────────────────────────────────────────

  /**
   * ⚠️ MUST be called when you are done with the document to free C++ memory.
   * Failing to call this causes memory leaks in long-running browser sessions.
   */
  delete(): void;
}

// ─── Module factory ───────────────────────────────────────────────────────────

/** The instantiated WASM module, returned by createWinnerzModule() */
export interface WinnerzModule {
  /**
   * The WasmDocument constructor.
   * Pass raw PDF bytes as a Uint8Array (or string).
   *
   * @example
   * const response = await fetch('document.pdf');
   * const buffer = await response.arrayBuffer();
   * const pdfBytes = new Uint8Array(buffer);
   * // Convert Uint8Array to string for Embind
   * const byteStr = String.fromCharCode(...pdfBytes);
   * const doc = new W.WasmDocument(byteStr);
   * console.log(doc.pageCount());
   * doc.delete();
   */
  WasmDocument: new (pdfBytesAsString: string) => WasmDocument;
}

/**
 * Initialize the WinnerZ WASM module.
 * Call once at startup and reuse the returned module object.
 *
 * @example
 * import createWinnerzModule from './winnerz_wasm.js';
 * const W = await createWinnerzModule();
 * // W is now ready to use
 */
declare function createWinnerzModule(options?: {
  /** Path prefix for locating the .wasm file */
  locateFile?: (path: string, prefix: string) => string;
}): Promise<WinnerzModule>;

export default createWinnerzModule;
