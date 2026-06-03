#include "micro_ocr.hpp"
#include "micro_ocr_templates.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifdef WINEXTRACT_USE_FREETYPE
#include <ft2build.h>
#include FT_BITMAP_H
#endif

#ifdef _MSC_VER
#include <intrin.h>
#define POPCOUNT64 __popcnt64
#else
#define POPCOUNT64 __builtin_popcountll
#endif

#include <unordered_map>
#include <mutex>

namespace WinExtract {

// Thread-local Bitmap Hash Cache
static thread_local std::unordered_map<uint64_t, std::vector<int>> t_ocr_hash_cache;

inline uint64_t fnv1a_hash_ocr(const uint64_t blocks[16], int contours) {
    uint64_t hash = 14695981039346656037ULL;
    for (int i = 0; i < 16; ++i) {
        uint64_t val = blocks[i];
        for (int b = 0; b < 8; ++b) {
            hash ^= (val & 0xFF);
            hash *= 1099511628211ULL;
            val >>= 8;
        }
    }
    hash ^= contours;
    hash *= 1099511628211ULL;
    return hash;
}



#ifdef WINEXTRACT_USE_FREETYPE

static uint8_t sample_bitmap(const FT_Bitmap& bitmap, float u, float v) {
    if (bitmap.width == 0 || bitmap.rows == 0) return 0;
    
    int x = static_cast<int>(u * bitmap.width);
    int y = static_cast<int>(v * bitmap.rows);
    
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= static_cast<int>(bitmap.width)) x = bitmap.width - 1;
    if (y >= static_cast<int>(bitmap.rows)) y = bitmap.rows - 1;
    
    if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        return bitmap.buffer[y * bitmap.pitch + x];
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        uint8_t byte = bitmap.buffer[y * bitmap.pitch + (x / 8)];
        return (byte & (1 << (7 - (x % 8)))) ? 255 : 0;
    }
    return 0;
}

static std::vector<int> utf8_to_codepoints(const char* str) {
    std::vector<int> result;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(str);
    while (*s) {
        int cp = 0;
        if (*s <= 0x7F) {
            cp = *s++;
        } else if ((*s & 0xE0) == 0xC0) {
            cp = (*s++ & 0x1F) << 6;
            cp |= (*s++ & 0x3F);
        } else if ((*s & 0xF0) == 0xE0) {
            cp = (*s++ & 0x0F) << 12;
            cp |= (*s++ & 0x3F) << 6;
            cp |= (*s++ & 0x3F);
        } else if ((*s & 0xF8) == 0xF0) {
            cp = (*s++ & 0x07) << 18;
            cp |= (*s++ & 0x3F) << 12;
            cp |= (*s++ & 0x3F) << 6;
            cp |= (*s++ & 0x3F);
        } else {
            s++; // skip invalid
        }
        if (cp > 0) result.push_back(cp);
    }
    return result;
}

// LSH by Pixel Density
struct SortedTemplate {
    int index;
    int pixel_count;
};
static std::vector<SortedTemplate> g_sorted_templates;
static bool g_templates_initialized = false;

static void init_sorted_templates() {
    if (g_templates_initialized) return;
    g_sorted_templates.reserve(NUM_MICRO_OCR_TEMPLATES);
    for (int i = 0; i < NUM_MICRO_OCR_TEMPLATES; ++i) {
        int count = POPCOUNT64(MICRO_OCR_TEMPLATES[i].pixels[0]) + 
                    POPCOUNT64(MICRO_OCR_TEMPLATES[i].pixels[1]) + 
                    POPCOUNT64(MICRO_OCR_TEMPLATES[i].pixels[2]) + 
                    POPCOUNT64(MICRO_OCR_TEMPLATES[i].pixels[3]);
        g_sorted_templates.push_back({i, count});
    }
    std::sort(g_sorted_templates.begin(), g_sorted_templates.end(), [](const SortedTemplate& a, const SortedTemplate& b) {
        return a.pixel_count < b.pixel_count;
    });
    g_templates_initialized = true;
}

