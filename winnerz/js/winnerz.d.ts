/**
 * winnerz.d.ts
 * TypeScript declarations cho WinnerZ WASM + winnerz_wrapper.js
 */

// ═══════════════════════════════════════════════════════════════════════════════
// 1. Data structures (từ JSON responses của C++)
// ═══════════════════════════════════════════════════════════════════════════════

export type BBox = [number, number, number, number];

export interface WasmChar {
  c: string;
  u: number;
  origin: [number, number];
  bbox: BBox;
  bidi: number;
  wmode: number;
  flags: number;
}

export interface WasmSpan {
  text: string;
  bbox: BBox;
  origin: [number, number];
  font: string;
  size: number;
  ascender: number;
  descender: number;
  color: number;
  flags: number;
  chars?: WasmChar[];
}

export interface WasmLine {
  bbox: BBox;
  wmode: number;
  dir: [number, number];
  spans: WasmSpan[];
}

export interface WasmBlock {
  type: 0 | 1;
  bbox: BBox;
  lines: WasmLine[];
}

export interface WasmPageDict {
  page_num: number;
  width: number;
  height: number;
  blocks: WasmBlock[];
}

export interface WasmSimpleBlock {
  bbox: BBox;
  text: string;
  type: 0 | 1;
  block_no: number;
}

export interface PageRect {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
  width: number;
  height: number;
}

export type WasmFontBasenameMap = Record<string, string>;

/** Cấu trúc trả về khi Render Page (PDFium) */
export interface WasmRenderedPage {
  width: number;
  height: number;
  channels: number;
  stride: number;
  samples: Uint8Array;
}

/** Hình vẽ Vector từ PDFium */
export interface WasmDrawingItem {
  type: number;
  scissor_clip: BBox;
  fill_color?: { components: number[]; alpha: number };
  stroke_color?: { components: number[]; alpha: number };
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. Low-level C++ Embind interface (WasmDocument)
// ═══════════════════════════════════════════════════════════════════════════════

export interface WasmDocument {
  // ── Basic info ──────────────────────────────────────────────────────────────
  pageCount(): number;
  isEncrypted(): boolean;
  pageRect(pageIndex: number): string;
  clearPageCache(): void;

  // ── Text extraction ─────────────────────────────────────────────────────────
  getTextPlain(pageIndex: number, sort?: boolean): string;
  getDict(pageIndex: number, sort?: boolean): string;
  getRawDict(pageIndex: number, sort?: boolean): string;
  getBlocks(pageIndex: number, sort?: boolean): string;
  extractText(pageIndex: number): string;
  getAllText(): string;
  getJson(pageIndex: number, includeChars?: boolean, sort?: boolean): string;
  getAllDictsJson(includeChars?: boolean, sort?: boolean): string;

  // ── Font & Draw & Render (PDFium) ───────────────────────────────────────────
  getPageFontBasenames(pageIndex: number): string;
  getDrawings(pageIndex: number): WasmDrawingItem[];
  renderPage(pageIndex: number, scale: number, clip: number[] | Float32Array | null): WasmRenderedPage;

  // ── Redaction (Trả về Uint8Array) ───────────────────────────────────────────
  redactPagesBytes(pageRectsMapJson: string): Uint8Array;
  redactRects(pageIndex: number, rectsJson: string): Uint8Array;

  // ── Insert (Trả về Uint8Array) ──────────────────────────────────────────────
  insertTextToPagesJson(jsonStr: string, fontsDir: string): Uint8Array;
  insertTextToPagesFitSpacingJson(jsonStr: string, fontsDir: string): Uint8Array;
  insertRectsToPagesJson(jsonStr: string): Uint8Array;

