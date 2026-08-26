# WinnerZ WASM — JavaScript API Reference

WinnerZ WASM is a WebAssembly port of the WinnerZ PDF library. The JavaScript API **mirrors the Python API exactly** — utilizing the same class names, method names, and argument orders. This parity ensures that developers can transition seamlessly between Python backend environments and JavaScript browser/Node.js environments without referencing separate documentation.

The module runs entirely client-side in the browser or via Node.js, requiring no backend server for processing.

---

## Installation

Copy the following files to your project's static asset directory:

| File | Description |
| --- | --- |
| `winnerz_wasm.js` | Emscripten glue code (auto-generated) |
| `winnerz_wasm.wasm` | Compiled WebAssembly binary |
| `winnerz_wrapper.js` | High-level JavaScript wrapper **(use this)** |
| `winnerz.d.ts` | TypeScript declarations |

> **Note:** Standard `file://` URLs will not work for loading the WASM file. You must serve these files via a standard HTTP server.

---

## Import

```javascript
// Recommended — mirrors Python: import winnerz
import winnerz from './winnerz_wrapper.js';

// Named imports are also available for destructuring
import { open, Document, Page, load_winnerz_module } from './winnerz_wrapper.js';

```

---

## 1. Opening a PDF

```python
# Python
import winnerz
doc = winnerz.open(path)           # from file
doc = winnerz.Document(bytes_data) # from bytes

```

```javascript
// JavaScript — identical call style
import winnerz from './winnerz_wrapper.js';

const response = await fetch('sample.pdf');
const pdfBytes = new Uint8Array(await response.arrayBuffer());

const doc = await winnerz.open(pdfBytes);           // equivalent to Python winnerz.open()
const doc2 = await winnerz.Document(pdfBytes);      // equivalent to Python winnerz.Document()

```

---

## 2. Document Properties

```python
# Python
len(doc)          # total pages
doc.is_encrypted  # bool

```

```javascript
// JavaScript
doc.length        // total pages (Python: len(doc))
doc.page_count    // same as doc.length
doc.is_encrypted  // boolean (Python: doc.is_encrypted)

```

---

## 3. Accessing Pages

```python
# Python
page       = doc[0]    # first page
last_page  = doc[-1]   # last page
page.number            # 0-based index
page.parent            # back-reference to doc
page.rect              # Rect(x0, y0, x1, y1)

```

```javascript
// JavaScript — identical syntax
const page      = doc[0];    // first page
const last_page = doc[-1];   // last page
page.number                  // 0-based index (Python: page.number)
page.parent                  // back-reference to doc (Python: page.parent)
page.rect                    // { x0, y0, x1, y1, width, height } (Python: page.rect)

// Alternative method:
const page2 = doc.get_page(0);
const rect  = doc.page_rect(0);

```

---

## 4. Text Extraction

### Single Page

```python
# Python
text    = doc[0].get_text('text')
dict_   = doc[0].get_text('dict')
rawdict = doc[0].get_text('rawdict')
json_   = doc[0].get_text('json')
blocks  = doc[0].get_text('blocks')
sorted_ = doc[0].get_text('text', sort=True)

```

```javascript
// JavaScript — identical call style
const text    = doc[0].get_text('text');
const dict_   = doc[0].get_text('dict');       // default mode
const rawdict = doc[0].get_text('rawdict');
const json_   = doc[0].get_text('json');       // raw JSON string, avoids JSON.parse overhead
const blocks  = doc[0].get_text('blocks');
const sorted_ = doc[0].get_text('text', true); // sort=true

```

**Mode reference:**

| Mode | Return Type | Description |
| --- | --- | --- |
| `'text'` | `string` | Plain text, lines separated by `\n` |
| `'dict'` | `WasmPageDict` object | Blocks → lines → spans with bbox, font, color |
| `'rawdict'` | `WasmPageDict` object | Same as `dict` but spans include `chars[]` per character |
| `'json'` | `string` | Raw JSON string of `WasmPageDict` |
| `'rawjson'` | `string` | Raw JSON string including `chars[]` |
| `'blocks'` | `WasmSimpleBlock[]` | Flat list of simple text blocks |

