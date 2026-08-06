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
  color: string;
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
  lines?: WasmLine[]; // type 0
  width?: number;     // type 1
  height?: number;    // type 1
  ext?: string;       // type 1
  image?: string;     // type 1
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

export interface WasmPdfiumEditorDoc {
  import_pages(src: WasmPdfiumEditorDoc, pageRange: string): void;
  save(incremental?: boolean): Uint8Array;
  clean_contents(pageIndex: number): void;
  insert_image_rgba(pageIndex: number, width: number, height: number, rgbaBytes: Uint8Array, rect: [number, number, number, number]): void;
  optimize(maxDpi: number, jpegQuality: number): void;
  close(): void;
  delete(): void;
}

export interface WinnerzModule {
  // Constructor nhận Uint8Array thay vì string
  Document: new (pdfBytes: Uint8Array) => WasmDocument;
  /** PDFium editor (chỉ khi WINNERZ_USE_PDFIUM_PREVIEW=1) */
  PdfiumEditorDoc?: new (pdfBytes?: Uint8Array) => WasmPdfiumEditorDoc;
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
  get_pixmap(options?: number | { scale?: number, clip?: [number, number, number, number] | null, hide_text?: boolean }, clip?: [number, number, number, number] | null): WasmRenderedPage;
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

  /** Chèn ảnh RGBA vào trang */
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
// 3b. PdfiumEditorDoc — High-level wrapper cho WasmPdfiumEditorDoc
// ═══════════════════════════════════════════════════════════════════════════════

export declare class PdfiumEditorDoc {
  /** Load PDF từ bytes. */
  static create(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<PdfiumEditorDoc>;
  /** Tạo tài liệu PDF rỗng. */
  static create_empty(wasmPath?: string): Promise<PdfiumEditorDoc>;

  /**
   * Ghép trang từ tài liệu nguồn vào tài liệu hiện tại.
   * @param srcEditor  Tài liệu nguồn
   * @param pageRange  Chuỗi range trang (VD: "1,3,5-7"). Rỗng = toàn bộ.
   */
  import_pages(srcEditor: PdfiumEditorDoc, pageRange?: string): void;

  /**
   * Xuất tài liệu ra bytes PDF.
   * @param incremental Lưu kiểu incremental (nhanh, file lớn hơn)
   */
  save(incremental?: boolean): Uint8Array;

  /**
   * Xoá toàn bộ nội dung (objects) trong một trang.
   */
  clean_contents(pageIndex: number): void;

  /**
   * Chèn ảnh RGBA vào một vị trí trên trang.
   * @param pageIndex  Chỉ số trang
   * @param width      Chiều rộng ảnh (pixel)
   * @param height     Chiều cao ảnh (pixel)
   * @param rgbaBytes  Dữ liệu pixel RGBA (width * height * 4 bytes)
   * @param rect       [x0, y0, x1, y1] trong toạ độ PDF
   */
  insert_image_rgba(
    pageIndex: number,
    width: number,
    height: number,
    rgbaBytes: Uint8Array,
    rect: [number, number, number, number]
  ): void;

  /**
   * Tối ưu hoá / nén ảnh trong toàn bộ tài liệu.
   * @param maxDpi      DPI tối đa muốn giữ lại (mặc định: 144)
   * @param jpegQuality Chất lượng JPEG 1–100 (mặc định: 80)
   */
  optimize(maxDpi?: number, jpegQuality?: number): void;

  /** Đóng và giải phóng bộ nhớ WASM. */
  close(): void;
  [Symbol.dispose](): void;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. Top-level helper functions & Namespace
// ═══════════════════════════════════════════════════════════════════════════════

export declare function load_winnerz_module(wasmPath?: string): Promise<WinnerzModule>;
export declare function open(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;
export declare function open_editor(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<PdfiumEditorDoc>;
export declare function measure_text_width(
  text: string,
  fontPath: string,
  fontSize: number,
  isBold?: boolean,
  isItalic?: boolean,
  wasmPath?: string
): Promise<number>;
export declare function extract_page(
  pdfData: Uint8Array | ArrayBuffer,
  pageIndex?: number,
  mode?: 'text' | 'dict' | 'rawdict' | 'json' | 'rawjson' | 'blocks',
  sort?: boolean
): Promise<string | WasmPageDict | WasmSimpleBlock[]>;

export interface WinnerzNamespace {
  open(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;
  Document(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<Document>;
  open_editor(pdfData: Uint8Array | ArrayBuffer, wasmPath?: string): Promise<PdfiumEditorDoc>;
  PdfiumEditorDoc: typeof PdfiumEditorDoc;
  measure_text_width(
    text: string,
    fontPath: string,
    fontSize: number,
    isBold?: boolean,
    isItalic?: boolean,
    wasmPath?: string
  ): Promise<number>;
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