# WinnerZ WASM API Documentation

WinnerZ WASM cung cấp một lớp wrapper thân thiện (`WinnerzPdf`) bằng JavaScript, đóng gói WebAssembly (Emscripten). API này được thiết kế để giống hệt với thư viện WinnerZ trên Python, nhưng chạy hoàn toàn trên trình duyệt hoặc Node.js.

## 1. Import và Mở file PDF

Bạn cần sử dụng wrapper `WinnerzPdf` để load và giao tiếp với WASM:

```javascript
import { WinnerzPdf } from './winnerz_wrapper.js';

// Đọc file PDF thành mảng byte (Uint8Array)
const response = await fetch('sample.pdf');
const pdfBytes = new Uint8Array(await response.arrayBuffer());

// Khởi tạo đối tượng PDF (Tự động tải WASM module)
const pdf = await WinnerzPdf.open(pdfBytes);
```

## 2. Các thuộc tính cơ bản

- `pdf.pageCount`: (Number) Tổng số trang của file PDF.
- `pdf.isEncrypted`: (Boolean) File PDF có bị mã hóa (đặt mật khẩu) hay không.

## 3. Các hàm trích xuất dữ liệu (Extract)

Tất cả chỉ số trang (`page_index`) đều bắt đầu từ **0**.

### Lấy kích thước trang
```javascript
const rect = pdf.pageRect(page_index);
// Trả về: { x0, y0, x1, y1 }
```

### Lấy Text (Văn bản thô)
```javascript
// Lấy text của 1 trang cụ thể (có hỗ trợ cờ sắp xếp sort=true)
const text = pdf.getTextPlain(page_index, true);

// Lấy text của tất cả các trang
const allText = pdf.getAllText();
```

### Lấy dữ liệu Dictionaries (JSON)
Lấy dữ liệu cấu trúc chi tiết (vị trí tọa độ, font chữ, màu sắc) của từng ký tự / từ.
```javascript
// Lấy Dict rút gọn (gom nhóm ký tự thành dòng)
const dict = pdf.getDict(page_index, true);

// Lấy Dict đầy đủ dạng Raw (từng ký tự rời rạc)
const rawDict = pdf.getRawDict(page_index, true);

// Lấy toàn bộ Dictionaries của mọi trang
const allDicts = pdf.getAllDicts(false, true); 
// - includeChars: bao gồm bounding box của từng ký tự riêng lẻ không
// - sort: sắp xếp theo luồng đọc từ trên xuống, trái sang phải
```

### Lấy Blocks
```javascript
// Lấy danh sách các khối (block) văn bản
const blocks = pdf.getBlocks(page_index, true);
```

### Lấy thông tin Fonts
```javascript
// Trả về mảng danh sách tên Font (basename) được dùng trên trang
const fonts = pdf.getPageFontBasenames(page_index);
```

## 4. Các hàm chỉnh sửa (Redact & Insert)

Các hàm này thay đổi cấu trúc PDF và trả về một file PDF mới dưới dạng `Uint8Array`. Chúng chạy bằng C++ thuần (không cần PDFium) nên hoạt động tốt trên môi trường WASM.

### Xóa dữ liệu nhạy cảm (Redact)
Che và xóa nội dung văn bản bên dưới một vùng hình chữ nhật.

```javascript
// Cấu trúc của rects: [{ x0, y0, x1, y1, color: [r, g, b] }, ...]
const rects = [
  { x0: 50, y0: 100, x1: 200, y1: 120, color: [0, 0, 0] } // Che khối đen
];

// Redact trên 1 trang
const newPdfBytes1 = pdf.redactPage(0, rects);

// Redact trên nhiều trang cùng lúc
// Cấu trúc pageRectsMap: { "page_index": [rect1, rect2], "page_index_2": [...] }
const newPdfBytes2 = pdf.redactPages({
  "0": rects,
  "2": rects
});
```

### Chèn Hình chữ nhật (Insert Rects)
Vẽ thêm hình chữ nhật đè lên PDF.

```javascript
const pagesTasksMap = {
  "0": [
    { rect: [50, 100, 200, 120], color: [255, 0, 0], pad: 2.0 }
  ]
};

const modifiedBytes = pdf.insertRects(pagesTasksMap);
```

### Chèn Văn bản (Insert Text)
Vẽ thêm chữ mới vào PDF. Cần thư mục font chữ (ảo hóa nếu chạy trên trình duyệt).

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

// Truyền kèm đường dẫn thư mục chứa font 
const finalPdfBytes = pdf.insertText(textTasks, "/fonts");
```

## 5. Dọn dẹp bộ nhớ (Cleanup)

WASM không có Garbage Collector tự động dọn dẹp các class C++. **Rất quan trọng:** Bạn luôn phải gọi hàm `close()` sau khi dùng xong file PDF để giải phóng bộ nhớ (tránh memory leak trên trình duyệt).

```javascript
// Xóa cache của trang (giảm RAM tạm thời)
pdf.clearPageCache();

// Hủy hoàn toàn đối tượng PDF (Bắt buộc gọi khi không dùng nữa)
pdf.close();
```
