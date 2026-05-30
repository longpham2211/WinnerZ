#ifndef PDF_ENGINE_HPP
#define PDF_ENGINE_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace winnerz {

struct RectF {
    float x0;
    float y0;
    float x1;
    float y1;
};

struct Matrix2D {
    float a = 1.0f;
    float b = 1.0f;
};

struct CharItem {
    uint32_t unicode = 0;
    RectF bbox{};
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float font_size = 0.0f;
    uint32_t color_rgb = 0;   // 0xRRGGBB
    uint32_t flags = 0;       // bold/italic/etc.
    int bidi = 0;
    int wmode = 0;
    std::string font_name;
};

struct SpanItem {
    RectF bbox{};
    float size = 0.0f;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float ascender = 0.0f;
    float descender = 0.0f;
    uint32_t color_rgb = 0;
    uint32_t flags = 0;
    std::string font_name;
    std::string text;
    std::vector<CharItem> chars;
};

struct LineItem {
    RectF bbox{};
    int wmode = 0;
    float dir_x = 1.0f;
    float dir_y = 0.0f;
    std::vector<SpanItem> spans;
};

struct TextBlockItem {
    int type = 0; // 0=text, other values reserved for images/graphics
    RectF bbox{};
    std::vector<LineItem> lines;
};

struct TextDict {
    std::vector<TextBlockItem> blocks;
};

struct DrawingItem {
    std::optional<RectF> rect;
    std::optional<std::array<float, 3>> fill_rgb01; // each channel in [0,1]
};

struct PixmapBuffer {
    int width = 0;
    int height = 0;
    int channels = 3;
    int stride = 0;
    std::vector<uint8_t> data;
};

struct TextExtractOptions {
    bool sort = false;
};

struct RenderOptions {
    Matrix2D matrix{};
    std::optional<RectF> clip;
};

struct RedactOptions {
    int images = 0;
    int graphics = 0;
};

struct RedactAnnot {
    RectF rect{};
    std::optional<std::array<float, 3>> fill_rgb01;
};

class IPage {
public:
    virtual ~IPage() = default;

    virtual RectF GetRect() const = 0;
    virtual TextDict GetTextDict(const TextExtractOptions& opts) const = 0;
    virtual std::vector<std::array<float, 7>> GetTextBlocks() const = 0;
    virtual std::vector<DrawingItem> GetDrawings() const = 0;
    virtual PixmapBuffer RenderPixmap(const RenderOptions& opts) const = 0;

    virtual void AddRedactAnnot(const RedactAnnot& annot) = 0;
    virtual void ApplyRedactions(const RedactOptions& opts) = 0;

    virtual void ShowPdfPage(
        const RectF& target_rect,
        class IDocument& src_doc,
        int src_page_index,
        bool overlay) = 0;
};

class IDocument {
public:
    virtual ~IDocument() = default;

    virtual int PageCount() const = 0;
    virtual IPage& PageAt(int index) = 0;
    virtual const IPage& PageAt(int index) const = 0;

    virtual void Save(const std::string& output_path, bool clean, bool deflate) = 0;
    virtual void Close() = 0;
};

} // namespace winnerz

#endif // PDF_ENGINE_HPP
