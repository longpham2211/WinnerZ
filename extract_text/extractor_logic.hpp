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
    bool is_synthetic = false;
};

struct WinLine {
    Rect bbox;
    Vec2 dir;
    int wmode;
    bool joined; 
    std::vector<WinChar> chars;
};

enum class BlockType { TEXT, IMAGE, VECTOR };

struct WinBlock {
    BlockType type;
    Rect bbox;
    std::vector<WinLine> lines;
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

    WinPage finish_page();
    std::string get_text(const WinPage& page);

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
    bool dehyphenate = true;
    bool new_obj;
    bool maybe_bullet;
};

} // namespace WinExtract