### All Pages

```python
# Python
all_text  = doc.get_all_text()
json_str  = doc.get_all_dicts_json(include_chars=False, sort=False)
all_dicts = json.loads(json_str)  # WasmPageDict[]

```

```javascript
// JavaScript — identical API
const all_text  = doc.get_all_text();
const json_str  = doc.get_all_dicts_json();             // raw JSON string
const json_full = doc.get_all_dicts_json(true);         // includes chars[]
const json_sort = doc.get_all_dicts_json(false, true);  // sorted output
const all_dicts = JSON.parse(json_str);                 // WasmPageDict[]

```

---

## 5. Font Information

```python
# Python
fonts = doc.get_page_font_basenames(page_index=0)
# -> { 'R14': 'TimesNewRomanPS-BoldMT', ... }

fonts2 = doc[0].get_page_font_basenames()  # via page object

```

```javascript
// JavaScript — identical syntax
const fonts  = doc.get_page_font_basenames();     // page_index defaults to 0
const fonts2 = doc.get_page_font_basenames(2);    // specifying page index
const fonts3 = doc[0].get_page_font_basenames();  // via page object

```

---

## 6. Image Extraction

```python
# Python
images = doc[0].get_images()
images_json = doc[0].get_images_json()
```

```javascript
// JavaScript — identical API
const images = doc[0].get_images();           // Returns array of objects with raw stream bytes
const images_json = doc[0].get_images_json(); // Returns JSON string containing Base64 PNGs
```

---

## 6. Get Current PDF Bytes

```python
# Python
raw_bytes = doc.tobytes()

```

```javascript
// JavaScript
const bytes = doc.tobytes();  // Returns Uint8Array
// Note: In WASM, this returns the original bytes used to open the document.
// For PDFs modified by redact/insert operations, utilize the bytes returned directly by those functions.

```

---

## 7. Redaction

Permanently removes text by overlaying filled rectangles. Returns a **new PDF** as a `Uint8Array`.

```python
# Python
# Single page
new_bytes = doc[0].redact_text(rects, output_path)

# Multiple pages
new_bytes = doc.redact_pages_bytes({ page_index: [[x0,y0,x1,y1], ...] })

```

```javascript
// JavaScript — returns memory bytes instead of writing to a file

// Single page (via page object)
const rects   = [[50, 100, 300, 120]];  // [[x0, y0, x1, y1], ...]
const newPdf1 = doc[0].redact_text(rects);

// Single page (via document object)
const newPdf2 = doc.redact_page(0, rects);

// Multiple pages simultaneously
const newPdf3 = doc.redact_pages_bytes({
  0: [[50, 100, 300, 120]],
  2: [[10, 200, 400, 220], [10, 250, 200, 270]]
});

// Trigger download in browser
const blob = new Blob([newPdf3], { type: 'application/pdf' });
const url  = URL.createObjectURL(blob);

```

---

## 8. Insert Colored Rectangles

```python
# Python
doc.insert_rects_json(json_str)

```

```javascript
// JavaScript — pass object directly (the wrapper automatically serializes to JSON)
const newPdf = doc.insert_rects_json({
  0: [
    { rect: [50, 100, 300, 120], color: [255, 0, 0], pad: 2 },  // Red rectangle
    { rect: [50, 140, 300, 160], color: [0, 0, 255] }           // Blue rectangle
  ]
});

```

> **Note:** Add `pad: 2` (2-3 points) around text bounding boxes to ensure complete visual coverage.

---

## 9. Insert Text

Renders text into the PDF using FreeType + HarfBuzz natively within WebAssembly.

### Step 1 — Mount fonts into the WASM virtual filesystem

```javascript
const W = await winnerz.load_module();  // Access the raw Emscripten module
W.FS.mkdir('/fonts');

const fontResp  = await fetch('MyFont-Bold.ttf');
const fontBytes = new Uint8Array(await fontResp.arrayBuffer());
W.FS.writeFile('/fonts/MyFont-Bold.ttf', fontBytes);

```

### Step 2 — Insert text (Standard Mode)