std::vector<int> run_micro_ocr_on_glyph(FT_Face face, int glyph_id) {
    if (!face) return {};
    
    init_sorted_templates();
    
    // Set a fixed pixel size so the rendered bitmap is comparable to our 16x16 templates
    FT_Set_Pixel_Sizes(face, 0, 16);
    
    // Load the glyph first to get the structural contours BEFORE rendering turns it into a bitmap
    if (FT_Load_Glyph(face, glyph_id, FT_LOAD_DEFAULT) != 0) {
        return {};
    }
    
    int target_contours = face->glyph->outline.n_contours;
    
    // [TOFU FILTER]: Bypass rendering for non-text characters (spaces or complex icons)
    if (target_contours == 0 || target_contours > 5) {
        return {};
    }
    
    // Now render it as a mono bitmap for the pixel fallback
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO) != 0) {
        return {};
    }
    
    FT_Bitmap& bitmap = face->glyph->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0) {
        return {' '}; // Empty glyph is usually a space
    }
    
    // Downsample/upsample the bitmap into a 32x32 buffer packed as 16 uint64_t
    uint64_t rendered_blocks[16] = {0};
    int glyph_pixels = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            float u = (x + 0.5f) / 32.0f;
            float v = (y + 0.5f) / 32.0f;
            uint8_t pixel = sample_bitmap(bitmap, u, v);
            if (pixel > 127) {
                int pixel_idx = y * 32 + x;
                int block_idx = pixel_idx / 64;
                int bit_idx = pixel_idx % 64;
                rendered_blocks[block_idx] |= (1ULL << bit_idx);
                glyph_pixels++;
            }
        }
    }
    
    // [BITMAP HASH CACHE]: Fast lookup before doing POPCNT
    uint64_t hash = fnv1a_hash_ocr(rendered_blocks, target_contours);
    auto cache_it = t_ocr_hash_cache.find(hash);
    if (cache_it != t_ocr_hash_cache.end()) {
        return cache_it->second;
    }

    // Compare with templates
    std::string best_char_utf8 = "";
    float best_score = 0.0f;
    
    for (int i = 0; i < NUM_MICRO_OCR_TEMPLATES; ++i) {
        const MicroOcrTemplate& tpl = MICRO_OCR_TEMPLATES[i];
        
        // Structural Topology Filter: MUST BE EXACT MATCH
        // 5 has 1 contour, 6 has 2 contours, B has 3 contours. 
        if (tpl.contours != target_contours) {
            continue;
        }
        
        long sum_intersection = 0;
        long sum_union = 0;
        
        for (int p = 0; p < 16; ++p) {
            uint64_t a = tpl.pixels[p];
            uint64_t b = rendered_blocks[p];
            sum_intersection += POPCOUNT64(a & b);
            sum_union += POPCOUNT64(a | b);
        }
        
        if (sum_union > 0) {
            float score = static_cast<float>(sum_intersection) / static_cast<float>(sum_union);
            if (score > best_score) {
                best_score = score;
                best_char_utf8 = tpl.utf8_char;
            }
        } else if (sum_intersection == 0 && sum_union == 0) {
            // both empty
            float score = 1.0f;
            if (score > best_score) {
                best_score = score;
                best_char_utf8 = tpl.utf8_char;
                break; // PERF: Early exit for perfect match (empty)
            }
        }
    }
    
    // Threshold for IoU is generally lower than pixel similarity
    // A good match is usually > 0.4 or 0.5 IoU
    if (best_score > 0.30f && !best_char_utf8.empty()) {
        return utf8_to_codepoints(best_char_utf8.c_str());
    }
    
    return {};
}
#endif

// --- VNI & TCVN3 Heuristics ---

bool apply_heuristic_vni_tcvn3(const std::string& font_name, int code, int& out_unicode) {
    if (font_name.empty()) return false;
    
    // Fast path: Only run heuristics if it's a single byte code (VNI/TCVN3 are 8-bit encodings)
    if (code > 255) return false;
    
    // Check if font name hints at VNI
    bool is_vni = (font_name.find("VNI") != std::string::npos); // "VNI" covers "VNI-"
    bool is_tcvn3 = (font_name.find(".Vn") != std::string::npos || font_name.find("VN") != std::string::npos);
    
    if (!is_vni && !is_tcvn3) return false;
    
    if (is_vni) {
        // Simple mapping for VNI single byte to Unicode (Just a tiny subset for demo/heuristic)
        // VNI often uses upper ASCII for tones.
        // Full mapping is large, but we can catch obvious ones.
        if (code >= 32 && code <= 126) {
            out_unicode = code; // Basic ASCII matches
            return true;
        }
    }
    
    if (is_tcvn3) {
        // TCVN3 basic mapping
        if (code >= 32 && code <= 126) {
            out_unicode = code;
            return true;
        }
    }
    
    return false;
}

} // namespace WinExtract
