# WinnerZ WASM API Documentation

WinnerZ WASM provides a user-friendly JavaScript wrapper (`WinnerzPdf`) around the underlying WebAssembly (Emscripten) module. The API is designed to closely match the WinnerZ Python library while running entirely in the browser or Node.js.

## 1. Import and Open a PDF

Use the `WinnerzPdf` wrapper to load and interact with the WASM module:

```javascript
import { WinnerzPdf } from './winnerz_wrapper.js';

// Read the PDF file into a Uint8Array
const response = await fetch('sample.pdf');
const pdfBytes = new Uint8Array(await response.arrayBuffer());

// Create a PDF instance (automatically loads the WASM module)
const pdf = await Winnerz.open(pdfBytes);
```

## 2. Basic Properties

- `pdf.pageCount`: (Number) Total number of pages in the PDF.
- `pdf.isEncrypted`: (Boolean) Indicates whether the PDF is password-protected.

## 3. Data Extraction APIs

All page indices (`page_index`) are **zero-based**.

### Get Page Dimensions

```javascript
const rect = pdf.pageRect(page_index);
// Returns: { x0, y0, x1, y1 }
```

### Extract Plain Text

```javascript
// Get the text from a specific page (supports reading-order sorting with sort=true)
const text = pdf.getTextPlain(page_index, true);

// Get the text from all pages
const allText = pdf.getAllText();
```

### Extract Dictionary Data (JSON)

Retrieve detailed structured data, including coordinates, font information, and colors for each character or word.

```javascript
// Get the simplified dictionary (characters grouped into text lines)
const dict = pdf.getDict(page_index, true);

// Get the raw dictionary (individual characters)
const rawDict = pdf.getRawDict(page_index, true);

// Get dictionaries for all pages
const allDicts = pdf.getAllDicts(false, true);
// - includeChars: whether to include bounding boxes for individual characters
// - sort: sort the output in reading order (top-to-bottom, left-to-right)
```

### Get Text Blocks

```javascript
// Get the list of text blocks on a page
const blocks = pdf.getBlocks(page_index, true);
```

### Get Font Information

```javascript
// Returns an array of font base names used on the page
const fonts = pdf.getPageFontBasenames(page_index);
```

## 4. PDF Editing APIs (Redact & Insert)

These APIs modify the PDF structure and return a new PDF as a `Uint8Array`. They are implemented in native C++ and do not rely on PDFium, making them fully compatible with WebAssembly environments.

### Redact Sensitive Content

Cover and permanently remove the underlying text within one or more rectangular regions.

```javascript
// Rectangle format: [{ x0, y0, x1, y1, color: [r, g, b] }, ...]
const rects = [
  { x0: 50, y0: 100, x1: 200, y1: 120, color: [0, 0, 0] } // Black redaction box
];

// Redact a single page
const newPdfBytes1 = pdf.redactPage(0, rects);

// Redact multiple pages
// pageRectsMap format: { "page_index": [rect1, rect2], "page_index_2": [...] }
const newPdfBytes2 = pdf.redactPages({
  "0": rects,
  "2": rects
});
```

### Insert Rectangles

Draw one or more rectangles on top of the PDF.

```javascript
const pagesTasksMap = {
  "0": [
    { rect: [50, 100, 200, 120], color: [255, 0, 0], pad: 2.0 }
  ]
};

const modifiedBytes = pdf.insertRects(pagesTasksMap);
```

### Insert Text

Draw new text onto the PDF. A font directory is required (virtualized when running in the browser).

```javascript
const textTasks = {
  "0": [
    {
      text: "Hello WinnerZ",
      rect: [50, 50, 250, 80],
      size: 14.0,
      color: [0, 0, 255],
      bold: false,
      italic: false,
      multiline: true,
      font_family: "Arial"
    }
  ]
};

// Pass the path to the font directory
const finalPdfBytes = pdf.insertText(textTasks, "/fonts");
```

## 5. Memory Cleanup

WebAssembly does not automatically garbage collect underlying C++ objects. **Important:** Always call `close()` after you are finished using a PDF to release native resources and prevent memory leaks, especially in browser environments.

```javascript
// Clear the page cache to reduce temporary memory usage
pdf.clearPageCache();

// Destroy the PDF object (must be called when finished)
pdf.close();
```