```python
# Python
doc.insert_text_json(json_str, fonts_dir='')

```

```javascript
// JavaScript
const newPdf = doc.insert_text_json({
  0: [{
    text:        'Hello WinnerZ',
    rect:        [50, 50, 300, 80],
    size:        14,
    color:       [0, 0, 0],
    bold:        false,
    italic:      false,
    multiline:   true,       // enables word-wrap within the rect
    font_family: 'MyFont-Bold'
  }]
}, '/fonts');

```

### Step 2b — Insert text with letter-spacing compression (Fit Spacing)

When text is too wide for the bounding box, this mode compresses letter spacing instead of shrinking the font size or wrapping text. This is highly effective for translated documents where single-line layouts must be strictly preserved.

```python
# Python
doc.insert_text_fit_spacing_json(json_str, fonts_dir='')

```

```javascript
// JavaScript — identical task format
const newPdf = doc.insert_text_fit_spacing_json({
  0: [{
    text:        'Long translated text that must fit on one line',
    rect:        [50, 50, 300, 70],
    size:        12,
    color:       [0, 0, 0],
    font_family: 'MyFont-Bold'
  }]
}, '/fonts');

```

**Task Fields Reference:**

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `text` | `string` | `''` | Text content to insert |
| `rect` | `[x0,y0,x1,y1]` | required | Bounding box (top-down PDF coordinates) |
| `size` | `number` | `12` | Font size in points |
| `color` | `[r,g,b]` | `[0,0,0]` | Text color (0–255 per RGB channel) |
| `bold` | `boolean` | `false` | Apply bold style |
| `italic` | `boolean` | `false` | Apply italic style |
| `multiline` | `boolean` | `false` | Enable word-wrap within bounding box |
| `font_family` | `string` | `''` | Font family name (matches TTF filename without extension) |

---

## 10. Cache Management

```python
# Python
doc.clear_page_cache()

```

```javascript
// JavaScript
doc.clear_page_cache();
// It is recommended to call this periodically when processing large documents to reduce peak RAM usage.

```

---

## 11. Closing (CRITICAL)

WebAssembly does **not** automatically garbage-collect C++ objects. You must explicitly call `close()` to prevent memory leaks in the browser.

```python
# Python
doc.close()

```

```javascript
// JavaScript — explicit call required
doc.close();

```

### Recommended Implementation Pattern

```javascript
const doc = await winnerz.open(pdfBytes);
try {
  for (let i = 0; i < doc.length; i++) {
    const text = doc[i].get_text('text');
    // Data processing logic...
    
    if (i % 10 === 0) doc.clear_page_cache(); // Manage memory consumption
  }
} finally {
  doc.close(); // Ensures closure even if an exception occurs
}

```

### TC39 Explicit Resource Management (Stage 3)

```javascript
{
  await using doc = await winnerz.open(pdfBytes);
  console.log(doc[0].get_text('text'));
} // doc.close() is invoked automatically at the end of the block

```

---

## 12. Rendering & Vector Graphics (PDFium Enabled)

Unlike many lightweight WASM ports, WinnerZ WASM includes the PDFium engine, enabling browser-native page rendering and vector path extraction.

### Render Page to Image (Pixmap)

```javascript
// JavaScript
const scale = 1.5; // Zoom scale factor (default is 1.0)
const clip = null; // Alternatively, provide [x0, y0, x1, y1] for cropping

const pixmap = doc[0].get_pixmap(scale, clip);
console.log(`Rendered: ${pixmap.width}x${pixmap.height} at ${pixmap.channels} channels`);

// The raw image bytes (RGBA) can be drawn directly to an HTML5 Canvas:
const imageData = new ImageData(
  new Uint8ClampedArray(pixmap.samples.buffer), 
  pixmap.width, 
  pixmap.height
);
// ctx.putImageData(imageData, 0, 0);

```

### Extract Vector Drawings

```javascript
// JavaScript
const drawings = doc[0].get_drawings();
for (const path of drawings) {
  console.log("Path Type:", path.type);
  console.log("Fill Color:", path.fill_color);
  console.log("Stroke Color:", path.stroke_color);
}

```

