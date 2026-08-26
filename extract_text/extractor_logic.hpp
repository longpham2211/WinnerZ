#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace WinExtract {

struct Vec2 { float x, y; };
struct Quad { Vec2 ul, ur, ll, lr; };
struct Rect { float x0, y0, x1, y1; };


struct WinChar {
    int c;
    int bidi;
    Vec2 origin;
    Quad quad;
    float size;
    uint32_t color;
    bool is_bold;
    bool is_italic;
    bool is_serif;
    bool is_mono;
    std::string font_name;
    float ascender = 0.8f;
    float descender = -0.2f;

    bool is_underlined = false;
    bool is_strikeout = false;
    bool is_synthetic = false;       // inserted space, not present in original content
    bool is_synthetic_large = false; // synthetic space inserted for a "large" gap (>2*SPACE_DIST)
};

struct WinLine {
    Rect bbox;
    Vec2 dir;
    int wmode;
    bool joined; 
    std::vector<WinChar> chars;
};

enum class BlockType { TEXT, IMAGE, VECTOR };

struct WinImageXObject;

struct WinBlock {
    BlockType type;
    Rect bbox;
    std::vector<WinLine> lines;
    
    // Image specific fields
    std::string image_base64;
    std::string image_ext;
    int image_width = 0;
    int image_height = 0;
    std::string image_color_space;
    std::vector<float> image_decode;
};

struct WinPage {
    Rect mediabox;
    std::vector<WinBlock> blocks;
};

class WinTextExtractor {
public:
    WinTextExtractor();

    void begin_page(float width, float height);
    void hint_new_text_obj(); 
    
    void add_char(int unicode, float x, float y, float adv, float matrix[6], 
                  const std::string& font_name, float size, uint32_t color, 
                  bool bold, bool italic, bool serif, bool mono, int wmode, float ascender, float descender, 
                  int bidi_level, bool has_real_glyph);
                  
    void add_image(const Rect& bbox, const WinImageXObject& img);

    WinPage finish_page();
    std::string get_text(const WinPage& page);

    // If true, use the (approximate) glyph bbox info supplied by the caller as-is;
    // reserved hook for callers that later plug in real glyph-outline bboxes.
    bool accurate_bboxes = false;
    // If true, never insert synthetic space characters between glyphs.
    bool inhibit_spaces = false;
    // If true, do NOT expand ligatures (ff, fi, fl, ffi, ffl, st, and Unicode
    // presentation forms) into their constituent characters.
    bool preserve_ligatures = false;
    // If true, do NOT normalize whitespace variants (tab, nbsp, unicode spaces...) to ' '.
    bool preserve_whitespace = false;
    // If true, run the fake-bold detection pass (scans previously emitted chars
    // on the page for overlaid duplicate glyphs). 
    bool collect_styles = true;
    // Whether to attempt to join hyphenated words across lines.
    bool dehyphenate = true;

private:
    void add_char_imp(int c, int glyph, float adv, float matrix[6], const std::string& font_name, 
                      float size, uint32_t color, bool bold, bool italic, bool serif, bool mono,
                      int wmode, int bidi, bool force_new_line, float ascender, float descender, bool is_synthetic_space);

    WinPage page;
    WinBlock* cur_block;
    WinLine* cur_line;
    WinLine* last_line;

    int last_char;
    int last_bidi;
    Vec2 pen;
    Vec2 lag_pen;
    Vec2 start;
    bool new_obj;
    bool maybe_bullet;
    bool last_was_fake_bold;
};

} // namespace WinExtract