  // ── Lifecycle ───────────────────────────────────────────────────────────────
  delete(): void;
}

export interface WinnerzModule {
  // Constructor nhận Uint8Array thay vì string
  Document: new (pdfBytes: Uint8Array) => WasmDocument;
  FS: {
    mkdir(path: string): void;
    writeFile(path: string, data: Uint8Array | string): void;
    readFile(path: string): Uint8Array;
    unlink(path: string): void;
  };
  measureTextWidth(text: string, font_path: string, font_size: number, is_bold: boolean, is_italic: boolean): number;
}

declare function createWinnerzModule(options?: {
  locateFile?: (path: string, prefix: string) => string;
}): Promise<WinnerzModule>;

export default createWinnerzModule;

// ═══════════════════════════════════════════════════════════════════════════════
// 3. High-level wrapper API (winnerz_wrapper.js)
// ═══════════════════════════════════════════════════════════════════════════════

export declare class Page {
  readonly doc: Document;
  readonly index: number;
  readonly number: number;
  readonly rect: PageRect;

  get_text(mode?: 'text' | 'dict' | 'rawdict' | 'json' | 'rawjson' | 'blocks', sort?: boolean): string | WasmPageDict | WasmSimpleBlock[];
  get_page_font_basenames(): WasmFontBasenameMap;
  redact_text(rects: Array<[number, number, number, number]>): Uint8Array;

  /** Đã hỗ trợ trong WASM (PDFium) */
  get_drawings(): WasmDrawingItem[];

  /** Render trang PDF ra hình ảnh (Pixel data) */
  get_pixmap(scale?: number, clip?: [number, number, number, number] | null): Promise<WasmRenderedPage>;
}

export declare class Document {
  [pageIndex: number]: Page;

  readonly length: number;
  readonly page_count: number;
  readonly is_encrypted: boolean;

  static create(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;

  get_page(pageIndex: number): Page;
  page_rect(pageIndex: number): PageRect;

  get_all_text(): string;
  get_all_dicts_json(include_chars?: boolean, sort?: boolean): string;
  get_text(pageIndex: number, mode?: 'text' | 'dict' | 'rawdict' | 'json' | 'rawjson' | 'blocks', sort?: boolean): string | WasmPageDict | WasmSimpleBlock[];

  get_page_font_basenames(pageIndex: number): WasmFontBasenameMap;

  clear_page_cache(): void;

  redact_page(pageIndex: number, rects: Array<[number, number, number, number]>): Uint8Array;
  redact_pages_bytes(page_rects_map: Record<number, Array<[number, number, number, number]>>): Uint8Array;

  insert_rects_json(page_tasks_map: Record<number, Array<{
    rect: [number, number, number, number];
    color: [number, number, number];
    pad?: number;
  }>>): Uint8Array;

  insert_text_json(page_tasks_map: Record<number, Array<{
    text: string;
    rect: [number, number, number, number];
    size?: number;
    color?: [number, number, number];
    bold?: boolean;
    italic?: boolean;
    multiline?: boolean;
    font_family?: string;
  }>>, fonts_dir?: string): Uint8Array;

  /** Căn lề chữ (Fit spacing mode) */
  insert_text_fit_spacing_json(page_tasks_map: Record<number, Array<{
    text: string;
    rect: [number, number, number, number];
    size?: number;
    color?: [number, number, number];
    bold?: boolean;
    italic?: boolean;
    multiline?: boolean;
    font_family?: string;
  }>>, fonts_dir?: string): Uint8Array;

  close(): void;
  [Symbol.dispose](): void;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. Top-level helper functions & Namespace
// ═══════════════════════════════════════════════════════════════════════════════

export declare function load_winnerz_module(wasmPath?: string): Promise<WinnerzModule>;
export declare function open(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;
export declare function extract_page(
  pdfData: Uint8Array | ArrayBuffer,
  pageIndex?: number,
  mode?: 'text' | 'dict' | 'rawdict' | 'json' | 'rawjson' | 'blocks',
  sort?: boolean
): Promise<string | WasmPageDict | WasmSimpleBlock[]>;

export interface WinnerzNamespace {
  open(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;
  Document(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;
  extract_page(
    pdfData: Uint8Array | ArrayBuffer,
    pageIndex?: number,
    mode?: 'text' | 'dict' | 'rawdict' | 'json' | 'rawjson' | 'blocks',
    sort?: boolean
  ): Promise<string | WasmPageDict | WasmSimpleBlock[]>;
  load_module(wasmPath?: string): Promise<WinnerzModule>;
  Page: typeof Page;
}

declare const winnerz: WinnerzNamespace;
export default winnerz;