---

## 13. One-shot Helper

```javascript
// Open document, extract a single page, and close — executing seamlessly in one call
const text = await winnerz.extract_page(pdfBytes, 0, 'text');
const dict = await winnerz.extract_page(pdfBytes, 0, 'dict');

```

---

## 14. Data Structures

### `WasmPageDict`

```typescript
{
  page_num: number;
  width:    number;
  height:   number;
  blocks:   WasmBlock[];
}

```

### `WasmBlock`

```typescript
{
  type:  0 | 1;     // 0 = text, 1 = image
  bbox:  [x0, y0, x1, y1];
  lines: WasmLine[];
}

```

### `WasmLine`

```typescript
{
  bbox:  [x0, y0, x1, y1];
  wmode: number;           // 0 = horizontal layout, 1 = vertical layout
  dir:   [dx, dy];
  spans: WasmSpan[];
}

```

### `WasmSpan`

```typescript
{
  text:      string;
  bbox:      [x0, y0, x1, y1];
  origin:    [x, y];
  font:      string;
  size:      number;
  ascender:  number;
  descender: number;
  color:     number;   // 0xRRGGBB integer format
  flags:     number;   // 2=italic, 4=serif, 8=mono, 16=bold
  chars?:    WasmChar[]; // Included only when include_chars=true (rawdict/rawjson)
}

```

---

## 15. API Cheatsheet

| Python | JavaScript |
| --- | --- |
| `import winnerz` | `import winnerz from './winnerz_wrapper.js'` |
| `winnerz.open(path)` | `await winnerz.open(pdfBytes)` |
| `winnerz.Document(bytes)` | `await winnerz.Document(pdfBytes)` |
| `len(doc)` | `doc.length` |
| `doc[0]` | `doc[0]` |
| `doc[-1]` | `doc[-1]` |
| `page.number` | `page.number` |
| `page.parent` | `page.parent` |
| `page.rect` | `page.rect` |
| `page.get_text('text')` | `page.get_text('text')` |
| `page.get_text('dict')` | `page.get_text('dict')` |
| `page.get_text('rawdict')` | `page.get_text('rawdict')` |
| `page.get_text('blocks')` | `page.get_text('blocks')` |
| `page.get_drawings()` | `page.get_drawings()` |
| `page.get_pixmap(scale, clip)` | `page.get_pixmap(scale, clip)` |
| `page.redact_text(rects, path)` | `page.redact_text(rects)` → `Uint8Array` |
| `doc.get_all_text()` | `doc.get_all_text()` |
| `doc.get_all_dicts_json(ic, sort)` | `doc.get_all_dicts_json(ic, sort)` |
| `doc.get_page_font_basenames(i=0)` | `doc.get_page_font_basenames(i=0)` |
| `doc.tobytes()` | `doc.tobytes()` → `Uint8Array` |
| `doc.insert_text_json(json, dir)` | `doc.insert_text_json(tasks, dir)` |
| `doc.insert_text_fit_spacing_json(json, dir)` | `doc.insert_text_fit_spacing_json(tasks, dir)` |
| `doc.insert_rects_json(json)` | `doc.insert_rects_json(tasks)` |
| `doc.redact_pages_bytes(map)` | `doc.redact_pages_bytes(map)` |
| `doc.clear_page_cache()` | `doc.clear_page_cache()` |
| `doc.close()` | `doc.close()` |

---

## 16. Unsupported Features (WASM Limitations)

Thanks to the integrated PDFium engine, the majority of advanced features are fully supported. However, the following Python methods are **not available** in WASM due to browser and sandbox constraints:

| Python Method | Reason |
| --- | --- |
| `page.insert_image()` | Requires complex image codec integration not yet enabled in the WASM build |
| `page.show_pdf_page()` | Requires PDFium overlay rendering capabilities |
| `page.clean_contents()` | Not exposed in the current WASM architecture |
| `doc.save(path)` | Browsers lack native file system access (utilize the returned `Uint8Array` bytes instead) |

Invoking these methods on a WASM `Page` or `Document` object will throw a descriptive `Error` at runtime.