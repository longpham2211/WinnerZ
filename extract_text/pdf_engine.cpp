#include <unordered_set>
#include <iostream>
#include "pdf_engine.hpp"
#include "micro_ocr.hpp"
#include "parse.hpp"
#include "xref.hpp"
#include "cmap_parse.hpp"
#include "cmap.hpp"
#include "cmap_table.hpp"
#include "unicode.hpp"
#include "type_3.hpp"
#include <fstream>
#include <memory>
#include <algorithm>
#include <utility>
#include <array>
#include <unordered_map>
#include <cctype>
#include <cstdio>

#include <string>
#include <vector>
#include <cctype>
#include <mutex>
#include <memory>

// Global RAM Cache for OCR results to avoid re-OCRing across pages/documents
using GlobalFontUnicodeMap = std::unordered_map<int, std::vector<int>>;
static std::mutex g_global_font_cache_mutex;
static std::unordered_map<uint64_t, std::shared_ptr<GlobalFontUnicodeMap>> g_global_font_cache;

// FNV-1a 64-bit hash
inline uint64_t fnv1a_hash_bytes(const std::vector<uint8_t>& data) {
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t fnv1a_hash_string(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>

// Custom fast parsers to avoid GLIBC_2.38 dependency from __isoc23_sscanf / __isoc23_strtol
static long wz_strtol(const char* nptr, char** endptr, int base) {
    long result = 0; int sign = 1;
    const char* p = nptr;
    while (*p && std::isspace(static_cast<unsigned char>(*p))) p++;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') { p++; }
    if (!std::isdigit(static_cast<unsigned char>(*p))) {
        if (endptr) *endptr = const_cast<char*>(nptr);
        return 0;
    }
    while (*p && std::isdigit(static_cast<unsigned char>(*p))) {
        result = result * base + (*p - '0');
        p++;
    }
    if (endptr) *endptr = const_cast<char*>(p);
    return result * sign;
}

static bool wz_parse_obj_ref(const char* p, int& id, int& gen) {
    char* e1;
    long v1 = wz_strtol(p, &e1, 10);
    if (e1 == p || v1 <= 0) return false;
    char* e2;
    long v2 = wz_strtol(e1, &e2, 10);
    if (e2 == e1 || v2 < 0) return false;
    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) e2++;
    if (*e2 == 'R') { id = v1; gen = v2; return true; }
    return false;
}

static bool wz_parse_obj_header(const char* p, int& id, int& gen) {
    char* e1;
    long v1 = wz_strtol(p, &e1, 10);
    if (e1 == p || v1 <= 0) return false;
    char* e2;
    long v2 = wz_strtol(e1, &e2, 10);
    if (e2 == e1 || v2 < 0) return false;
    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) e2++;
    if (*e2 == 'o' && *(e2+1) == 'b' && *(e2+2) == 'j') { id = v1; gen = v2; return true; }
    return false;
}

static bool wz_parse_xref_line(const char* p, long& offset, int& gen, char& type) {
    char* e1;
    long v1 = wz_strtol(p, &e1, 10);
    if (e1 == p || v1 < 0) return false;
    char* e2;
    long v2 = wz_strtol(e1, &e2, 10);
    if (e2 == e1 || v2 < 0) return false;
    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) e2++;
    if (*e2 == 'n' || *e2 == 'f') { offset = v1; gen = v2; type = *e2; return true; }
    return false;
}

static bool wz_parse_xref_line_3(const char* p, int& id, int& gen, char& r, int& consumed) {
    const char* start = p;
    char* e1;
    long v1 = wz_strtol(p, &e1, 10);
    if (e1 == p) return false;
    char* e2;
    long v2 = wz_strtol(e1, &e2, 10);
    if (e2 == e1) return false;
    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) e2++;
    if (!*e2) return false;
    char tp = *e2;
    e2++;
    consumed = static_cast<int>(e2 - start);
    id = v1; gen = v2; r = tp;
    return true;
}

#include <cstring>
#include <map>
#include <set>
#include <functional>
#include <limits>
#include <cmath>
#include <mutex>
#include "encodings.h"
#include "ucdn.hpp"

#if defined(WINNERZ_USE_LCMS2) && WINNERZ_USE_LCMS2
#include "lcms2.h"
#endif

#ifdef WINEXTRACT_USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace WinExtract {

static std::shared_mutex g_global_freetype_cache_mutex;
static std::unordered_map<uint64_t, std::shared_ptr<std::unordered_map<unsigned int, int>>> g_global_freetype_cache;


static uint8_t hex_to_byte(char h1, char h2) {
    auto to_nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    return (to_nibble(h1) << 4) | to_nibble(h2);
}

namespace {

#ifdef WINEXTRACT_USE_FREETYPE
static FT_Library get_freetype_library() {
    thread_local FT_Library library = nullptr;
    thread_local bool initialized = false;
    if (!initialized) {
        initialized = true;
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
        }
    }
    return library;
}
#endif

#if defined(WINNERZ_USE_LCMS2) && WINNERZ_USE_LCMS2
struct WinIccDeviceConverter {
    cmsHPROFILE rgb_profile = nullptr;
    cmsHPROFILE cmyk_profile = nullptr;
    cmsHPROFILE lab_profile = nullptr;
    cmsHTRANSFORM cmyk_to_rgb = nullptr;
    cmsHTRANSFORM lab_to_rgb = nullptr;
    std::mutex lock;

    WinIccDeviceConverter() {
        auto open_profile = [](const std::vector<const char*>& candidates) -> cmsHPROFILE {
            for (const char* path : candidates) {
                if (path == nullptr || *path == '\0') {
                    continue;
                }
                if (cmsHPROFILE p = cmsOpenProfileFromFile(path, "r")) {
                    return p;
                }
            }
            return nullptr;
        };

        rgb_profile = open_profile({
            "/usr/share/color/icc/ghostscript/default_rgb.icc",
            "/usr/share/color/icc/default_rgb.icc"
        });
        cmyk_profile = open_profile({
            "/usr/share/color/icc/ghostscript/default_cmyk.icc",
            "/usr/share/color/icc/default_cmyk.icc"
        });
        lab_profile = open_profile({
            "/usr/share/color/icc/ghostscript/lab.icc",
            "/usr/share/color/icc/lab.icc"
        });

        if (rgb_profile == nullptr || cmyk_profile == nullptr || lab_profile == nullptr) {
            return;
        }

        const cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_LOWRESPRECALC;
        cmyk_to_rgb = cmsCreateTransform(
            cmyk_profile,
            TYPE_CMYK_16,
            rgb_profile,
            TYPE_RGB_16,
            INTENT_RELATIVE_COLORIMETRIC,
            flags);

        lab_to_rgb = cmsCreateTransform(
            lab_profile,
            TYPE_Lab_16,
            rgb_profile,
            TYPE_RGB_16,
            INTENT_RELATIVE_COLORIMETRIC,
            flags);
    }

    ~WinIccDeviceConverter() {
        if (cmyk_to_rgb != nullptr) {
            cmsDeleteTransform(cmyk_to_rgb);
            cmyk_to_rgb = nullptr;
        }
        if (lab_to_rgb != nullptr) {
            cmsDeleteTransform(lab_to_rgb);
            lab_to_rgb = nullptr;
        }
        if (rgb_profile != nullptr) {
            cmsCloseProfile(rgb_profile);
            rgb_profile = nullptr;
        }
        if (cmyk_profile != nullptr) {
            cmsCloseProfile(cmyk_profile);
            cmyk_profile = nullptr;
        }
        if (lab_profile != nullptr) {
            cmsCloseProfile(lab_profile);
            lab_profile = nullptr;
        }
    }

    bool convert_cmyk(float c, float m, float y, float k, float& r, float& g, float& b) {
        if (cmyk_to_rgb == nullptr) {
            return false;
        }
        const auto to_u16 = [](float v) -> uint16_t {
            const float clamped = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
            return static_cast<uint16_t>(std::lround(clamped * 65535.0f));
        };

        const uint16_t in[4] = {to_u16(c), to_u16(m), to_u16(y), to_u16(k)};
        uint16_t out[3] = {0, 0, 0};
        {
            std::lock_guard<std::mutex> guard(lock);
            cmsDoTransform(cmyk_to_rgb, in, out, 1);
        }
        r = static_cast<float>(out[0]) / 65535.0f;
        g = static_cast<float>(out[1]) / 65535.0f;
        b = static_cast<float>(out[2]) / 65535.0f;
        return true;
    }

    bool convert_lab(float l, float a, float b_in, float& r, float& g, float& b) {
        if (lab_to_rgb == nullptr) {
            return false;
        }
        const float ll = (l < 0.0f) ? 0.0f : ((l > 100.0f) ? 100.0f : l);
        const float aa = (a < -128.0f) ? -128.0f : ((a > 127.0f) ? 127.0f : a);
        const float bb = (b_in < -128.0f) ? -128.0f : ((b_in > 127.0f) ? 127.0f : b_in);

        const uint16_t in[3] = {
            static_cast<uint16_t>(std::lround(ll * 655.35f)),
            static_cast<uint16_t>(std::lround((aa + 128.0f) * 257.0f)),
            static_cast<uint16_t>(std::lround((bb + 128.0f) * 257.0f)),
        };
        uint16_t out[3] = {0, 0, 0};
        {
            std::lock_guard<std::mutex> guard(lock);
            cmsDoTransform(lab_to_rgb, in, out, 1);
        }
        r = static_cast<float>(out[0]) / 65535.0f;
        g = static_cast<float>(out[1]) / 65535.0f;
        b = static_cast<float>(out[2]) / 65535.0f;
        return true;
    }
};

static WinIccDeviceConverter& win_icc_device_converter() {
    static WinIccDeviceConverter converter;
    return converter;
}

static bool win_icc_convert_cmyk_to_rgb(float c, float m, float y, float k, float& r, float& g, float& b) {
    return win_icc_device_converter().convert_cmyk(c, m, y, k, r, g, b);
}

static bool win_icc_convert_lab_to_rgb(float l, float a, float b_in, float& r, float& g, float& b) {
    return win_icc_device_converter().convert_lab(l, a, b_in, r, g, b);
}
#endif


struct PdfToken {
    enum class Type { Number, Name, String, Array, Dictionary };
    Type type = Type::Name;
    double number = 0.0;
    std::string name;
    std::vector<uint8_t> bytes;
    std::vector<PdfToken> items;
    std::string dict;
};

struct TextState {
    float tm[6] = {1, 0, 0, 1, 0, 0};
    float tlm[6] = {1, 0, 0, 1, 0, 0};
    float font_size = 12.0f;
    std::string font_name = "Unknown";
    float char_spacing = 0.0f;
    float word_spacing = 0.0f;
    float h_scale = 100.0f;
    float leading = 0.0f;
    float text_rise = 0.0f;
    int wmode = 0;
};

static std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string normalize_pdf_font_name(std::string name) {
    if (!name.empty() && name[0] == '/') {
        name.erase(name.begin());
    }

    size_t plus = name.find('+');
    if (plus != std::string::npos && plus > 0 && plus <= 7) {
        name = name.substr(plus + 1);
    }
    return name;
}

static float get_base14_width(const std::string& raw_font_name, int char_code) {
    const std::string font_name = normalize_pdf_font_name(raw_font_name);
    const std::string lower_name = to_lower_ascii(font_name);

    if (lower_name.find("courier") != std::string::npos) {
        return 0.600f;
    }

    static const float helv_widths[256] = {
        0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,
        0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,0.278f,
        0.278f,0.278f,0.355f,0.556f,0.556f,0.889f,0.667f,0.191f,0.333f,0.333f,0.389f,0.584f,0.278f,0.333f,0.278f,0.278f,
        0.556f,0.556f,0.556f,0.556f,0.556f,0.556f,0.556f,0.556f,0.556f,0.556f,0.278f,0.278f,0.584f,0.584f,0.584f,0.556f,
        0.730f,0.667f,0.667f,0.722f,0.722f,0.667f,0.611f,0.778f,0.722f,0.278f,0.500f,0.667f,0.556f,0.833f,0.722f,0.778f,
        0.667f,0.778f,0.722f,0.667f,0.611f,0.722f,0.667f,0.944f,0.667f,0.667f,0.611f,0.278f,0.278f,0.278f,0.469f,0.556f,
        0.222f,0.556f,0.556f,0.500f,0.556f,0.556f,0.278f,0.556f,0.556f,0.222f,0.222f,0.500f,0.222f,0.833f,0.556f,0.556f,
        0.556f,0.556f,0.333f,0.500f,0.278f,0.556f,0.500f,0.722f,0.500f,0.500f,0.500f,0.334f,0.260f,0.334f,0.584f,0.278f
    };

    if (char_code < 0 || char_code >= 256) {
        if (lower_name.find("times") != std::string::npos) return 0.500f;
        if (lower_name.find("symbol") != std::string::npos || lower_name.find("zapfdingbats") != std::string::npos) return 0.500f;
        return 0.550f;
    }

    if (lower_name.find("times") != std::string::npos) {
        if (char_code == ' ') return 0.250f;
        if (char_code == '\'' || char_code == ',' || char_code == '.' || char_code == ':' || char_code == ';') return 0.250f;
        if (char_code == '!') return 0.333f;
        if (char_code >= '0' && char_code <= '9') return 0.500f;
        if (char_code == 'I') return 0.333f;
        if (char_code == 'M') return 0.889f;
        if (char_code == 'W') return 0.944f;
        if (char_code >= 'A' && char_code <= 'Z') return 0.667f;
        if (char_code == 'i' || char_code == 'l') return 0.278f;
        if (char_code == 'm') return 0.778f;
        if (char_code == 'w') return 0.722f;
        if (char_code >= 'a' && char_code <= 'z') return 0.444f;
        return 0.500f;
    }

    if (lower_name.find("symbol") != std::string::npos || lower_name.find("zapfdingbats") != std::string::npos) {
        return 0.500f;
    }

    return helv_widths[char_code];
}

static void populate_font_flags_and_properties(WinFontVerticalMetrics& metrics) {
    const std::string lower_font = to_lower_ascii(metrics.base_font);
    
    // Bold
    bool is_bold = false;
    if ((metrics.flags & 262144) != 0 || metrics.font_weight >= 700.0f) {
        is_bold = true;
    }
    if (!is_bold) {
        if (lower_font.find("bold") != std::string::npos ||
            lower_font.find("black") != std::string::npos ||
            lower_font.find("heavy") != std::string::npos ||
            lower_font.find("demi") != std::string::npos ||
            lower_font.find("semibold") != std::string::npos ||
            lower_font.find("extrabold") != std::string::npos) {
            is_bold = true;
        }
    }
    metrics.is_bold = is_bold;

    // Italic
    bool is_italic = false;
    if ((metrics.flags & 64) != 0) {
        is_italic = true;
    }
    if (!is_italic) {
        if (lower_font.find("italic") != std::string::npos ||
            lower_font.find("oblique") != std::string::npos) {
            is_italic = true;
        }
    }
    metrics.is_italic = is_italic;

    // Serifed
    bool is_serif = true;
    if (lower_font.find("sans") != std::string::npos ||
        lower_font.find("arial") != std::string::npos ||
        lower_font.find("helvetica") != std::string::npos ||
        lower_font.find("calibri") != std::string::npos ||
        lower_font.find("tahoma") != std::string::npos ||
        lower_font.find("verdana") != std::string::npos ||
        lower_font.find("futura") != std::string::npos ||
        lower_font.find("optima") != std::string::npos ||
        lower_font.find("segoe") != std::string::npos ||
        lower_font.find("gill") != std::string::npos) {
        is_serif = false;
    }
    metrics.is_serif = is_serif;

    // Monospaced
    bool is_mono = false;
    if ((metrics.flags & 1) != 0) {
        is_mono = true;
    }
    if (!is_mono) {
        if (lower_font.find("courier") != std::string::npos ||
            lower_font.find("mono") != std::string::npos ||
            lower_font.find("consolas") != std::string::npos ||
            lower_font.find("lucida console") != std::string::npos) {
            is_mono = true;
        }
    }
    metrics.is_mono = is_mono;
}

static WinFontVerticalMetrics get_base14_vertical_metrics(const std::string& raw_font_name) {
    const std::string font_name = normalize_pdf_font_name(raw_font_name);
    const std::string lower_name = to_lower_ascii(font_name);

    if (lower_name.find("courier") != std::string::npos || lower_name.find("nimbusmono") != std::string::npos) {
        return {0.629f, -0.157f};
    }
    if (lower_name.find("times") != std::string::npos || lower_name.find("nimbusroman") != std::string::npos) {
        return {0.683f, -0.217f};
    }
    if (lower_name.find("symbol") != std::string::npos || lower_name.find("standardsymbols") != std::string::npos) {
        return {0.692f, -0.241f};
    }
    if (lower_name.find("zapfdingbats") != std::string::npos || lower_name.find("dingbats") != std::string::npos) {
        return {0.820f, -0.143f};
    }

    // Helvetica/Nimbus Sans style fallback.
    return {0.718f, -0.207f};
}

#ifdef WINEXTRACT_USE_FREETYPE
static std::vector<std::string> get_system_font_candidates(const std::string& raw_font_name) {
    std::vector<std::string> candidates;
    const std::string normalized = normalize_pdf_font_name(raw_font_name);
    const std::string lower_name = to_lower_ascii(normalized);

    const bool is_bold =
        (lower_name.find("bold") != std::string::npos) ||
        (lower_name.find("black") != std::string::npos) ||
        (lower_name.find("heavy") != std::string::npos) ||
        (lower_name.find("demi") != std::string::npos) ||
        (lower_name.find("semibold") != std::string::npos) ||
        (lower_name.find("-bd") != std::string::npos) ||
        (lower_name.find("bdmt") != std::string::npos);
    const bool is_italic = (lower_name.find("italic") != std::string::npos) || (lower_name.find("oblique") != std::string::npos);

    auto add_urw_font = [&](const char* file_name) {
        const std::vector<std::string> prefixes = {
            "",
            "../",
            "../../",
            "../../../",
            "../../../../",
            "../../../../../"
        };
        for (const std::string& prefix : prefixes) {
            candidates.push_back(prefix + "pymupdf/mupdf-master/resources/fonts/urw/" + file_name);
        }
    };

    auto add_urw_family = [&](const char* regular, const char* italic, const char* bold, const char* bold_italic) {
        if (is_bold && is_italic) {
            add_urw_font(bold_italic);
        } else if (is_bold) {
            add_urw_font(bold);
        } else if (is_italic) {
            add_urw_font(italic);
        } else {
            add_urw_font(regular);
        }
    };

    if (lower_name.find("symbol") != std::string::npos || lower_name.find("standardsymbols") != std::string::npos) {
        add_urw_font("StandardSymbolsPS.cff");
    } else if (lower_name.find("zapfdingbats") != std::string::npos || lower_name.find("dingbats") != std::string::npos) {
        add_urw_font("Dingbats.cff");
    } else if (lower_name.find("courier") != std::string::npos || lower_name.find("nimbusmono") != std::string::npos) {
        add_urw_family("NimbusMonoPS-Regular.cff", "NimbusMonoPS-Italic.cff", "NimbusMonoPS-Bold.cff", "NimbusMonoPS-BoldItalic.cff");
    } else if (lower_name.find("times") != std::string::npos || lower_name.find("nimbusroman") != std::string::npos) {
        add_urw_family("NimbusRoman-Regular.cff", "NimbusRoman-Italic.cff", "NimbusRoman-Bold.cff", "NimbusRoman-BoldItalic.cff");
    } else if (lower_name.find("helvetica") != std::string::npos || lower_name.find("arial") != std::string::npos || lower_name.find("nimbussans") != std::string::npos) {
        add_urw_family("NimbusSans-Regular.cff", "NimbusSans-Italic.cff", "NimbusSans-Bold.cff", "NimbusSans-BoldItalic.cff");
    }

    const char* windir = std::getenv("WINDIR");
    const std::string fonts_dir = windir ? (std::string(windir) + "\\Fonts\\") : std::string("C:\\Windows\\Fonts\\");

    auto add_font = [&](const char* file_name) {
        candidates.push_back(fonts_dir + file_name);
    };

    if (lower_name.find("helvetica-boldoblique") != std::string::npos || lower_name.find("helvetica-bolditalic") != std::string::npos ||
        (lower_name.find("helvetica") != std::string::npos && is_bold && is_italic)) {
        add_font("arialbi.ttf");
    } else if (lower_name.find("helvetica-bold") != std::string::npos || (lower_name.find("helvetica") != std::string::npos && is_bold)) {
        add_font("arialbd.ttf");
    } else if (lower_name.find("helvetica-oblique") != std::string::npos || lower_name.find("helvetica-italic") != std::string::npos ||
               (lower_name.find("helvetica") != std::string::npos && is_italic)) {
        add_font("ariali.ttf");
    } else if (lower_name.find("helvetica") != std::string::npos) {
        add_font("arial.ttf");
    } else if (lower_name.find("times-bolditalic") != std::string::npos || (lower_name.find("times") != std::string::npos && is_bold && is_italic)) {
        add_font("timesbi.ttf");
    } else if (lower_name.find("times-bold") != std::string::npos || (lower_name.find("times") != std::string::npos && is_bold)) {
        add_font("timesbd.ttf");
    } else if (lower_name.find("times-italic") != std::string::npos || lower_name.find("times-oblique") != std::string::npos ||
               (lower_name.find("times") != std::string::npos && is_italic)) {
        add_font("timesi.ttf");
    } else if (lower_name.find("times") != std::string::npos) {
        add_font("times.ttf");
    } else if (lower_name.find("courier-boldoblique") != std::string::npos || lower_name.find("courier-bolditalic") != std::string::npos ||
               (lower_name.find("courier") != std::string::npos && is_bold && is_italic)) {
        add_font("courbi.ttf");
    } else if (lower_name.find("courier-bold") != std::string::npos || (lower_name.find("courier") != std::string::npos && is_bold)) {
        add_font("courbd.ttf");
    } else if (lower_name.find("courier-oblique") != std::string::npos || lower_name.find("courier-italic") != std::string::npos ||
               (lower_name.find("courier") != std::string::npos && is_italic)) {
        add_font("couri.ttf");
    } else if (lower_name.find("courier") != std::string::npos) {
        add_font("cour.ttf");
    } else if (lower_name.find("symbol") != std::string::npos) {
        add_font("symbol.ttf");
    }

    add_font("arial.ttf");
    add_font("times.ttf");

    return candidates;
}
#endif

static bool is_white(uint8_t c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static bool is_delimiter(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static bool is_number_start(uint8_t c) {
    return c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9');
}

static void skip_ws_and_comments(const std::vector<uint8_t>& stream, size_t& i) {
    while (i < stream.size()) {
        if (is_white(stream[i])) {
            ++i;
            continue;
        }
        if (stream[i] == '%') {
            while (i < stream.size() && stream[i] != '\n' && stream[i] != '\r') {
                ++i;
            }
            continue;
        }
        break;
    }
}

static std::vector<uint8_t> parse_literal_string(const std::vector<uint8_t>& stream, size_t& i) {
    std::vector<uint8_t> out;
    if (i >= stream.size() || stream[i] != '(') {
        return out;
    }

    ++i;
    int depth = 1;
    while (i < stream.size() && depth > 0) {
        uint8_t c = stream[i++];
        if (c == '\\' && i < stream.size()) {
            uint8_t e = stream[i++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '(': out.push_back('('); break;
            case ')': out.push_back(')'); break;
            case '\\': out.push_back('\\'); break;
            case '\r':
                if (i < stream.size() && stream[i] == '\n') {
                    ++i;
                }
                break;
            case '\n':
                break;
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0';
                    int consumed = 0;
                    while (consumed < 2 && i < stream.size() && stream[i] >= '0' && stream[i] <= '7') {
                        v = (v * 8) + (stream[i] - '0');
                        ++i;
                        ++consumed;
                    }
                    out.push_back(static_cast<uint8_t>(v & 0xFF));
                } else {
                    out.push_back(e);
                }
                break;
            }
            continue;
        }

        if (c == '(') {
            ++depth;
            out.push_back(c);
            continue;
        }
        if (c == ')') {
            --depth;
            if (depth > 0) {
                out.push_back(c);
            }
            continue;
        }
        out.push_back(c);
    }
    return out;
}

static std::vector<uint8_t> parse_hex_string(const std::vector<uint8_t>& stream, size_t& i) {
    std::vector<uint8_t> out;
    if (i >= stream.size() || stream[i] != '<' || (i + 1 < stream.size() && stream[i + 1] == '<')) {
        return out;
    }

    ++i;
    std::string hex;
    while (i < stream.size() && stream[i] != '>') {
        if (!is_white(stream[i])) {
            hex.push_back(static_cast<char>(stream[i]));
        }
        ++i;
    }
    if (i < stream.size() && stream[i] == '>') {
        ++i;
    }

    if ((hex.size() % 2) != 0) {
        hex.push_back('0');
    }
    for (size_t j = 0; j + 1 < hex.size(); j += 2) {
        out.push_back(hex_to_byte(hex[j], hex[j + 1]));
    }
    return out;
}

static std::string parse_name_token(const std::vector<uint8_t>& stream, size_t& i) {
    if (i >= stream.size() || stream[i] != '/') {
        return {};
    }
    ++i;
    size_t start = i;
    while (i < stream.size() && !is_white(stream[i]) && !is_delimiter(stream[i])) {
        ++i;
    }
    return std::string(reinterpret_cast<const char*>(stream.data() + start), i - start);
}

static double parse_number_token(const std::vector<uint8_t>& stream, size_t& i) {
    size_t start = i;
    if (stream[i] == '+' || stream[i] == '-') {
        ++i;
    }
    while (i < stream.size() && ((stream[i] >= '0' && stream[i] <= '9') || stream[i] == '.')) {
        ++i;
    }
    std::string n(reinterpret_cast<const char*>(stream.data() + start), i - start);
    return std::strtod(n.c_str(), nullptr);
}

static bool parse_operand(const std::vector<uint8_t>& stream, size_t& i, PdfToken& out);

static std::string parse_dictionary_token(const std::vector<uint8_t>& stream, size_t& i) {
    if (i + 1 >= stream.size() || stream[i] != '<' || stream[i + 1] != '<') {
        return {};
    }

    const size_t start = i;
    i += 2;
    int depth = 1;

    while (i < stream.size() && depth > 0) {
        if (stream[i] == '%') {
            while (i < stream.size() && stream[i] != '\n' && stream[i] != '\r') {
                ++i;
            }
            continue;
        }

        if (stream[i] == '(') {
            parse_literal_string(stream, i);
            continue;
        }

        if (stream[i] == '<') {
            if (i + 1 < stream.size() && stream[i + 1] == '<') {
                ++depth;
                i += 2;
                continue;
            }
            parse_hex_string(stream, i);
            continue;
        }

        if (stream[i] == '>' && i + 1 < stream.size() && stream[i + 1] == '>') {
            --depth;
            i += 2;
            continue;
        }

        ++i;
    }

    if (depth != 0 || i <= start) {
        return {};
    }

    const size_t end = i;
    return std::string(reinterpret_cast<const char*>(stream.data() + start), end - start);
}

static std::vector<PdfToken> parse_array_token(const std::vector<uint8_t>& stream, size_t& i) {
    std::vector<PdfToken> arr;
    if (i >= stream.size() || stream[i] != '[') {
        return arr;
    }
    ++i;
    while (i < stream.size()) {
        skip_ws_and_comments(stream, i);
        if (i >= stream.size()) {
            break;
        }
        if (stream[i] == ']') {
            ++i;
            break;
        }
        PdfToken tok;
        if (!parse_operand(stream, i, tok)) {
            ++i;
            continue;
        }
        arr.push_back(std::move(tok));
    }
    return arr;
}

static bool parse_operand(const std::vector<uint8_t>& stream, size_t& i, PdfToken& out) {
    skip_ws_and_comments(stream, i);
    if (i >= stream.size()) {
        return false;
    }
    if (stream[i] == '(') {
        out.type = PdfToken::Type::String;
        out.bytes = parse_literal_string(stream, i);
        return true;
    }
    if (stream[i] == '<' && i + 1 < stream.size() && stream[i + 1] == '<') {
        out.type = PdfToken::Type::Dictionary;
        out.dict = parse_dictionary_token(stream, i);
        return !out.dict.empty();
    }
    if (stream[i] == '<' && (i + 1 >= stream.size() || stream[i + 1] != '<')) {
        out.type = PdfToken::Type::String;
        out.bytes = parse_hex_string(stream, i);
        return true;
    }
    if (stream[i] == '/') {
        out.type = PdfToken::Type::Name;
        out.name = parse_name_token(stream, i);
        return true;
    }
    if (stream[i] == '[') {
        out.type = PdfToken::Type::Array;
        out.items = parse_array_token(stream, i);
        return true;
    }
    if (is_number_start(stream[i])) {
        out.type = PdfToken::Type::Number;
        out.number = parse_number_token(stream, i);
        return true;
    }
    return false;
}

static std::string parse_operator(const std::vector<uint8_t>& stream, size_t& i) {
    size_t start = i;
    while (i < stream.size() && !is_white(stream[i]) && !is_delimiter(stream[i])) {
        ++i;
    }
    return std::string(reinterpret_cast<const char*>(stream.data() + start), i - start);
}

static bool is_inline_image_id_token(const std::vector<uint8_t>& stream, size_t i) {
    if (i + 1 >= stream.size()) {
        return false;
    }
    if (stream[i] != 'I' || stream[i + 1] != 'D') {
        return false;
    }

    const bool prev_ok = (i == 0) || is_white(stream[i - 1]) || is_delimiter(stream[i - 1]);
    const bool next_ok = (i + 2 >= stream.size()) || is_white(stream[i + 2]) || is_delimiter(stream[i + 2]);
    return prev_ok && next_ok;
}

static bool is_inline_image_ei_token(const std::vector<uint8_t>& stream, size_t i) {
    if (i + 1 >= stream.size()) {
        return false;
    }
    if (stream[i] != 'E' || stream[i + 1] != 'I') {
        return false;
    }

    const bool prev_ok = (i > 0) && is_white(stream[i - 1]);
    const bool next_ok = (i + 2 >= stream.size()) || is_white(stream[i + 2]) || is_delimiter(stream[i + 2]);
    return prev_ok && next_ok;
}

static void skip_inline_image(const std::vector<uint8_t>& stream, size_t& i) {
    // Skip inline image dictionary until ID.
    while (i < stream.size()) {
        skip_ws_and_comments(stream, i);
        if (i >= stream.size()) {
            return;
        }

        if (is_inline_image_id_token(stream, i)) {
            i += 2;
            if (i < stream.size() && stream[i] == '\r') {
                ++i;
                if (i < stream.size() && stream[i] == '\n') {
                    ++i;
                }
            } else if (i < stream.size() && stream[i] == '\n') {
                ++i;
            } else if (i < stream.size() && is_white(stream[i])) {
                ++i;
            }
            break;
        }

        PdfToken ignored;
        if (parse_operand(stream, i, ignored)) {
            continue;
        }

        std::string ignored_op = parse_operator(stream, i);
        if (ignored_op.empty() && i < stream.size()) {
            ++i;
        }
    }

    // Skip raw inline image data until EI.
    while (i + 1 < stream.size()) {
        if (is_inline_image_ei_token(stream, i)) {
            i += 2;
            return;
        }
        ++i;
    }

    i = stream.size();
}

static bool is_name_terminator_char(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '[' || c == ']' ||
           c == '<' || c == '>' || c == '(' || c == ')';
}

struct PdfDecodeParams {
    int predictor = 1;
    int colors = 1;
    int bits_per_component = 8;
    int columns = 1;
    int early_change = 1;
};

struct PdfFilterSpec {
    std::string name;
    PdfDecodeParams params;
};

static std::string extract_first_dict_fragment(const std::string& text);

static std::string normalize_filter_name(const std::string& name) {
    if (name == "Fl") return "FlateDecode";
    if (name == "AHx") return "ASCIIHexDecode";
    if (name == "A85") return "ASCII85Decode";
    if (name == "LZW") return "LZWDecode";
    if (name == "RL") return "RunLengthDecode";
    if (name == "CCF") return "CCITTFaxDecode";
    if (name == "DCT") return "DCTDecode";
    return name;
}

static int parse_int_in_dict(const std::string& dict, const std::string& key, int fallback) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return fallback;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos >= dict.size()) {
        return fallback;
    }

    char* end_ptr = nullptr;
    long v = wz_strtol(dict.c_str() + pos, &end_ptr, 10);
    if (end_ptr == dict.c_str() + pos) {
        return fallback;
    }
    return static_cast<int>(v);
}

static bool extract_inline_dict_fragment(const std::string& src, size_t start, std::string& out, size_t* end_pos = nullptr) {
    if (start + 1 >= src.size() || src[start] != '<' || src[start + 1] != '<') {
        return false;
    }

    int depth = 0;
    for (size_t i = start; i + 1 < src.size(); ++i) {
        if (src[i] == '<' && src[i + 1] == '<') {
            ++depth;
            ++i;
            continue;
        }
        if (src[i] == '>' && src[i + 1] == '>') {
            --depth;
            if (depth == 0) {
                out = src.substr(start, i - start + 2);
                if (end_pos) {
                    *end_pos = i + 2;
                }
                return true;
            }
            ++i;
        }
    }

    return false;
}

static std::vector<std::string> parse_filter_names(const std::string& dict) {
    std::vector<std::string> filters;
    size_t pos = dict.find("/Filter");
    while (pos != std::string::npos) {
        const size_t after = pos + 7;
        if (after >= dict.size() || is_name_terminator_char(dict[after])) {
            break;
        }
        pos = dict.find("/Filter", pos + 7);
    }
    if (pos == std::string::npos) {
        return filters;
    }

    pos += 7;
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos >= dict.size()) {
        return filters;
    }

    auto read_name = [&](size_t& p) -> std::string {
        if (p >= dict.size() || dict[p] != '/') {
            return {};
        }
        ++p;
        size_t start = p;
        while (p < dict.size() && !is_name_terminator_char(dict[p])) {
            ++p;
        }
        if (p <= start) {
            return {};
        }
        return normalize_filter_name(dict.substr(start, p - start));
    };

    if (dict[pos] == '[') {
        ++pos;
        while (pos < dict.size() && dict[pos] != ']') {
            while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
                ++pos;
            }
            std::string name = read_name(pos);
            if (!name.empty()) {
                filters.push_back(name);
                continue;
            }
            ++pos;
        }
    } else if (dict[pos] == '/') {
        std::string name = read_name(pos);
        if (!name.empty()) {
            filters.push_back(name);
        }
    }

    return filters;
}

static PdfDecodeParams parse_decode_params_dict(const std::string& dict_text) {
    PdfDecodeParams p;
    if (dict_text.empty()) {
        return p;
    }

    p.predictor = parse_int_in_dict(dict_text, "/Predictor", p.predictor);
    p.colors = parse_int_in_dict(dict_text, "/Colors", p.colors);
    p.bits_per_component = parse_int_in_dict(dict_text, "/BitsPerComponent", p.bits_per_component);
    p.bits_per_component = parse_int_in_dict(dict_text, "/BPC", p.bits_per_component);
    p.columns = parse_int_in_dict(dict_text, "/Columns", p.columns);
    p.early_change = parse_int_in_dict(dict_text, "/EarlyChange", p.early_change);

    if (p.colors <= 0) p.colors = 1;
    if (p.bits_per_component <= 0) p.bits_per_component = 8;
    if (p.columns <= 0) p.columns = 1;
    if (p.early_change != 0 && p.early_change != 1) p.early_change = 1;
    if (p.predictor < 1) p.predictor = 1;

    return p;
}

static std::vector<std::string> parse_decode_params_entries(const std::string& dict, WinPdfDocument* resolver = nullptr) {
    std::vector<std::string> entries;
    size_t pos = dict.find("/DecodeParms");
    size_t key_len = 12;
    if (pos == std::string::npos) {
        size_t dp_pos = dict.find("/DP");
        while (dp_pos != std::string::npos) {
            const size_t after = dp_pos + 3;
            if (after >= dict.size() || is_name_terminator_char(dict[after])) {
                pos = dp_pos;
                key_len = 3;
                break;
            }
            dp_pos = dict.find("/DP", dp_pos + 3);
        }
    }
    if (pos == std::string::npos) {
        return entries;
    }

    pos = dict.find_first_not_of(" \t\r\n", pos + key_len);
    if (pos == std::string::npos) {
        return entries;
    }

    auto skip_ws = [&](size_t& p) {
        while (p < dict.size() && std::isspace(static_cast<unsigned char>(dict[p]))) {
            ++p;
        }
    };

    auto read_int_token = [&](size_t& p, int& out) -> bool {
        size_t start = p;
        if (p < dict.size() && (dict[p] == '+' || dict[p] == '-')) {
            ++p;
        }
        size_t digits = p;
        while (p < dict.size() && std::isdigit(static_cast<unsigned char>(dict[p]))) {
            ++p;
        }
        if (digits == p) {
            p = start;
            return false;
        }
        out = std::atoi(dict.substr(start, p - start).c_str());
        return true;
    };

    auto consume_indirect_ref = [&](size_t& p, int& ref_id) -> bool {
        size_t t = p;
        int obj_id = 0;
        int obj_gen = 0;
        if (!read_int_token(t, obj_id)) {
            return false;
        }
        skip_ws(t);
        if (!read_int_token(t, obj_gen)) {
            return false;
        }
        skip_ws(t);
        if (t >= dict.size() || dict[t] != 'R') {
            return false;
        }
        ++t;
        p = t;
        ref_id = obj_id;
        return true;
    };

    auto push_resolved_decode_params = [&](int ref_id) {
        if (!resolver || ref_id <= 0) {
            entries.push_back({});
            return;
        }

        WinPdfObject ref_obj = resolver->read_obj(ref_id);
        std::string ref_dict = !ref_obj.dict.empty() ? ref_obj.dict : extract_first_dict_fragment(ref_obj.body);
        entries.push_back(ref_dict);
    };

    auto consume_simple_token = [&](size_t& p) {
        while (p < dict.size() && !std::isspace(static_cast<unsigned char>(dict[p])) && dict[p] != ']') {
            ++p;
        }
    };

    if (dict[pos] == '<' && pos + 1 < dict.size() && dict[pos + 1] == '<') {
        std::string d;
        if (extract_inline_dict_fragment(dict, pos, d)) {
            entries.push_back(d);
        }
        return entries;
    }

    if (dict[pos] != '[') {
        if (dict.compare(pos, 4, "null") == 0) {
            entries.push_back({});
            return entries;
        }
        int ref_id = 0;
        if (consume_indirect_ref(pos, ref_id)) {
            push_resolved_decode_params(ref_id);
            return entries;
        }
        consume_simple_token(pos);
        entries.push_back({});
        return entries;
    }

    ++pos;
    while (pos < dict.size() && dict[pos] != ']') {
        skip_ws(pos);
        if (pos >= dict.size() || dict[pos] == ']') {
            break;
        }

        if (dict[pos] == '<' && pos + 1 < dict.size() && dict[pos + 1] == '<') {
            std::string d;
            size_t end_pos = pos;
            if (extract_inline_dict_fragment(dict, pos, d, &end_pos)) {
                entries.push_back(d);
                pos = end_pos;
                continue;
            }
            break;
        }

        if (dict.compare(pos, 4, "null") == 0) {
            entries.push_back({});
            pos += 4;
            continue;
        }

        int ref_id = 0;
        if (consume_indirect_ref(pos, ref_id)) {
            push_resolved_decode_params(ref_id);
            continue;
        }

        // Unsupported DecodeParms token form: keep alignment with a default entry.
        entries.push_back({});
        consume_simple_token(pos);
    }

    return entries;
}

static std::vector<PdfFilterSpec> parse_filter_specs(const std::string& dict, WinPdfDocument* resolver = nullptr) {
    std::vector<std::string> names = parse_filter_names(dict);
    std::vector<PdfFilterSpec> specs;
    specs.reserve(names.size());
    for (const std::string& n : names) {
        specs.push_back({n, {}});
    }

    std::vector<std::string> dp_entries = parse_decode_params_entries(dict, resolver);
    if (!dp_entries.empty()) {
        if (dp_entries.size() == 1 && specs.size() >= 1) {
            specs[0].params = parse_decode_params_dict(dp_entries[0]);
        } else {
            const size_t n = std::min(specs.size(), dp_entries.size());
            for (size_t i = 0; i < n; ++i) {
                specs[i].params = parse_decode_params_dict(dp_entries[i]);
            }
        }
    }

    return specs;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static std::vector<uint8_t> decode_ascii_hex_stream(const std::vector<uint8_t>& src) {
    std::vector<uint8_t> out;
    int high = -1;
    for (uint8_t b : src) {
        char c = static_cast<char>(b);
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (c == '>') {
            break;
        }
        int n = hex_nibble(c);
        if (n < 0) {
            continue;
        }
        if (high < 0) {
            high = n;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | n));
            high = -1;
        }
    }
    if (high >= 0) {
        out.push_back(static_cast<uint8_t>(high << 4));
    }
    return out;
}

static std::vector<uint8_t> decode_ascii85_stream(const std::vector<uint8_t>& src) {
    std::vector<uint8_t> out;
    int tuple[5] = {0, 0, 0, 0, 0};
    int tlen = 0;

    size_t i = 0;
    if (src.size() >= 2 && src[0] == '<' && src[1] == '~') {
        i = 2;
    }

    auto flush_tuple = [&](int valid_len) {
        uint32_t v = 0;
        for (int k = 0; k < 5; ++k) {
            v = v * 85u + static_cast<uint32_t>(tuple[k]);
        }
        uint8_t bytes[4] = {
            static_cast<uint8_t>((v >> 24) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>(v & 0xFF)
        };
        int out_n = (valid_len == 5) ? 4 : (valid_len - 1);
        for (int k = 0; k < out_n; ++k) {
            out.push_back(bytes[k]);
        }
    };

    for (; i < src.size(); ++i) {
        char c = static_cast<char>(src[i]);
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (c == '~') {
            break;
        }
        if (c == 'z') {
            if (tlen != 0) {
                return {};
            }
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
            continue;
        }
        if (c < '!' || c > 'u') {
            continue;
        }

        tuple[tlen++] = c - '!';
        if (tlen == 5) {
            flush_tuple(5);
            tlen = 0;
        }
    }

    if (tlen == 1) {
        return {};
    }
    if (tlen > 1) {
        for (int k = tlen; k < 5; ++k) {
            tuple[k] = 'u' - '!';
        }
        flush_tuple(tlen);
    }

    return out;
}

static std::vector<uint8_t> decode_run_length_stream(const std::vector<uint8_t>& src) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < src.size()) {
        uint8_t b = src[i++];
        if (b == 128) {
            break;
        }
        if (b <= 127) {
            size_t n = static_cast<size_t>(b) + 1;
            if (i + n > src.size()) {
                return {};
            }
            out.insert(out.end(), src.begin() + i, src.begin() + i + n);
            i += n;
        } else {
            size_t n = static_cast<size_t>(257 - b);
            if (i >= src.size()) {
                return {};
            }
            out.insert(out.end(), n, src[i]);
            ++i;
        }
    }
    return out;
}

class LzwBitReader {
public:
    explicit LzwBitReader(const std::vector<uint8_t>& in) : data(in) {}

    bool read_bits(int n, int& out) {
        if (n <= 0) {
            out = 0;
            return true;
        }

        out = 0;
        for (int i = 0; i < n; ++i) {
            if (byte_pos >= data.size()) {
                return false;
            }
            int bit = (data[byte_pos] >> (7 - bit_pos)) & 1;
            out = (out << 1) | bit;

            ++bit_pos;
            if (bit_pos == 8) {
                bit_pos = 0;
                ++byte_pos;
            }
        }
        return true;
    }

private:
    const std::vector<uint8_t>& data;
    size_t byte_pos = 0;
    int bit_pos = 0;
};

static std::vector<uint8_t> decode_lzw_stream(const std::vector<uint8_t>& src, int early_change) {
    std::vector<std::vector<uint8_t>> table(4096);
    auto reset_table = [&]() {
        for (auto& e : table) {
            e.clear();
        }
        for (int i = 0; i < 256; ++i) {
            table[i] = {static_cast<uint8_t>(i)};
        }
    };

    reset_table();
    LzwBitReader br(src);
    std::vector<uint8_t> out;

    const int clear = 256;
    const int eod = 257;
    int next_code = 258;
    int code_size = 9;
    int prev_code = -1;

    while (true) {
        int code = 0;
        if (!br.read_bits(code_size, code)) {
            break;
        }

        if (code == clear) {
            reset_table();
            next_code = 258;
            code_size = 9;
            prev_code = -1;
            continue;
        }
        if (code == eod) {
            break;
        }

        std::vector<uint8_t> entry;
        if (code >= 0 && code < next_code && !table[code].empty()) {
            entry = table[code];
        } else if (code == next_code && prev_code >= 0 && !table[prev_code].empty()) {
            entry = table[prev_code];
            entry.push_back(entry.front());
        } else {
            return {};
        }

        out.insert(out.end(), entry.begin(), entry.end());

        if (prev_code >= 0 && next_code < 4096 && !table[prev_code].empty()) {
            std::vector<uint8_t> new_entry = table[prev_code];
            new_entry.push_back(entry.front());
            table[next_code++] = std::move(new_entry);

            const int bump = (1 << code_size) - early_change;
            if (code_size < 12 && next_code == bump) {
                ++code_size;
            }
        }

        prev_code = code;
    }

    return out;
}

static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static std::vector<uint8_t> apply_predictor(const std::vector<uint8_t>& src, const PdfDecodeParams& p) {
    if (p.predictor <= 1) {
        return src;
    }

    const int row_bytes = (p.colors * p.bits_per_component * p.columns + 7) / 8;
    const int bpp = std::max(1, (p.colors * p.bits_per_component + 7) / 8);
    if (row_bytes <= 0) {
        return {};
    }

    if (p.predictor == 2) {
        if (src.size() % static_cast<size_t>(row_bytes) != 0) {
            return {};
        }
        std::vector<uint8_t> out = src;
        const size_t rows = src.size() / static_cast<size_t>(row_bytes);
        for (size_t r = 0; r < rows; ++r) {
            const size_t row_off = r * static_cast<size_t>(row_bytes);
            for (int i = bpp; i < row_bytes; ++i) {
                const size_t idx = row_off + static_cast<size_t>(i);
                out[idx] = static_cast<uint8_t>(out[idx] + out[idx - static_cast<size_t>(bpp)]);
            }
        }
        return out;
    }

    if (p.predictor >= 10 && p.predictor <= 15) {
        const bool row_has_tag = true;
        const size_t row_unit = static_cast<size_t>(row_bytes + (row_has_tag ? 1 : 0));
        if (row_unit == 0 || src.size() % row_unit != 0) {
            return {};
        }

        const size_t rows = src.size() / row_unit;
        std::vector<uint8_t> out(rows * static_cast<size_t>(row_bytes));
        std::vector<uint8_t> prev(static_cast<size_t>(row_bytes), 0);

        size_t in_pos = 0;
        for (size_t r = 0; r < rows; ++r) {
            int filter = p.predictor - 10;
            if (row_has_tag) {
                filter = src[in_pos++];
            }

            uint8_t* row_out = out.data() + r * static_cast<size_t>(row_bytes);
            const uint8_t* row_in = src.data() + in_pos;
            in_pos += static_cast<size_t>(row_bytes);

            switch (filter) {
            case 0:
                std::memcpy(row_out, row_in, static_cast<size_t>(row_bytes));
                break;
            case 1:
                for (int i = 0; i < row_bytes; ++i) {
                    uint8_t left = (i >= bpp) ? row_out[i - bpp] : 0;
                    row_out[i] = static_cast<uint8_t>(row_in[i] + left);
                }
                break;
            case 2:
                for (int i = 0; i < row_bytes; ++i) {
                    row_out[i] = static_cast<uint8_t>(row_in[i] + prev[static_cast<size_t>(i)]);
                }
                break;
            case 3:
                for (int i = 0; i < row_bytes; ++i) {
                    uint8_t left = (i >= bpp) ? row_out[i - bpp] : 0;
                    uint8_t up = prev[static_cast<size_t>(i)];
                    row_out[i] = static_cast<uint8_t>(row_in[i] + static_cast<uint8_t>((static_cast<int>(left) + static_cast<int>(up)) / 2));
                }
                break;
            case 4:
                for (int i = 0; i < row_bytes; ++i) {
                    uint8_t left = (i >= bpp) ? row_out[i - bpp] : 0;
                    uint8_t up = prev[static_cast<size_t>(i)];
                    uint8_t up_left = (i >= bpp) ? prev[static_cast<size_t>(i - bpp)] : 0;
                    row_out[i] = static_cast<uint8_t>(row_in[i] + paeth_predictor(left, up, up_left));
                }
                break;
            default:
                return {};
            }

            std::memcpy(prev.data(), row_out, static_cast<size_t>(row_bytes));
        }

        return out;
    }

    return src;
}

static std::vector<uint8_t> decode_stream_data(const std::vector<uint8_t>& src, const std::string& dict, WinPdfDocument* resolver = nullptr) {
    std::vector<PdfFilterSpec> specs = parse_filter_specs(dict, resolver);
    if (specs.empty()) {
        return src;
    }

    std::vector<uint8_t> data = src;
    for (const PdfFilterSpec& spec : specs) {
        const std::string& f = spec.name;
        if (f == "FlateDecode") {
            data = WinInflate::decompress(data);
        } else if (f == "ASCIIHexDecode") {
            data = decode_ascii_hex_stream(data);
        } else if (f == "ASCII85Decode") {
            data = decode_ascii85_stream(data);
        } else if (f == "RunLengthDecode") {
            data = decode_run_length_stream(data);
        } else if (f == "LZWDecode") {
            data = decode_lzw_stream(data, spec.params.early_change);
        } else {
            return {};
        }

        if ((f == "FlateDecode" || f == "LZWDecode") && spec.params.predictor > 1) {
            data = apply_predictor(data, spec.params);
        }

        if (data.empty() && !src.empty()) {
            return {};
        }
    }

    return data;
}

static void copy_matrix(float dst[6], const float src[6]) {
    for (int k = 0; k < 6; ++k) {
        dst[k] = src[k];
    }
}

static void set_identity(float m[6]) {
    m[0] = 1.0f; m[1] = 0.0f;
    m[2] = 0.0f; m[3] = 1.0f;
    m[4] = 0.0f; m[5] = 0.0f;
}

static void move_text_position(TextState& st, float tx, float ty) {
    // PDF spec: new_tlm = translate(tx, ty) × old_tlm
    // = [tlm[0], tlm[1], tlm[2], tlm[3], tx*tlm[0]+ty*tlm[2]+tlm[4], tx*tlm[1]+ty*tlm[3]+tlm[5]]
    st.tlm[4] += tx * st.tlm[0] + ty * st.tlm[2];
    st.tlm[5] += tx * st.tlm[1] + ty * st.tlm[3];
    copy_matrix(st.tm, st.tlm);
}

static void matrix_multiply(const float a[6], const float b[6], float out[6]) {
    // PDF matrix multiply for affine matrices represented as [a b c d e f].
    out[0] = a[0] * b[0] + a[1] * b[2];
    out[1] = a[0] * b[1] + a[1] * b[3];
    out[2] = a[2] * b[0] + a[3] * b[2];
    out[3] = a[2] * b[1] + a[3] * b[3];
    out[4] = a[4] * b[0] + a[5] * b[2] + b[4];
    out[5] = a[4] * b[1] + a[5] * b[3] + b[5];
}

static Vec2 apply_matrix_to_point(const float m[6], float x, float y) {
    return {x * m[0] + y * m[2] + m[4], x * m[1] + y * m[3] + m[5]};
}

static std::string trim_copy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

static int parse_int_after_key(const std::string& dict, const std::string& key, int fallback = -1) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return fallback;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos >= dict.size()) {
        return fallback;
    }

    char* end_ptr = nullptr;
    long v = wz_strtol(dict.c_str() + pos, &end_ptr, 10);
    if (end_ptr == dict.c_str() + pos) {
        return fallback;
    }
    return static_cast<int>(v);
}

static std::vector<int> parse_int_array_after_key(const std::string& dict, const std::string& key) {
    std::vector<int> out;
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return out;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos >= dict.size() || dict[pos] != '[') {
        return out;
    }

    ++pos;
    while (pos < dict.size() && dict[pos] != ']') {
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size() || dict[pos] == ']') {
            break;
        }

        char* end_ptr = nullptr;
        long v = wz_strtol(dict.c_str() + pos, &end_ptr, 10);
        if (end_ptr != dict.c_str() + pos) {
            out.push_back(static_cast<int>(v));
            pos = static_cast<size_t>(end_ptr - dict.c_str());
            continue;
        }

        ++pos;
    }

    return out;
}

static int parse_first_int(const std::string& text, int fallback = -1) {
    const char* ptr = text.c_str();
    while (*ptr && std::isspace(static_cast<unsigned char>(*ptr))) {
        ++ptr;
    }
    if (!*ptr) {
        return fallback;
    }

    char* end_ptr = nullptr;
    long v = wz_strtol(ptr, &end_ptr, 10);
    if (end_ptr == ptr) {
        return fallback;
    }
    return static_cast<int>(v);
}

static std::string extract_first_dict_fragment(const std::string& text) {
    size_t dict_start = text.find("<<");
    if (dict_start == std::string::npos) {
        return {};
    }

    int depth = 0;
    for (size_t i = dict_start; i + 1 < text.size(); ++i) {
        if (text[i] == '<' && text[i + 1] == '<') {
            ++depth;
            ++i;
            continue;
        }
        if (text[i] == '>' && text[i + 1] == '>') {
            --depth;
            if (depth == 0) {
                return text.substr(dict_start, i - dict_start + 2);
            }
            ++i;
        }
    }

    return {};
}

static bool object_has_payload(const WinPdfObject& obj) {
    return obj.is_stream || !obj.dict.empty() || !trim_copy(obj.body).empty();
}

static bool extract_inline_dict_after_key(const std::string& dict, const std::string& key, std::string& out) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return false;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos + 1 >= dict.size() || dict[pos] != '<' || dict[pos + 1] != '<') {
        return false;
    }

    const size_t start = pos;
    int depth = 0;
    for (size_t i = pos; i + 1 < dict.size(); ++i) {
        if (dict[i] == '<' && dict[i + 1] == '<') {
            ++depth;
            ++i;
            continue;
        }
        if (dict[i] == '>' && dict[i + 1] == '>') {
            --depth;
            if (depth == 0) {
                out = dict.substr(start, i - start + 2);
                return true;
            }
            ++i;
        }
    }

    return false;
}

static std::map<std::string, int> parse_font_refs_from_dict(const std::string& font_dict) {
    std::map<std::string, int> refs;
    size_t pos = 0;
    while (pos < font_dict.size()) {
        if (font_dict[pos] != '/') {
            ++pos;
            continue;
        }

        ++pos;
        size_t name_start = pos;
        while (pos < font_dict.size() && !std::isspace(static_cast<unsigned char>(font_dict[pos])) &&
               font_dict[pos] != '/' && font_dict[pos] != '<' && font_dict[pos] != '>' &&
               font_dict[pos] != '[' && font_dict[pos] != ']' && font_dict[pos] != '(' && font_dict[pos] != ')') {
            ++pos;
        }
        std::string name = font_dict.substr(name_start, pos - name_start);
        if (name.empty()) {
            continue;
        }

        while (pos < font_dict.size() && std::isspace(static_cast<unsigned char>(font_dict[pos]))) {
            ++pos;
        }

        int id = 0;
        int gen = 0;
        if (pos < font_dict.size() && wz_parse_obj_ref_fitz(font_dict.c_str() + pos, id, gen) && id > 0) {
            refs[name] = id;
        }
    }

    return refs;
}

static std::vector<std::string> extract_hex_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < line.size()) {
        pos = line.find('<', pos);
        if (pos == std::string::npos) {
            break;
        }
        size_t end = line.find('>', pos + 1);
        if (end == std::string::npos) {
            break;
        }
        tokens.push_back(line.substr(pos, end - pos + 1));
        pos = end + 1;
    }
    return tokens;
}

static bool parse_hex_token_to_bytes(const std::string& token, std::vector<uint8_t>& out) {
    out.clear();
    if (token.size() < 2 || token.front() != '<' || token.back() != '>') {
        return false;
    }
    if (token == "<<" || token == ">>") {
        return false;
    }

    std::string hex;
    hex.reserve(token.size());
    for (size_t i = 1; i + 1 < token.size(); ++i) {
        char c = token[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        hex.push_back(c);
    }

    if (hex.empty()) {
        return false;
    }

    if ((hex.size() % 2) != 0) {
        hex.push_back('0');
    }

    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(hex_to_byte(hex[i], hex[i + 1]));
    }
    return !out.empty();
}

static bool parse_hex_token_to_int(const std::string& token, int& out) {
    std::vector<uint8_t> bytes;
    if (!parse_hex_token_to_bytes(token, bytes)) {
        return false;
    }

    uint64_t v = 0;
    for (uint8_t b : bytes) {
        v = (v << 8) | static_cast<uint64_t>(b);
        if (v > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
    }
    out = static_cast<int>(v);
    return true;
}

static bool parse_hex_token_to_unicode_sequence(const std::string& token, std::vector<int>& out) {
    out.clear();
    std::vector<uint8_t> bytes;
    if (!parse_hex_token_to_bytes(token, bytes)) {
        return false;
    }

    if (bytes.empty()) {
        return false;
    }

    if (bytes.size() == 1) {
        out.push_back(bytes[0]);
        return true;
    }

    if ((bytes.size() % 2) != 0) {
        bytes.insert(bytes.begin(), 0);
    }

    for (size_t i = 0; i + 1 < bytes.size();) {
        const uint16_t u = static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
        i += 2;

        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= bytes.size()) {
                return false;
            }
            const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
            if (v < 0xDC00 || v > 0xDFFF) {
                return false;
            }
            cp = 0x10000u + (((static_cast<uint32_t>(u) - 0xD800u) << 10) | (static_cast<uint32_t>(v) - 0xDC00u));
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;
        }

        if (cp == 0 || cp > 0x10FFFFu) {
            return false;
        }

        out.push_back(static_cast<int>(cp));
    }

    return !out.empty();
}

static bool parse_hex_token_to_unicode_scalar(const std::string& token, int& out) {
    std::vector<int> seq;
    if (!parse_hex_token_to_unicode_sequence(token, seq) || seq.empty()) {
        return false;
    }
    out = seq.front();
    return true;
}

static bool is_cmap_hex_token(const std::string& tok) {
    return tok.size() >= 2 && tok.front() == '<' && tok.back() == '>' && tok != "<<" && tok != ">>";
}

static bool parse_int_token_strict(const std::string& tok, int& out) {
    if (tok.empty()) {
        return false;
    }
    char* end_ptr = nullptr;
    long v = wz_strtol(tok.c_str(), &end_ptr, 10);
    if (end_ptr == tok.c_str() || *end_ptr != '\0') {
        return false;
    }
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

static std::vector<std::string> tokenize_cmap_stream(const std::string& text) {
    std::vector<std::string> toks;
    size_t pos = 0;
    while (pos < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        if (std::isspace(c)) {
            ++pos;
            continue;
        }
        if (c == '%') {
            while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') {
                ++pos;
            }
            continue;
        }
        if (c == '[' || c == ']') {
            toks.push_back(text.substr(pos, 1));
            ++pos;
            continue;
        }
        if (c == '<') {
            if (pos + 1 < text.size() && text[pos + 1] == '<') {
                toks.push_back("<<");
                pos += 2;
                continue;
            }
            size_t end = text.find('>', pos + 1);
            if (end == std::string::npos) {
                break;
            }
            toks.push_back(text.substr(pos, end - pos + 1));
            pos = end + 1;
            continue;
        }
        if (c == '>') {
            if (pos + 1 < text.size() && text[pos + 1] == '>') {
                toks.push_back(">>");
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }

        size_t start = pos;
        while (pos < text.size()) {
            unsigned char ch = static_cast<unsigned char>(text[pos]);
            if (std::isspace(ch) || ch == '%' || ch == '[' || ch == ']' || ch == '<' || ch == '>') {
                break;
            }
            ++pos;
        }
        if (pos > start) {
            toks.push_back(text.substr(start, pos - start));
        }
    }

    return toks;
}

static std::unordered_map<int, std::vector<int>> parse_tounicode_cmap(const std::vector<uint8_t>& bytes) {
    std::unordered_map<int, std::vector<int>> mapping;
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::vector<std::string> toks = tokenize_cmap_stream(text);

    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] == "beginbfchar") {
            int count = 0;
            if (i == 0 || !parse_int_token_strict(toks[i - 1], count) || count <= 0) {
                continue;
            }

            ++i;
            int remaining = count;
            while (i < toks.size() && remaining > 0) {
                while (i < toks.size() && !is_cmap_hex_token(toks[i])) {
                    ++i;
                }
                if (i + 1 >= toks.size()) {
                    break;
                }

                int src = 0;
                std::vector<int> dst;
                if (parse_hex_token_to_int(toks[i], src) && parse_hex_token_to_unicode_sequence(toks[i + 1], dst) && src >= 0) {
                    mapping[src] = std::move(dst);
                }
                i += 2;
                --remaining;
            }
            if (i > 0) {
                --i;
            }
            continue;
        }

        if (toks[i] == "beginbfrange") {
            int count = 0;
            if (i == 0 || !parse_int_token_strict(toks[i - 1], count) || count <= 0) {
                continue;
            }

            ++i;
            int remaining = count;
            while (i < toks.size() && remaining > 0) {
                while (i < toks.size() && !is_cmap_hex_token(toks[i])) {
                    ++i;
                }
                if (i + 1 >= toks.size()) {
                    break;
                }

                int src_start = 0;
                int src_end = 0;
                if (!parse_hex_token_to_int(toks[i], src_start) || !parse_hex_token_to_int(toks[i + 1], src_end) || src_start > src_end) {
                    ++i;
                    --remaining;
                    continue;
                }
                i += 2;

                if (i < toks.size() && toks[i] == "[") {
                    ++i;
                    int code = src_start;
                    while (i < toks.size() && toks[i] != "]") {
                        if (is_cmap_hex_token(toks[i])) {
                            std::vector<int> dst;
                            if (code <= src_end && parse_hex_token_to_unicode_sequence(toks[i], dst)) {
                                mapping[code] = std::move(dst);
                            }
                            ++code;
                        }
                        ++i;
                    }
                    if (i < toks.size() && toks[i] == "]") {
                        ++i;
                    }
                } else if (i < toks.size() && is_cmap_hex_token(toks[i])) {
                    std::vector<int> dst_start;
                    if (parse_hex_token_to_unicode_sequence(toks[i], dst_start) && !dst_start.empty()) {
                        if (dst_start.size() == 1) {
                            for (int code = src_start; code <= src_end; ++code) {
                                const int cp = dst_start.front() + (code - src_start);
                                if (cp > 0 && cp <= 0x10FFFF) {
                                    mapping[code] = {cp};
                                }
                            }
                        } else {
                            for (int code = src_start; code <= src_end; ++code) {
                                mapping[code] = dst_start;
                            }
                        }
                    }
                    ++i;
                }

                --remaining;
            }

            if (i > 0) {
                --i;
            }
        }
    }

    return mapping;
}

static std::vector<WinCodeSpaceRange> parse_cmap_codespace_ranges(const std::vector<uint8_t>& bytes) {
    std::vector<WinCodeSpaceRange> ranges;
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::vector<std::string> toks = tokenize_cmap_stream(text);

    auto bytes_to_u32 = [](const std::vector<uint8_t>& b, uint32_t& out) -> bool {
        if (b.empty() || b.size() > 4) {
            return false;
        }
        uint32_t v = 0;
        for (uint8_t x : b) {
            v = (v << 8) | static_cast<uint32_t>(x);
        }
        out = v;
        return true;
    };

    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] != "begincodespacerange") {
            continue;
        }

        int count = 0;
        if (i == 0 || !parse_int_token_strict(toks[i - 1], count) || count <= 0) {
            continue;
        }

        ++i;
        int remaining = count;
        while (i < toks.size() && remaining > 0) {
            while (i < toks.size() && !is_cmap_hex_token(toks[i])) {
                ++i;
            }
            if (i + 1 >= toks.size()) {
                break;
            }

            std::vector<uint8_t> low_bytes;
            std::vector<uint8_t> high_bytes;
            if (parse_hex_token_to_bytes(toks[i], low_bytes) && parse_hex_token_to_bytes(toks[i + 1], high_bytes) &&
                !low_bytes.empty() && low_bytes.size() == high_bytes.size()) {
                uint32_t low = 0;
                uint32_t high = 0;
                if (bytes_to_u32(low_bytes, low) && bytes_to_u32(high_bytes, high)) {
                    WinCodeSpaceRange r;
                    r.nbytes = static_cast<int>(low_bytes.size());
                    r.low = low;
                    r.high = high;
                    ranges.push_back(r);
                }
            }

            i += 2;
            --remaining;
        }

        if (i > 0) {
            --i;
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const WinCodeSpaceRange& a, const WinCodeSpaceRange& b) {
        if (a.nbytes != b.nbytes) {
            return a.nbytes > b.nbytes;
        }
        if (a.low != b.low) {
            return a.low < b.low;
        }
        return a.high < b.high;
    });

    return ranges;
}

static std::string parse_name_value_after_key(const std::string& dict, const std::string& key) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return {};
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos >= dict.size() || dict[pos] != '/') {
        return {};
    }

    ++pos;
    size_t start = pos;
    while (pos < dict.size() && !std::isspace(static_cast<unsigned char>(dict[pos])) &&
           dict[pos] != '/' && dict[pos] != '<' && dict[pos] != '>' &&
           dict[pos] != '[' && dict[pos] != ']' && dict[pos] != '(' && dict[pos] != ')') {
        ++pos;
    }
    return dict.substr(start, pos - start);
}

static bool is_pdf_dict_delimiter(char c) {
    switch (c) {
        case '(': case ')':
        case '<': case '>':
        case '[': case ']':
        case '{': case '}':
        case '/': case '%':
            return true;
        default:
            return false;
    }
}

static size_t skip_pdf_ws_and_comments_text(const std::string& text, size_t pos, size_t limit) {
    while (pos < limit) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        if (std::isspace(c)) {
            ++pos;
            continue;
        }
        if (text[pos] == '%') {
            while (pos < limit && text[pos] != '\n' && text[pos] != '\r') {
                ++pos;
            }
            continue;
        }
        break;
    }
    return pos;
}

static bool parse_pdf_name_token_text(const std::string& text, size_t& pos, size_t limit, std::string& out_name) {
    pos = skip_pdf_ws_and_comments_text(text, pos, limit);
    if (pos >= limit || text[pos] != '/') {
        return false;
    }

    ++pos;
    const size_t start = pos;
    while (pos < limit && !std::isspace(static_cast<unsigned char>(text[pos])) && !is_pdf_dict_delimiter(text[pos])) {
        ++pos;
    }

    out_name = text.substr(start, pos - start);
    return !out_name.empty();
}

static bool parse_pdf_simple_token_text(const std::string& text, size_t& pos, size_t limit, std::string& out_tok) {
    pos = skip_pdf_ws_and_comments_text(text, pos, limit);
    if (pos >= limit) {
        return false;
    }

    if (is_pdf_dict_delimiter(text[pos])) {
        return false;
    }

    const size_t start = pos;
    while (pos < limit && !std::isspace(static_cast<unsigned char>(text[pos])) && !is_pdf_dict_delimiter(text[pos])) {
        ++pos;
    }

    out_tok = text.substr(start, pos - start);
    return !out_tok.empty();
}

static bool is_strict_integer_token(const std::string& tok) {
    if (tok.empty()) {
        return false;
    }

    size_t i = 0;
    if (tok[i] == '+' || tok[i] == '-') {
        ++i;
    }
    if (i >= tok.size()) {
        return false;
    }

    for (; i < tok.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tok[i]))) {
            return false;
        }
    }
    return true;
}

static bool read_pdf_object_span_text(const std::string& text, size_t& pos, size_t limit) {
    pos = skip_pdf_ws_and_comments_text(text, pos, limit);
    if (pos >= limit) {
        return false;
    }

    const char c = text[pos];
    if (c == '(') {
        int depth = 0;
        while (pos < limit) {
            const char ch = text[pos++];
            if (ch == '\\') {
                if (pos < limit) {
                    ++pos;
                }
                continue;
            }
            if (ch == '(') {
                ++depth;
            } else if (ch == ')') {
                --depth;
                if (depth <= 0) {
                    return true;
                }
            }
        }
        return false;
    }

    if (c == '<') {
        if (pos + 1 < limit && text[pos + 1] == '<') {
            pos += 2;
            while (pos < limit) {
                pos = skip_pdf_ws_and_comments_text(text, pos, limit);
                if (pos + 1 < limit && text[pos] == '>' && text[pos + 1] == '>') {
                    pos += 2;
                    return true;
                }
                if (!read_pdf_object_span_text(text, pos, limit)) {
                    return false;
                }
            }
            return false;
        }

        ++pos;
        while (pos < limit && text[pos] != '>') {
            ++pos;
        }
        if (pos < limit && text[pos] == '>') {
            ++pos;
            return true;
        }
        return false;
    }

    if (c == '[') {
        ++pos;
        while (pos < limit) {
            pos = skip_pdf_ws_and_comments_text(text, pos, limit);
            if (pos < limit && text[pos] == ']') {
                ++pos;
                return true;
            }
            if (!read_pdf_object_span_text(text, pos, limit)) {
                return false;
            }
        }
        return false;
    }

    if (c == '/') {
        std::string name;
        return parse_pdf_name_token_text(text, pos, limit, name);
    }

    size_t token_end = pos;
    std::string tok1;
    if (!parse_pdf_simple_token_text(text, token_end, limit, tok1)) {
        return false;
    }
    pos = token_end;

    if (is_strict_integer_token(tok1)) {
        size_t probe = token_end;
        std::string tok2;
        if (parse_pdf_simple_token_text(text, probe, limit, tok2) && is_strict_integer_token(tok2)) {
            std::string tok3;
            if (parse_pdf_simple_token_text(text, probe, limit, tok3) && tok3 == "R") {
                pos = probe;
            }
        }
    }

    return true;
}

static std::map<std::string, std::string> parse_pdf_dict_entries(const std::string& dict_text) {
    std::map<std::string, std::string> out;
    const size_t start = dict_text.find("<<");
    if (start == std::string::npos) {
        return out;
    }

    size_t pos = start + 2;
    const size_t limit = dict_text.size();
    while (pos < limit) {
        pos = skip_pdf_ws_and_comments_text(dict_text, pos, limit);
        if (pos + 1 < limit && dict_text[pos] == '>' && dict_text[pos + 1] == '>') {
            break;
        }

        std::string key;
        if (!parse_pdf_name_token_text(dict_text, pos, limit, key)) {
            if (!read_pdf_object_span_text(dict_text, pos, limit)) {
                break;
            }
            continue;
        }

        pos = skip_pdf_ws_and_comments_text(dict_text, pos, limit);
        if (pos + 1 < limit && dict_text[pos] == '>' && dict_text[pos + 1] == '>') {
            out[key] = "";
            break;
        }

        const size_t value_start = pos;
        if (!read_pdf_object_span_text(dict_text, pos, limit)) {
            break;
        }

        out[key] = trim_copy(dict_text.substr(value_start, pos - value_start));
    }

    return out;
}

static std::vector<std::string> split_pdf_array_items(const std::string& array_text) {
    std::vector<std::string> out;
    size_t pos = skip_pdf_ws_and_comments_text(array_text, 0, array_text.size());
    if (pos >= array_text.size() || array_text[pos] != '[') {
        return out;
    }
    ++pos;

    const size_t limit = array_text.size();
    while (pos < limit) {
        pos = skip_pdf_ws_and_comments_text(array_text, pos, limit);
        if (pos < limit && array_text[pos] == ']') {
            break;
        }

        const size_t item_start = pos;
        if (!read_pdf_object_span_text(array_text, pos, limit)) {
            break;
        }
        out.push_back(trim_copy(array_text.substr(item_start, pos - item_start)));
    }

    return out;
}

static std::string parse_name_from_expr(const std::string& expr) {
    std::string name;
    size_t pos = 0;
    if (parse_pdf_name_token_text(expr, pos, expr.size(), name)) {
        return name;
    }
    return {};
}

static bool parse_ref_id_from_expr(const std::string& expr, int& out_ref_id) {
    const std::string t = trim_copy(expr);
    if (t.empty()) {
        return false;
    }

    int gen = 0;
    if (wz_parse_obj_ref_fitz(t.c_str(), out_ref_id, gen) && out_ref_id > 0) {
        return true;
    }
    return false;
}

static int default_component_count_for_colorspace(WinColorSpaceKind kind) {
    switch (kind) {
        case WinColorSpaceKind::DeviceGray:
            return 1;
        case WinColorSpaceKind::DeviceRGB:
            return 3;
        case WinColorSpaceKind::DeviceCMYK:
            return 4;
        case WinColorSpaceKind::Lab:
            return 3;
        default:
            return 0;
    }
}

static WinColorSpaceDef make_colorspace_def(WinColorSpaceKind kind, int component_count = -1) {
    WinColorSpaceDef out;
    out.kind = kind;
    out.alt_kind = WinColorSpaceKind::Unknown;
    out.alt_component_count = 0;
    out.component_count = (component_count >= 0) ? component_count : default_component_count_for_colorspace(kind);
    return out;
}

static WinColorSpaceDef colorspace_from_name(const std::string& name) {
    if (name == "DeviceGray" || name == "G" || name == "CalGray") {
        return make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
    }
    if (name == "DeviceRGB" || name == "RGB" || name == "CalRGB") {
        return make_colorspace_def(WinColorSpaceKind::DeviceRGB, 3);
    }
    if (name == "Lab") {
        return make_colorspace_def(WinColorSpaceKind::Lab, 3);
    }
    if (name == "DeviceCMYK" || name == "CMYK") {
        return make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
    }
    if (name == "Pattern") {
        return make_colorspace_def(WinColorSpaceKind::Pattern, 0);
    }
    return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
}

static bool parse_float_token_strict_fitz(const std::string& token, float& out_value) {
    const std::string t = trim_copy(token);
    if (t.empty()) {
        return false;
    }

    char* end_ptr = nullptr;
    const double v = std::strtod(t.c_str(), &end_ptr);
    if (end_ptr == t.c_str()) {
        return false;
    }
    while (*end_ptr && std::isspace(static_cast<unsigned char>(*end_ptr))) {
        ++end_ptr;
    }
    if (*end_ptr != '\0') {
        return false;
    }

    out_value = static_cast<float>(v);
    return true;
}

static std::vector<float> parse_float_array_expr_fitz(const std::string& expr) {
    std::vector<float> out;
    const std::string t = trim_copy(expr);
    if (t.empty() || t[0] != '[') {
        return out;
    }

    const std::vector<std::string> items = split_pdf_array_items(t);
    out.reserve(items.size());
    for (const std::string& item : items) {
        float v = 0.0f;
        if (parse_float_token_strict_fitz(item, v)) {
            out.push_back(v);
        }
    }
    return out;
}

static std::vector<int> parse_int_array_expr_fitz(const std::string& expr) {
    std::vector<int> out;
    const std::string t = trim_copy(expr);
    if (t.empty() || t[0] != '[') {
        return out;
    }

    const std::vector<std::string> items = split_pdf_array_items(t);
    out.reserve(items.size());
    for (const std::string& item : items) {
        int v = 0;
        if (parse_int_token_strict(item, v)) {
            out.push_back(v);
        }
    }
    return out;
}

static bool parse_lab_whitepoint_from_expr_fitz(const std::string& dict_expr,
                                                float& out_x,
                                                float& out_y,
                                                float& out_z) {
    if (dict_expr.empty() || dict_expr.rfind("<<", 0) != 0) {
        return false;
    }

    const std::map<std::string, std::string> entries = parse_pdf_dict_entries(dict_expr);
    auto it = entries.find("WhitePoint");
    if (it == entries.end()) {
        return false;
    }

    const std::vector<float> wp = parse_float_array_expr_fitz(it->second);
    if (wp.size() < 3) {
        return false;
    }

    out_x = wp[0];
    out_y = wp[1];
    out_z = wp[2];
    return true;
}

static int parse_int_from_dict_entries_fitz(const std::map<std::string, std::string>& entries,
                                            const std::string& key,
                                            int fallback) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return fallback;
    }
    int out = 0;
    if (parse_int_token_strict(it->second, out)) {
        return out;
    }
    return fallback;
}

static float parse_float_from_dict_entries_fitz(const std::map<std::string, std::string>& entries,
                                                const std::string& key,
                                                float fallback) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return fallback;
    }
    float out = 0.0f;
    if (parse_float_token_strict_fitz(it->second, out)) {
        return out;
    }
    return fallback;
}

static std::vector<float> parse_float_array_from_dict_entries_fitz(const std::map<std::string, std::string>& entries,
                                                                   const std::string& key) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return {};
    }
    return parse_float_array_expr_fitz(it->second);
}

static std::vector<int> parse_int_array_from_dict_entries_fitz(const std::map<std::string, std::string>& entries,
                                                               const std::string& key) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return {};
    }
    return parse_int_array_expr_fitz(it->second);
}

static int infer_component_count_from_kind_fitz(WinColorSpaceKind kind) {
    switch (kind) {
        case WinColorSpaceKind::DeviceGray:
            return 1;
        case WinColorSpaceKind::DeviceRGB:
            return 3;
        case WinColorSpaceKind::DeviceCMYK:
            return 4;
        case WinColorSpaceKind::Lab:
            return 3;
        default:
            return 0;
    }
}

static bool parse_tint_function_from_dict_fitz(const std::string& function_dict,
                                               const std::vector<uint8_t>& decoded_stream,
                                               int expected_input_count,
                                               int expected_output_count,
                                               WinColorSpaceDef& out_space) {
    if (function_dict.empty()) {
        return false;
    }

    const std::map<std::string, std::string> entries = parse_pdf_dict_entries(function_dict);
    if (entries.empty()) {
        return false;
    }

    const int function_type = parse_int_from_dict_entries_fitz(entries, "FunctionType", -1);
    if (function_type == 2) {
        std::vector<float> c0 = parse_float_array_from_dict_entries_fitz(entries, "C0");
        std::vector<float> c1 = parse_float_array_from_dict_entries_fitz(entries, "C1");

        int out_count = expected_output_count;
        if (out_count <= 0) {
            out_count = static_cast<int>(std::max(c0.size(), c1.size()));
        }
        if (out_count <= 0) {
            out_count = 1;
        }

        if (c0.empty()) {
            c0.assign(static_cast<size_t>(out_count), 0.0f);
        }
        if (c1.empty()) {
            c1.assign(static_cast<size_t>(out_count), 1.0f);
        }
        if (static_cast<int>(c0.size()) < out_count) {
            c0.resize(static_cast<size_t>(out_count), c0.back());
        }
        if (static_cast<int>(c1.size()) < out_count) {
            c1.resize(static_cast<size_t>(out_count), c1.back());
        }

        std::vector<float> domain = parse_float_array_from_dict_entries_fitz(entries, "Domain");
        if (domain.size() < 2) {
            domain = {0.0f, 1.0f};
        }

        out_space.has_tint_transform = true;
        out_space.tint_function_type = 2;
        out_space.tint_input_count = 1;
        out_space.tint_output_count = out_count;
        out_space.tint_n = parse_float_from_dict_entries_fitz(entries, "N", 1.0f);
        out_space.tint_domain = std::move(domain);
        out_space.tint_range = parse_float_array_from_dict_entries_fitz(entries, "Range");
        out_space.tint_c0 = std::move(c0);
        out_space.tint_c1 = std::move(c1);
        out_space.tint_size.clear();
        out_space.tint_bits_per_sample = 0;
        out_space.tint_encode.clear();
        out_space.tint_decode.clear();
        out_space.tint_samples.clear();
        return true;
    }

    if (function_type == 0) {
        std::vector<int> size = parse_int_array_from_dict_entries_fitz(entries, "Size");
        if (size.empty()) {
            return false;
        }
        for (int dim : size) {
            if (dim <= 0) {
                return false;
            }
        }

        const int bits_per_sample = parse_int_from_dict_entries_fitz(entries, "BitsPerSample", 0);
        if (bits_per_sample <= 0 || bits_per_sample > 32) {
            return false;
        }

        int input_count = static_cast<int>(size.size());
        if (expected_input_count > 0) {
            input_count = expected_input_count;
        }

        std::vector<float> domain = parse_float_array_from_dict_entries_fitz(entries, "Domain");
        if (domain.size() != static_cast<size_t>(input_count * 2)) {
            domain.assign(static_cast<size_t>(input_count * 2), 0.0f);
            for (int i = 0; i < input_count; ++i) {
                domain[static_cast<size_t>(2 * i + 1)] = 1.0f;
            }
        }

        std::vector<float> range = parse_float_array_from_dict_entries_fitz(entries, "Range");
        int output_count = static_cast<int>(range.size() / 2);
        if (output_count <= 0) {
            output_count = expected_output_count;
        }
        if (output_count <= 0) {
            output_count = 1;
        }

        std::vector<float> encode = parse_float_array_from_dict_entries_fitz(entries, "Encode");
        if (encode.size() != static_cast<size_t>(input_count * 2)) {
            encode.clear();
            encode.reserve(static_cast<size_t>(input_count * 2));
            for (int i = 0; i < input_count; ++i) {
                const int dim = (i < static_cast<int>(size.size())) ? size[static_cast<size_t>(i)] : 1;
                encode.push_back(0.0f);
                encode.push_back(static_cast<float>(std::max(dim - 1, 0)));
            }
        }

        std::vector<float> decode = parse_float_array_from_dict_entries_fitz(entries, "Decode");
        if (decode.size() != static_cast<size_t>(output_count * 2)) {
            if (range.size() == static_cast<size_t>(output_count * 2)) {
                decode = range;
            } else {
                decode.assign(static_cast<size_t>(output_count * 2), 0.0f);
                for (int i = 0; i < output_count; ++i) {
                    decode[static_cast<size_t>(2 * i + 1)] = 1.0f;
                }
            }
        }

        uint64_t sample_count = 1;
        for (int dim : size) {
            const uint64_t d = static_cast<uint64_t>(dim);
            if (d == 0 || sample_count > (std::numeric_limits<uint64_t>::max() / d)) {
                return false;
            }
            sample_count *= d;
        }

        const uint64_t total_values = sample_count * static_cast<uint64_t>(output_count);
        const uint64_t total_bits = total_values * static_cast<uint64_t>(bits_per_sample);
        const uint64_t needed_bytes = (total_bits + 7ull) / 8ull;
        if (needed_bytes == 0 || decoded_stream.size() < needed_bytes) {
            return false;
        }

        out_space.has_tint_transform = true;
        out_space.tint_function_type = 0;
        out_space.tint_input_count = input_count;
        out_space.tint_output_count = output_count;
        out_space.tint_n = 1.0f;
        out_space.tint_domain = std::move(domain);
        out_space.tint_range = std::move(range);
        out_space.tint_c0.clear();
        out_space.tint_c1.clear();
        out_space.tint_size = std::move(size);
        out_space.tint_bits_per_sample = bits_per_sample;
        out_space.tint_encode = std::move(encode);
        out_space.tint_decode = std::move(decode);
        out_space.tint_samples.assign(decoded_stream.begin(), decoded_stream.begin() + static_cast<std::ptrdiff_t>(needed_bytes));
        return true;
    }

    return false;
}

static bool parse_tint_transform_expr_fitz(const std::string& expr,
                                           WinPdfDocument* resolver,
                                           int expected_input_count,
                                           int expected_output_count,
                                           WinColorSpaceDef& out_space,
                                           int depth) {
    if (depth > 10) {
        return false;
    }

    const std::string t = trim_copy(expr);
    if (t.empty()) {
        return false;
    }

    int ref_id = 0;
    if (parse_ref_id_from_expr(t, ref_id) && resolver != nullptr) {
        WinPdfObject func_obj = resolver->read_obj(ref_id);
        std::string function_dict = func_obj.dict;
        if (function_dict.empty()) {
            function_dict = extract_first_dict_fragment(func_obj.body);
        }

        std::vector<uint8_t> function_stream;
        if (func_obj.is_stream && !func_obj.stream.empty()) {
            function_stream = decode_stream_data(func_obj.stream, function_dict, resolver);
            if (function_stream.empty()) {
                function_stream = func_obj.stream;
            }
        }

        return parse_tint_function_from_dict_fitz(
            function_dict,
            function_stream,
            expected_input_count,
            expected_output_count,
            out_space);
    }

    if (t.rfind("<<", 0) == 0) {
        return parse_tint_function_from_dict_fitz(t, {}, expected_input_count, expected_output_count, out_space);
    }

    return false;
}

static bool evaluate_tint_transform_fitz(const WinColorSpaceDef& space,
                                         const std::vector<float>& inputs,
                                         std::vector<float>& out_values) {
    auto clamp_to_range = [](float v, float lo, float hi) -> float {
        float min_v = lo;
        float max_v = hi;
        if (min_v > max_v) {
            std::swap(min_v, max_v);
        }
        if (v < min_v) return min_v;
        if (v > max_v) return max_v;
        return v;
    };

    if (!space.has_tint_transform || space.tint_function_type < 0) {
        return false;
    }

    if (space.tint_function_type == 2) {
        if (space.tint_output_count <= 0) {
            return false;
        }

        float x = inputs.empty() ? 0.0f : inputs[0];
        float d0 = 0.0f;
        float d1 = 1.0f;
        if (space.tint_domain.size() >= 2) {
            d0 = space.tint_domain[0];
            d1 = space.tint_domain[1];
        }

        x = clamp_to_range(x, d0, d1);
        float t = 0.0f;
        if (std::fabs(d1 - d0) > 1e-6f) {
            t = (x - d0) / (d1 - d0);
        }
        t = clamp_to_range(t, 0.0f, 1.0f);

        float exponent = space.tint_n;
        if (!std::isfinite(exponent) || exponent == 0.0f) {
            exponent = 1.0f;
        }
        const float tn = static_cast<float>(std::pow(static_cast<double>(t), static_cast<double>(exponent)));

        out_values.assign(static_cast<size_t>(space.tint_output_count), 0.0f);
        for (int i = 0; i < space.tint_output_count; ++i) {
            const float c0 = (i < static_cast<int>(space.tint_c0.size())) ? space.tint_c0[static_cast<size_t>(i)] : 0.0f;
            const float c1 = (i < static_cast<int>(space.tint_c1.size())) ? space.tint_c1[static_cast<size_t>(i)] : 1.0f;
            float out = c0 + tn * (c1 - c0);
            if (space.tint_range.size() >= static_cast<size_t>((i + 1) * 2)) {
                out = clamp_to_range(out, space.tint_range[static_cast<size_t>(2 * i)], space.tint_range[static_cast<size_t>(2 * i + 1)]);
            }
            out_values[static_cast<size_t>(i)] = out;
        }
        return true;
    }

    if (space.tint_function_type == 0) {
        if (space.tint_input_count <= 0 || space.tint_output_count <= 0) {
            return false;
        }
        if (space.tint_size.empty() || space.tint_bits_per_sample <= 0 || space.tint_samples.empty()) {
            return false;
        }
        if (space.tint_input_count != 1 || space.tint_size.size() != 1) {
            return false;
        }

        const int sample_count = space.tint_size[0];
        if (sample_count <= 0) {
            return false;
        }

        auto read_sample = [&](int sample_index, int output_index, float& out_norm) -> bool {
            if (sample_index < 0 || sample_index >= sample_count || output_index < 0 || output_index >= space.tint_output_count) {
                return false;
            }

            const uint64_t value_index = static_cast<uint64_t>(sample_index) * static_cast<uint64_t>(space.tint_output_count)
                                       + static_cast<uint64_t>(output_index);
            const uint64_t bit_start = value_index * static_cast<uint64_t>(space.tint_bits_per_sample);
            const uint64_t bit_end = bit_start + static_cast<uint64_t>(space.tint_bits_per_sample);
            const uint64_t available_bits = static_cast<uint64_t>(space.tint_samples.size()) * 8ull;
            if (bit_end > available_bits) {
                return false;
            }

            uint64_t packed = 0;
            for (int bit = 0; bit < space.tint_bits_per_sample; ++bit) {
                const uint64_t bit_pos = bit_start + static_cast<uint64_t>(bit);
                const size_t byte_index = static_cast<size_t>(bit_pos / 8ull);
                const int bit_in_byte = 7 - static_cast<int>(bit_pos % 8ull);
                const uint8_t bit_value = static_cast<uint8_t>((space.tint_samples[byte_index] >> bit_in_byte) & 0x1u);
                packed = (packed << 1ull) | static_cast<uint64_t>(bit_value);
            }

            const uint64_t max_packed = (space.tint_bits_per_sample == 64)
                ? std::numeric_limits<uint64_t>::max()
                : ((1ull << static_cast<uint64_t>(space.tint_bits_per_sample)) - 1ull);
            if (max_packed == 0ull) {
                out_norm = 0.0f;
                return true;
            }

            out_norm = static_cast<float>(static_cast<double>(packed) / static_cast<double>(max_packed));
            return true;
        };

        const float input = inputs.empty() ? 0.0f : inputs[0];
        float d0 = 0.0f;
        float d1 = 1.0f;
        if (space.tint_domain.size() >= 2) {
            d0 = space.tint_domain[0];
            d1 = space.tint_domain[1];
        }

        const float clamped_input = clamp_to_range(input, d0, d1);
        float unit = 0.0f;
        if (std::fabs(d1 - d0) > 1e-6f) {
            unit = (clamped_input - d0) / (d1 - d0);
        }
        unit = clamp_to_range(unit, 0.0f, 1.0f);

        float e0 = 0.0f;
        float e1 = static_cast<float>(std::max(sample_count - 1, 0));
        if (space.tint_encode.size() >= 2) {
            e0 = space.tint_encode[0];
            e1 = space.tint_encode[1];
        }

        float sample_pos = e0 + unit * (e1 - e0);
        sample_pos = clamp_to_range(sample_pos, 0.0f, static_cast<float>(std::max(sample_count - 1, 0)));

        int i0 = static_cast<int>(std::floor(sample_pos));
        int i1 = std::min(i0 + 1, sample_count - 1);
        const float frac = sample_pos - static_cast<float>(i0);

        out_values.assign(static_cast<size_t>(space.tint_output_count), 0.0f);
        for (int out_i = 0; out_i < space.tint_output_count; ++out_i) {
            float s0 = 0.0f;
            float s1 = 0.0f;
            if (!read_sample(i0, out_i, s0) || !read_sample(i1, out_i, s1)) {
                return false;
            }

            const float sampled = s0 + (s1 - s0) * frac;

            float dec0 = 0.0f;
            float dec1 = 1.0f;
            if (space.tint_decode.size() >= static_cast<size_t>((out_i + 1) * 2)) {
                dec0 = space.tint_decode[static_cast<size_t>(2 * out_i)];
                dec1 = space.tint_decode[static_cast<size_t>(2 * out_i + 1)];
            }

            float value = dec0 + sampled * (dec1 - dec0);
            if (space.tint_range.size() >= static_cast<size_t>((out_i + 1) * 2)) {
                value = clamp_to_range(value,
                                       space.tint_range[static_cast<size_t>(2 * out_i)],
                                       space.tint_range[static_cast<size_t>(2 * out_i + 1)]);
            }

            out_values[static_cast<size_t>(out_i)] = value;
        }
        return true;
    }

    return false;
}

static bool read_be_u16_fitz(const std::vector<uint8_t>& data, size_t off, uint16_t& out) {
    if (off + 2 > data.size()) {
        return false;
    }
    out = static_cast<uint16_t>((static_cast<uint16_t>(data[off]) << 8) |
                                static_cast<uint16_t>(data[off + 1]));
    return true;
}

static bool read_be_u32_fitz(const std::vector<uint8_t>& data, size_t off, uint32_t& out) {
    if (off + 4 > data.size()) {
        return false;
    }
    out = (static_cast<uint32_t>(data[off]) << 24) |
          (static_cast<uint32_t>(data[off + 1]) << 16) |
          (static_cast<uint32_t>(data[off + 2]) << 8) |
          static_cast<uint32_t>(data[off + 3]);
    return true;
}

static bool read_be_s15fixed16_fitz(const std::vector<uint8_t>& data, size_t off, float& out) {
    uint32_t raw = 0;
    if (!read_be_u32_fitz(data, off, raw)) {
        return false;
    }
    const int32_t sval = static_cast<int32_t>(raw);
    out = static_cast<float>(static_cast<double>(sval) / 65536.0);
    return true;
}

static bool parse_icc_xyz_tag_fitz(const std::vector<uint8_t>& profile,
                                   size_t tag_off,
                                   size_t tag_len,
                                   float& x,
                                   float& y,
                                   float& z) {
    if (tag_off + tag_len > profile.size() || tag_len < 20 || tag_off + 20 > profile.size()) {
        return false;
    }

    const std::string type(reinterpret_cast<const char*>(profile.data() + tag_off), 4);
    if (type != "XYZ ") {
        return false;
    }

    return read_be_s15fixed16_fitz(profile, tag_off + 8, x) &&
           read_be_s15fixed16_fitz(profile, tag_off + 12, y) &&
           read_be_s15fixed16_fitz(profile, tag_off + 16, z);
}

static bool parse_icc_curve_tag_fitz(const std::vector<uint8_t>& profile,
                                     size_t tag_off,
                                     size_t tag_len,
                                     WinIccCurve& out_curve) {
    out_curve = WinIccCurve{};

    if (tag_off + tag_len > profile.size() || tag_len < 12 || tag_off + 12 > profile.size()) {
        return false;
    }

    const std::string type(reinterpret_cast<const char*>(profile.data() + tag_off), 4);
    if (type == "curv") {
        uint32_t count = 0;
        if (!read_be_u32_fitz(profile, tag_off + 8, count)) {
            return false;
        }

        if (count == 0) {
            out_curve.type = WinIccCurveType::Identity;
            return true;
        }

        if (count == 1) {
            uint16_t gamma_u8f8 = 0;
            if (!read_be_u16_fitz(profile, tag_off + 12, gamma_u8f8)) {
                return false;
            }
            out_curve.type = WinIccCurveType::Gamma;
            out_curve.gamma = static_cast<float>(gamma_u8f8) / 256.0f;
            if (!(out_curve.gamma > 0.0f)) {
                out_curve.gamma = 1.0f;
            }
            return true;
        }

        const size_t needed = 12u + static_cast<size_t>(count) * 2u;
        if (needed > tag_len || tag_off + needed > profile.size()) {
            return false;
        }

        out_curve.type = WinIccCurveType::Table;
        out_curve.table.resize(static_cast<size_t>(count));
        for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
            uint16_t v = 0;
            if (!read_be_u16_fitz(profile, tag_off + 12 + i * 2, v)) {
                return false;
            }
            out_curve.table[i] = static_cast<float>(v) / 65535.0f;
        }
        return true;
    }

    if (type == "para") {
        uint16_t fn_type = 0;
        if (!read_be_u16_fitz(profile, tag_off + 8, fn_type)) {
            return false;
        }
        if (fn_type == 0 && tag_len >= 16 && tag_off + 16 <= profile.size()) {
            float gamma = 1.0f;
            if (!read_be_s15fixed16_fitz(profile, tag_off + 12, gamma)) {
                return false;
            }
            out_curve.type = WinIccCurveType::Gamma;
            out_curve.gamma = (gamma > 0.0f) ? gamma : 1.0f;
            return true;
        }
    }

    return false;
}

static bool parse_icc_rgb_profile_stream_fitz(const std::vector<uint8_t>& profile,
                                              WinColorSpaceDef& out_space) {
    if (profile.size() < 132) {
        return false;
    }

    const std::string color_space(reinterpret_cast<const char*>(profile.data() + 16), 4);
    if (color_space != "RGB ") {
        return false;
    }

    uint32_t tag_count = 0;
    if (!read_be_u32_fitz(profile, 128, tag_count)) {
        return false;
    }

    struct TagRef {
        size_t off = 0;
        size_t len = 0;
    };
    std::map<std::string, TagRef> tags;

    size_t entry_off = 132;
    for (uint32_t i = 0; i < tag_count; ++i) {
        if (entry_off + 12 > profile.size()) {
            break;
        }

        const std::string sig(reinterpret_cast<const char*>(profile.data() + entry_off), 4);
        uint32_t off_u32 = 0;
        uint32_t len_u32 = 0;
        if (!read_be_u32_fitz(profile, entry_off + 4, off_u32) ||
            !read_be_u32_fitz(profile, entry_off + 8, len_u32)) {
            entry_off += 12;
            continue;
        }

        const size_t off = static_cast<size_t>(off_u32);
        const size_t len = static_cast<size_t>(len_u32);
        if (off < profile.size() && off + len <= profile.size()) {
            tags[sig] = TagRef{off, len};
        }
        entry_off += 12;
    }

    const auto it_rxyz = tags.find("rXYZ");
    const auto it_gxyz = tags.find("gXYZ");
    const auto it_bxyz = tags.find("bXYZ");
    const auto it_rtrc = tags.find("rTRC");
    const auto it_gtrc = tags.find("gTRC");
    const auto it_btrc = tags.find("bTRC");
    if (it_rxyz == tags.end() || it_gxyz == tags.end() || it_bxyz == tags.end() ||
        it_rtrc == tags.end() || it_gtrc == tags.end() || it_btrc == tags.end()) {
        return false;
    }

    float rX = 0.0f, rY = 0.0f, rZ = 0.0f;
    float gX = 0.0f, gY = 0.0f, gZ = 0.0f;
    float bX = 0.0f, bY = 0.0f, bZ = 0.0f;
    if (!parse_icc_xyz_tag_fitz(profile, it_rxyz->second.off, it_rxyz->second.len, rX, rY, rZ) ||
        !parse_icc_xyz_tag_fitz(profile, it_gxyz->second.off, it_gxyz->second.len, gX, gY, gZ) ||
        !parse_icc_xyz_tag_fitz(profile, it_bxyz->second.off, it_bxyz->second.len, bX, bY, bZ)) {
        return false;
    }

    WinIccCurve trc_r;
    WinIccCurve trc_g;
    WinIccCurve trc_b;
    if (!parse_icc_curve_tag_fitz(profile, it_rtrc->second.off, it_rtrc->second.len, trc_r) ||
        !parse_icc_curve_tag_fitz(profile, it_gtrc->second.off, it_gtrc->second.len, trc_g) ||
        !parse_icc_curve_tag_fitz(profile, it_btrc->second.off, it_btrc->second.len, trc_b)) {
        return false;
    }

    out_space.kind = WinColorSpaceKind::ICCBased;
    out_space.component_count = 3;
    out_space.has_icc_rgb_profile = true;
    out_space.icc_rgb_to_xyz = {
        rX, gX, bX,
        rY, gY, bY,
        rZ, gZ, bZ
    };
    out_space.icc_trc_r = std::move(trc_r);
    out_space.icc_trc_g = std::move(trc_g);
    out_space.icc_trc_b = std::move(trc_b);
    return true;
}

static WinColorSpaceDef classify_colorspace_expr(const std::string& expr,
                                                 WinPdfDocument* resolver,
                                                 const std::map<std::string, std::string>* named_defs,
                                                 int depth);

static WinColorSpaceDef classify_colorspace_ref(int ref_id,
                                                WinPdfDocument* resolver,
                                                const std::map<std::string, std::string>* named_defs,
                                                int depth) {
    if (resolver == nullptr || ref_id <= 0 || depth > 10) {
        return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
    }

    WinPdfObject obj = resolver->read_obj(ref_id);
    if (obj.is_stream && !obj.dict.empty()) {
        const int n = parse_int_in_dict(obj.dict, "/N", 0);
        if (n == 1) {
            return make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
        }
        if (n == 3) {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::ICCBased, 3);
            if (!obj.stream.empty()) {
                std::vector<uint8_t> icc_profile = decode_stream_data(obj.stream, obj.dict, resolver);
                if (icc_profile.empty()) {
                    icc_profile = obj.stream;
                }
                parse_icc_rgb_profile_stream_fitz(icc_profile, out);
            }
            return out;
        }
        if (n == 4) {
            return make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
        }
        if (n > 0) {
            return make_colorspace_def(WinColorSpaceKind::ICCBased, n);
        }
    }

    std::string payload = trim_copy(obj.body);
    if (payload.empty()) {
        payload = trim_copy(obj.dict);
    }
    if (payload.empty()) {
        return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
    }

    return classify_colorspace_expr(payload, resolver, named_defs, depth + 1);
}

static WinColorSpaceDef classify_colorspace_expr(const std::string& expr,
                                                 WinPdfDocument* resolver,
                                                 const std::map<std::string, std::string>* named_defs,
                                                 int depth) {
    if (depth > 10) {
        return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
    }

    const std::string t = trim_copy(expr);
    if (t.empty()) {
        return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
    }

    if (!t.empty() && t[0] == '/') {
        const std::string name = parse_name_from_expr(t);
        WinColorSpaceDef direct = colorspace_from_name(name);
        if (direct.kind != WinColorSpaceKind::Unknown) {
            return direct;
        }
        if (named_defs != nullptr) {
            auto it = named_defs->find(name);
            if (it != named_defs->end()) {
                return classify_colorspace_expr(it->second, resolver, named_defs, depth + 1);
            }
        }
        return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
    }

    int ref_id = 0;
    if (parse_ref_id_from_expr(t, ref_id)) {
        return classify_colorspace_ref(ref_id, resolver, named_defs, depth + 1);
    }

    if (!t.empty() && t[0] == '[') {
        const std::vector<std::string> items = split_pdf_array_items(t);
        if (items.empty()) {
            return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
        }

        const std::string cs_name = parse_name_from_expr(items[0]);
        if (cs_name == "ICCBased") {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::ICCBased, 0);
            if (items.size() >= 2) {
                int profile_ref = 0;
                if (parse_ref_id_from_expr(items[1], profile_ref)) {
                    WinColorSpaceDef prof = classify_colorspace_ref(profile_ref, resolver, named_defs, depth + 1);
                    if (prof.component_count > 0) {
                        out.component_count = prof.component_count;
                    }
                    if (prof.kind == WinColorSpaceKind::DeviceGray ||
                        prof.kind == WinColorSpaceKind::DeviceRGB ||
                        prof.kind == WinColorSpaceKind::DeviceCMYK ||
                        prof.kind == WinColorSpaceKind::ICCBased) {
                        return prof;
                    }
                } else if (items[1].rfind("<<", 0) == 0) {
                    const int n = parse_int_in_dict(items[1], "/N", 0);
                    if (n == 1) return make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
                    if (n == 3) return make_colorspace_def(WinColorSpaceKind::DeviceRGB, 3);
                    if (n == 4) return make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
                    if (n > 0) out.component_count = n;
                }
            }
            if (out.component_count <= 0) {
                out.component_count = 3;
            }
            return out;
        }

        if (cs_name == "Lab") {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::Lab, 3);
            if (items.size() >= 2) {
                float wx = out.lab_white_x;
                float wy = out.lab_white_y;
                float wz = out.lab_white_z;
                if (parse_lab_whitepoint_from_expr_fitz(items[1], wx, wy, wz)) {
                    if (wx > 0.0f && wy > 0.0f && wz > 0.0f) {
                        out.lab_white_x = wx;
                        out.lab_white_y = wy;
                        out.lab_white_z = wz;
                    }
                }
            }
            return out;
        }

        if (cs_name == "Separation") {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::Separation, 1);
            if (items.size() >= 3) {
                WinColorSpaceDef alt = classify_colorspace_expr(items[2], resolver, named_defs, depth + 1);
                out.alt_kind = alt.kind;
                out.alt_component_count = alt.component_count;
            }
            if (items.size() >= 4) {
                int expected_outputs = out.alt_component_count;
                if (expected_outputs <= 0) {
                    expected_outputs = infer_component_count_from_kind_fitz(out.alt_kind);
                }
                parse_tint_transform_expr_fitz(items[3], resolver, out.component_count, expected_outputs, out, depth + 1);
            }
            return out;
        }

        if (cs_name == "DeviceN") {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::DeviceN, 1);
            if (items.size() >= 2) {
                int names_count = 0;
                const std::string names_expr = trim_copy(items[1]);
                if (!names_expr.empty() && names_expr[0] == '[') {
                    const auto names = split_pdf_array_items(names_expr);
                    for (const auto& name_item : names) {
                        if (!parse_name_from_expr(name_item).empty()) {
                            ++names_count;
                        }
                    }
                }
                if (names_count > 0) {
                    out.component_count = names_count;
                }
            }
            if (items.size() >= 3) {
                WinColorSpaceDef alt = classify_colorspace_expr(items[2], resolver, named_defs, depth + 1);
                out.alt_kind = alt.kind;
                out.alt_component_count = alt.component_count;
            }
            if (items.size() >= 4) {
                int expected_outputs = out.alt_component_count;
                if (expected_outputs <= 0) {
                    expected_outputs = infer_component_count_from_kind_fitz(out.alt_kind);
                }
                parse_tint_transform_expr_fitz(items[3], resolver, out.component_count, expected_outputs, out, depth + 1);
            }
            return out;
        }

        if (cs_name == "Indexed" || cs_name == "I") {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::Indexed, 1);
            if (items.size() >= 2) {
                WinColorSpaceDef base = classify_colorspace_expr(items[1], resolver, named_defs, depth + 1);
                out.alt_kind = base.kind;
                out.alt_component_count = base.component_count;
            }
            return out;
        }

        if (cs_name == "Pattern") {
            WinColorSpaceDef out = make_colorspace_def(WinColorSpaceKind::Pattern, 0);
            if (items.size() >= 2) {
                WinColorSpaceDef base = classify_colorspace_expr(items[1], resolver, named_defs, depth + 1);
                out.alt_kind = base.kind;
                out.alt_component_count = base.component_count;
                out.component_count = base.component_count;
            }
            return out;
        }

        WinColorSpaceDef direct = colorspace_from_name(cs_name);
        if (direct.kind != WinColorSpaceKind::Unknown) {
            return direct;
        }
    }

    return make_colorspace_def(WinColorSpaceKind::Unknown, 0);
}

static void parse_colorspace_dict_into_map(WinPdfDocument* resolver,
                                           const std::string& colorspace_dict,
                                           WinColorSpaceMap& out_map) {
    if (colorspace_dict.empty()) {
        return;
    }

    const std::map<std::string, std::string> entries = parse_pdf_dict_entries(colorspace_dict);
    for (const auto& entry : entries) {
        if (entry.first.empty() || entry.second.empty()) {
            continue;
        }
        out_map[entry.first] = classify_colorspace_expr(entry.second, resolver, &entries, 0);
    }
}

static std::string parse_name_or_string_value_after_key_fitz(const std::string& dict, const std::string& key) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return {};
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }
    if (pos >= dict.size()) {
        return {};
    }

    if (dict[pos] == '/') {
        ++pos;
        const size_t start = pos;
        while (pos < dict.size() && !std::isspace(static_cast<unsigned char>(dict[pos])) &&
               dict[pos] != '/' && dict[pos] != '<' && dict[pos] != '>' &&
               dict[pos] != '[' && dict[pos] != ']' && dict[pos] != '(' && dict[pos] != ')') {
            ++pos;
        }
        return dict.substr(start, pos - start);
    }

    if (dict[pos] == '(') {
        std::vector<uint8_t> bytes(dict.begin(), dict.end());
        size_t i = pos;
        std::vector<uint8_t> literal = parse_literal_string(bytes, i);
        return std::string(literal.begin(), literal.end());
    }

    if (dict[pos] == '<' && (pos + 1 >= dict.size() || dict[pos + 1] != '<')) {
        std::vector<uint8_t> bytes(dict.begin(), dict.end());
        size_t i = pos;
        std::vector<uint8_t> hex = parse_hex_string(bytes, i);
        return std::string(hex.begin(), hex.end());
    }

    return {};
}

static std::string build_cid_collection_name_from_cidsysteminfo_fitz(const std::string& cid_system_info_dict) {
    if (cid_system_info_dict.empty()) {
        return {};
    }

    std::string registry = trim_copy(parse_name_or_string_value_after_key_fitz(cid_system_info_dict, "/Registry"));
    std::string ordering = trim_copy(parse_name_or_string_value_after_key_fitz(cid_system_info_dict, "/Ordering"));
    if (registry.empty() || ordering.empty()) {
        return {};
    }

    return registry + "-" + ordering;
}

static std::map<int, int> make_identity_encoding_map() {
    std::map<int, int> m;
    for (int i = 0; i <= 255; ++i) {
        m[i] = i;
    }
    return m;
}

static std::map<int, int> make_glyph_table_encoding_map(const char *const names[256]) {
    std::map<int, int> m;
    for (int i = 0; i < 256; ++i) {
        const char* gname = names[i];
        if (gname == nullptr || gname[0] == '\0') {
            continue;
        }

        int cp = glyph_name_to_unicode_fitz(std::string(gname));
        if (cp > 0 && cp <= 0x10FFFF) {
            m[i] = cp;
        }
    }
    return m;
}

static std::map<int, int> make_mac_roman_encoding_map() {
    return make_glyph_table_encoding_map(fz_glyph_name_from_mac_roman);
}

static std::map<int, int> make_win_ansi_encoding_map() {
    return make_glyph_table_encoding_map(fz_glyph_name_from_win_ansi);
}

static std::map<int, int> make_standard_encoding_map() {
    return make_glyph_table_encoding_map(fz_glyph_name_from_adobe_standard);
}

static std::map<int, int> make_mac_expert_encoding_map() {
    return make_glyph_table_encoding_map(fz_glyph_name_from_mac_expert);
}

static std::map<int, int> build_encoding_map(const std::string& encoding_name) {
    if (encoding_name == "MacRomanEncoding") {
        return make_mac_roman_encoding_map();
    }
    if (encoding_name == "WinAnsiEncoding") {
        return make_win_ansi_encoding_map();
    }
    if (encoding_name == "StandardEncoding") {
        return make_standard_encoding_map();
    }
    if (encoding_name == "MacExpertEncoding") {
        return make_mac_expert_encoding_map();
    }
    return {};
}

static int parse_hex_codepoint(const std::string& s) {
    if (s.empty() || s.size() > 6) {
        return 0;
    }
    int v = 0;
    for (char c : s) {
        int n = -1;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        if (n < 0) {
            return 0;
        }
        v = (v << 4) | n;
    }
    return v;
}

#include "adobe_glyph_list.hpp"

static void append_utf8_codepoint(std::string& out, int cp) {
    if (cp == 0x2028 || cp == 0x2029) cp = '\n';
    if (cp <= 0 || cp > 0x10FFFF) {
        return;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
        return;
    }
    if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return;
        }
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return;
    }

    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

static std::vector<int> decode_utf8_to_codepoints(const std::string& text) {
    std::vector<int> out;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c0 = static_cast<unsigned char>(text[i]);
        if (c0 < 0x80) {
            out.push_back(static_cast<int>(c0));
            ++i;
            continue;
        }

        int cp = 0;
        size_t need = 0;
        if ((c0 & 0xE0) == 0xC0) {
            cp = c0 & 0x1F;
            need = 1;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = c0 & 0x0F;
            need = 2;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = c0 & 0x07;
            need = 3;
        } else {
            ++i;
            continue;
        }

        if (i + need >= text.size()) {
            break;
        }

        bool ok = true;
        for (size_t k = 1; k <= need; ++k) {
            unsigned char cb = static_cast<unsigned char>(text[i + k]);
            if ((cb & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cb & 0x3F);
        }

        if (!ok || cp <= 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            ++i;
            continue;
        }

        out.push_back(cp);
        i += need + 1;
    }
    return out;
}

static std::vector<int> decode_pdf_text_bytes_to_codepoints(const std::vector<uint8_t>& bytes) {
    std::vector<int> out;
    if (bytes.empty()) {
        return out;
    }

    if (bytes.size() >= 2 && ((bytes[0] == 0xFE && bytes[1] == 0xFF) || (bytes[0] == 0xFF && bytes[1] == 0xFE))) {
        const bool little_endian = (bytes[0] == 0xFF && bytes[1] == 0xFE);
        size_t i = 2;
        while (i + 1 < bytes.size()) {
            uint16_t u = 0;
            if (little_endian) {
                u = static_cast<uint16_t>(static_cast<uint16_t>(bytes[i]) | (static_cast<uint16_t>(bytes[i + 1]) << 8));
            } else {
                u = static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8) | static_cast<uint16_t>(bytes[i + 1]));
            }
            i += 2;

            uint32_t cp = u;
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < bytes.size()) {
                uint16_t v = 0;
                if (little_endian) {
                    v = static_cast<uint16_t>(static_cast<uint16_t>(bytes[i]) | (static_cast<uint16_t>(bytes[i + 1]) << 8));
                } else {
                    v = static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8) | static_cast<uint16_t>(bytes[i + 1]));
                }
                if (v >= 0xDC00 && v <= 0xDFFF) {
                    cp = 0x10000u + (((static_cast<uint32_t>(u) - 0xD800u) << 10) | (static_cast<uint32_t>(v) - 0xDC00u));
                    i += 2;
                }
            }

            if (cp > 0 && cp <= 0x10FFFF) {
                out.push_back(static_cast<int>(cp));
            }
        }
        return out;
    }

    static const std::map<int, int> win_ansi = make_win_ansi_encoding_map();
    out.reserve(bytes.size());
    for (uint8_t b : bytes) {
        int cp = static_cast<int>(b);
        if (b >= 0x80) {
            auto it = win_ansi.find(cp);
            if (it != win_ansi.end()) {
                cp = it->second;
            }
        }
        if (cp > 0 && cp <= 0x10FFFF) {
            out.push_back(cp);
        }
    }

    return out;
}

static bool extract_actual_text_from_dict(const std::string& dict_raw, std::string& out_utf8) {
    out_utf8.clear();
    if (dict_raw.empty()) {
        return false;
    }

    std::vector<uint8_t> data(dict_raw.begin(), dict_raw.end());
    size_t i = 0;
    skip_ws_and_comments(data, i);
    if (i + 1 >= data.size() || data[i] != '<' || data[i + 1] != '<') {
        return false;
    }
    i += 2;

    while (i < data.size()) {
        skip_ws_and_comments(data, i);
        if (i + 1 < data.size() && data[i] == '>' && data[i + 1] == '>') {
            break;
        }

        PdfToken key;
        if (!parse_operand(data, i, key)) {
            ++i;
            continue;
        }
        if (key.type != PdfToken::Type::Name) {
            continue;
        }

        PdfToken value;
        if (!parse_operand(data, i, value)) {
            continue;
        }

        if (key.name == "ActualText" && value.type == PdfToken::Type::String) {
            const std::vector<int> cps = decode_pdf_text_bytes_to_codepoints(value.bytes);
            if (cps.empty()) {
                return false;
            }
            for (int cp : cps) {
                append_utf8_codepoint(out_utf8, cp);
            }
            return !out_utf8.empty();
        }
    }

    return false;
}

static int glyph_name_to_unicode(std::string name) {
    if (name.empty()) {
        return 0;
    }

    size_t dot = name.find('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }

    size_t underscore = name.find('_');
    if (underscore != std::string::npos) {
        return glyph_name_to_unicode(name.substr(0, underscore));
    }

    const auto& builtin_glyphs = WinExtract::get_adobe_glyph_map();
    auto builtin_it = builtin_glyphs.find(name);
    if (builtin_it != builtin_glyphs.end()) {
        return builtin_it->second;
    }

    if (name.size() >= 7 && name.rfind("uni", 0) == 0 && ((name.size() - 3) % 4 == 0)) {
        std::string hex = name.substr(3, 4);
        return parse_hex_codepoint(hex);
    }
    if (name.size() >= 5 && name[0] == 'u') {
        std::string hex = name.substr(1);
        bool ok = true;
        for (char c : hex) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return parse_hex_codepoint(hex);
        }
    }

    return 0;
}

static void apply_differences_to_map(const std::string& dict, std::map<int, int>& map_out, std::map<int, std::string>* names_out = nullptr) {
    size_t diff_pos = dict.find("/Differences");
    if (diff_pos == std::string::npos) {
        return;
    }

    size_t pos = dict.find('[', diff_pos);
    if (pos == std::string::npos) {
        return;
    }

    ++pos;
    int code = -1;
    while (pos < dict.size()) {
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size() || dict[pos] == ']') {
            break;
        }

        if (dict[pos] == '/') {
            ++pos;
            size_t start = pos;
            while (pos < dict.size() && !std::isspace(static_cast<unsigned char>(dict[pos])) &&
                   dict[pos] != '/' && dict[pos] != '[' && dict[pos] != ']' && dict[pos] != '<' && dict[pos] != '>') {
                ++pos;
            }
            std::string gname = dict.substr(start, pos - start);
            if (code >= 0 && code <= 255) {
                if (names_out) {
                    (*names_out)[code] = gname;
                }
                int cp = glyph_name_to_unicode_fitz(gname);
                if (cp > 0) {
                    map_out[code] = cp;
                }
            }
            ++code;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(dict[pos])) || dict[pos] == '-' || dict[pos] == '+') {
            size_t start = pos;
            if (dict[pos] == '-' || dict[pos] == '+') {
                ++pos;
            }
            while (pos < dict.size() && std::isdigit(static_cast<unsigned char>(dict[pos]))) {
                ++pos;
            }
            code = std::atoi(dict.substr(start, pos - start).c_str());
            continue;
        }

        ++pos;
    }
}

} // namespace

void WinPdfInterpreter::run(const std::vector<uint8_t>& stream,
                           MuLogicExtractor& extractor,
                           const WinFontUnicodeMap& font_unicode_map,
                           const WinFontWidthMap& font_width_map,
                           const WinFontCodeBytesMap& font_code_bytes_map,
                           const WinFontCodeSpaceMap& font_codespace_map,
                           const WinFontMatrixMap& font_matrix_map,
                           const WinFontVerticalMetricsMap& font_vertical_metrics_map,
                           const WinColorSpaceMap& color_space_map,
                           const WinFormXObjectMap& form_xobject_map,
                           const float* initial_ctm,
                           int recursion_depth,
                           const Rect* page_mediabox,
                           const Rect* inherited_clip_box,
                           const uint32_t* inherited_fill_color,
                           const uint32_t* inherited_stroke_color,
                           const WinColorSpaceDef* inherited_fill_space,
                           const WinColorSpaceDef* inherited_stroke_space,
                           const int* inherited_text_render_mode) {
    if (recursion_depth > 32) {
        return;
    }

    TextState st;
    std::vector<PdfToken> operands;
    bool in_text_object = false;
    float ctm[6] = {1, 0, 0, 1, 0, 0};
    if (initial_ctm) {
        copy_matrix(ctm, initial_ctm);
    }
    struct GraphicsStateSnapshot {
        std::array<float, 6> ctm;
        bool has_clip = false;
        Rect clip = {0, 0, 0, 0};
        TextState text_state;
        const std::unordered_map<int, std::vector<int>>* font_map = nullptr;
        const std::unordered_map<int, float>* width_map = nullptr;
        const std::vector<WinCodeSpaceRange>* codespace_ranges = nullptr;
        int code_bytes = 1;
        float default_advance = 0.55f;
        bool is_italic = false;
        bool is_bold = false;
        float ascender = 0.8f;
        float descender = -0.2f;
        int bidi = 0;
        uint32_t fill_color = 0;
        uint32_t stroke_color = 0;
        WinColorSpaceDef fill_space;
        WinColorSpaceDef stroke_space;
        int render_mode = 0;
    };
    std::vector<GraphicsStateSnapshot> gstate_stack;
    const std::unordered_map<int, std::vector<int>>* active_font_map = nullptr;
    const std::unordered_map<int, float>* active_width_map = nullptr;
    const std::vector<WinCodeSpaceRange>* active_codespace_ranges = nullptr;
    int active_code_bytes = 1;
    float active_default_advance = 0.55f;
    bool active_is_italic = false;
    bool active_is_bold = false;
    bool active_is_serif = true;
    bool active_is_mono = false;
    float active_ascender = 0.8f;
    float active_descender = -0.2f;
    int active_bidi = 0;
    uint32_t current_fill_color = 0;
    uint32_t current_stroke_color = 0;
    WinColorSpaceDef current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
    WinColorSpaceDef current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
    int text_render_mode = 0;

    if (inherited_fill_color != nullptr) {
        current_fill_color = *inherited_fill_color;
    }
    if (inherited_stroke_color != nullptr) {
        current_stroke_color = *inherited_stroke_color;
    }
    if (inherited_fill_space != nullptr) {
        current_fill_space = *inherited_fill_space;
    }
    if (inherited_stroke_space != nullptr) {
        current_stroke_space = *inherited_stroke_space;
    }
    if (inherited_text_render_mode != nullptr) {
        text_render_mode = *inherited_text_render_mode;
        if (text_render_mode < 0) text_render_mode = 0;
        if (text_render_mode > 7) text_render_mode = 7;
    }

    bool clip_has_box = false;
    Rect current_clip_box = {0, 0, 0, 0};
    if (page_mediabox != nullptr) {
        clip_has_box = true;
        current_clip_box = *page_mediabox;
    }
    if (inherited_clip_box != nullptr) {
        if (!clip_has_box) {
            current_clip_box = *inherited_clip_box;
            clip_has_box = true;
        } else {
            current_clip_box.x0 = std::max(current_clip_box.x0, inherited_clip_box->x0);
            current_clip_box.y0 = std::max(current_clip_box.y0, inherited_clip_box->y0);
            current_clip_box.x1 = std::min(current_clip_box.x1, inherited_clip_box->x1);
            current_clip_box.y1 = std::min(current_clip_box.y1, inherited_clip_box->y1);
        }
    }

    bool pending_clip = false;
    bool path_has_bbox = false;
    Rect path_bbox = {0, 0, 0, 0};
    Vec2 path_current = {0.0f, 0.0f};
    Vec2 path_subpath_start = {0.0f, 0.0f};
    bool path_has_current = false;

    auto add_path_point = [&](const Vec2& pt) {
        if (!path_has_bbox) {
            path_bbox = {pt.x, pt.y, pt.x, pt.y};
            path_has_bbox = true;
            return;
        }
        path_bbox.x0 = std::min(path_bbox.x0, pt.x);
        path_bbox.y0 = std::min(path_bbox.y0, pt.y);
        path_bbox.x1 = std::max(path_bbox.x1, pt.x);
        path_bbox.y1 = std::max(path_bbox.y1, pt.y);
    };

    auto reset_current_path = [&]() {
        path_has_bbox = false;
        path_bbox = {0, 0, 0, 0};
        path_has_current = false;
        path_current = {0.0f, 0.0f};
        path_subpath_start = {0.0f, 0.0f};
    };

    auto consume_current_path = [&]() {
        if (pending_clip && path_has_bbox) {
            if (!clip_has_box) {
                current_clip_box = path_bbox;
                clip_has_box = true;
            } else {
                current_clip_box.x0 = std::max(current_clip_box.x0, path_bbox.x0);
                current_clip_box.y0 = std::max(current_clip_box.y0, path_bbox.y0);
                current_clip_box.x1 = std::min(current_clip_box.x1, path_bbox.x1);
                current_clip_box.y1 = std::min(current_clip_box.y1, path_bbox.y1);
            }
        }
        pending_clip = false;
        reset_current_path();
    };

    auto glyph_entirely_outside_box = [&](const float m[6], float adv, int wmode, float ascender, float descender, const Rect& box) -> bool {
        const float det = m[0] * m[3] - m[1] * m[2];
        const float m_size = std::sqrt(std::abs(det));
        if (!(m_size > 0.0f)) {
            return false;
        }

        Vec2 dir;
        if (wmode == 0) {
            dir = {m[0], m[1]};
        } else {
            dir = {-m[2], -m[3]};
        }

        const float dir_len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (!(dir_len > 0.0f)) {
            return false;
        }

        Vec2 ndir = {dir.x / dir_len, dir.y / dir_len};
        Vec2 p;
        Vec2 q;
        if (wmode == 0) {
            p = {m[4], m[5]};
            q = {m[4] + adv * dir.x, m[5] + adv * dir.y};
        } else {
            p = {m[4] - adv * dir.x, m[5] - adv * dir.y};
            q = {m[4], m[5]};
        }

        float asc = ascender;
        float dsc = -descender;
        if (!std::isfinite(asc) || asc <= 0.01f) {
            asc = 0.8f;
        }
        if (!std::isfinite(dsc) || dsc <= 0.01f) {
            dsc = 0.2f;
        }

        Vec2 up = {-ndir.y * m_size, ndir.x * m_size};

        Vec2 ll = {p.x - up.x * dsc, p.y - up.y * dsc};
        Vec2 ul = {p.x + up.x * asc, p.y + up.y * asc};
        Vec2 lr = {q.x - up.x * dsc, q.y - up.y * dsc};
        Vec2 ur = {q.x + up.x * asc, q.y + up.y * asc};

        Rect glyph_rect = {
            std::min({ll.x, ul.x, lr.x, ur.x}),
            std::min({ll.y, ul.y, lr.y, ur.y}),
            std::max({ll.x, ul.x, lr.x, ur.x}),
            std::max({ll.y, ul.y, lr.y, ur.y})
        };

        return glyph_rect.x1 <= box.x0 ||
               glyph_rect.y1 <= box.y0 ||
               glyph_rect.x0 >= box.x1 ||
               glyph_rect.y0 >= box.y1;
    };

    struct ActualTextState {
        std::vector<int> text;
    };
    std::vector<ActualTextState> actual_text_stack;

    auto active_actualtext = [&]() -> ActualTextState* {
        for (auto it = actual_text_stack.rbegin(); it != actual_text_stack.rend(); ++it) {
            if (!it->text.empty()) {
                return &(*it);
            }
        }
        return nullptr;
    };

    auto decode_actualtext_from_operands = [&](const std::vector<PdfToken>& op_tokens) -> std::vector<int> {
        for (auto it = op_tokens.rbegin(); it != op_tokens.rend(); ++it) {
            if (it->type != PdfToken::Type::Dictionary || it->dict.empty()) {
                continue;
            }

            std::string actual_utf8;
            if (extract_actual_text_from_dict(it->dict, actual_utf8)) {
                return decode_utf8_to_codepoints(actual_utf8);
            }
        }
        return {};
    };

    auto clamp01 = [](float v) -> float {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    };

    auto to_u8 = [&](float v) -> uint32_t {
        const float c = clamp01(v);
        return static_cast<uint32_t>(std::lround(c * 255.0f));
    };

    auto rgb_from_gray = [&](float g) -> uint32_t {
        const uint32_t gv = to_u8(g);
        return (gv << 16) | (gv << 8) | gv;
    };

    auto rgb_from_cmyk = [&](float c, float m, float y, float k) -> uint32_t {
        const float cc = clamp01(c);
        const float mm = clamp01(m);
        const float yy = clamp01(y);
        const float kk = clamp01(k);

#if defined(WINNERZ_USE_LCMS2) && WINNERZ_USE_LCMS2
        float ir = 0.0f;
        float ig = 0.0f;
        float ib = 0.0f;
        if (win_icc_convert_cmyk_to_rgb(cc, mm, yy, kk, ir, ig, ib)) {
            return (to_u8(ir) << 16) | (to_u8(ig) << 8) | to_u8(ib);
        }
#endif

        // MuPDF's ICC-backed DeviceCMYK maps full K-only black to a dark neutral,
        // not absolute RGB 0. Keep that anchor for parity in Separation /Black flows.
        if (cc <= 1e-6f && mm <= 1e-6f && yy <= 1e-6f && kk >= 0.999f) {
            return (34u << 16) | (31u << 8) | 31u;
        }

        const float r = 1.0f - std::min(1.0f, cc + kk);
        const float g = 1.0f - std::min(1.0f, mm + kk);
        const float b = 1.0f - std::min(1.0f, yy + kk);
        return (to_u8(r) << 16) | (to_u8(g) << 8) | to_u8(b);
    };

    auto rgb_from_lab = [&](float l, float a, float b, const WinColorSpaceDef* space_def) -> uint32_t {
        float xw = 0.9642f;
        float yw = 1.0000f;
        float zw = 0.8249f;
        if (space_def != nullptr &&
            space_def->lab_white_x > 0.0f &&
            space_def->lab_white_y > 0.0f &&
            space_def->lab_white_z > 0.0f) {
            xw = space_def->lab_white_x;
            yw = space_def->lab_white_y;
            zw = space_def->lab_white_z;
        }

        const float ll = std::max(0.0f, std::min(100.0f, l));
        const float aa = std::max(-128.0f, std::min(127.0f, a));
        const float bb = std::max(-128.0f, std::min(127.0f, b));

#if defined(WINNERZ_USE_LCMS2) && WINNERZ_USE_LCMS2
        // MuPDF maps device Lab through its built-in ICC Lab profile (D50). Use that
        // path when the Lab whitepoint is effectively D50 to match fitz color output.
        if (std::abs(xw - 0.964203f) <= 1e-4f &&
            std::abs(yw - 1.000000f) <= 1e-4f &&
            std::abs(zw - 0.824905f) <= 1e-4f) {
            float ir = 0.0f;
            float ig = 0.0f;
            float ib = 0.0f;
            if (win_icc_convert_lab_to_rgb(ll, aa, bb, ir, ig, ib)) {
                return (to_u8(ir) << 16) | (to_u8(ig) << 8) | to_u8(ib);
            }
        }
#endif

        const float fy = (ll + 16.0f) / 116.0f;
        const float fx = fy + (aa / 500.0f);
        const float fz = fy - (bb / 200.0f);

        auto lab_inv = [](float t) -> float {
            constexpr float eps = 216.0f / 24389.0f;
            constexpr float kappa = 24389.0f / 27.0f;
            const float t3 = t * t * t;
            if (t3 > eps) {
                return t3;
            }
            return (116.0f * t - 16.0f) / kappa;
        };

        const float X = xw * lab_inv(fx);
        const float Y = yw * lab_inv(fy);
        const float Z = zw * lab_inv(fz);

        // D50 XYZ to linear sRGB.
        float r_lin =  3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
        float g_lin = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
        float b_lin =  0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;

        auto gamma_encode = [](float v) -> float {
            if (v <= 0.0f) {
                return 0.0f;
            }
            if (v < 0.0031308f) {
                return 12.92f * v;
            }
            return 1.055f * static_cast<float>(std::pow(static_cast<double>(v), 1.0 / 2.4)) - 0.055f;
        };

        r_lin = gamma_encode(r_lin);
        g_lin = gamma_encode(g_lin);
        b_lin = gamma_encode(b_lin);
        return (to_u8(r_lin) << 16) | (to_u8(g_lin) << 8) | to_u8(b_lin);
    };

    auto eval_icc_curve = [&](const WinIccCurve& curve, float x) -> float {
        const float in = clamp01(x);
        switch (curve.type) {
            case WinIccCurveType::Identity:
                return in;
            case WinIccCurveType::Gamma: {
                const float g = (curve.gamma > 0.0f) ? curve.gamma : 1.0f;
                return clamp01(static_cast<float>(std::pow(static_cast<double>(in), static_cast<double>(g))));
            }
            case WinIccCurveType::Table: {
                if (curve.table.empty()) {
                    return in;
                }
                if (curve.table.size() == 1) {
                    return clamp01(curve.table[0]);
                }
                const float pos = in * static_cast<float>(curve.table.size() - 1);
                const size_t i0 = static_cast<size_t>(std::floor(pos));
                const size_t i1 = std::min(i0 + 1, curve.table.size() - 1);
                const float frac = pos - static_cast<float>(i0);
                const float v0 = curve.table[i0];
                const float v1 = curve.table[i1];
                return clamp01(v0 + (v1 - v0) * frac);
            }
            default:
                return in;
        }
    };

    auto rgb_from_icc_profile = [&](const WinColorSpaceDef* space_def, float r, float g, float b) -> uint32_t {
        if (space_def == nullptr || !space_def->has_icc_rgb_profile) {
            return (to_u8(r) << 16) | (to_u8(g) << 8) | to_u8(b);
        }

        const float rr = eval_icc_curve(space_def->icc_trc_r, r);
        const float gg = eval_icc_curve(space_def->icc_trc_g, g);
        const float bb = eval_icc_curve(space_def->icc_trc_b, b);

        const auto& M = space_def->icc_rgb_to_xyz;
        const float X = M[0] * rr + M[1] * gg + M[2] * bb;
        const float Y = M[3] * rr + M[4] * gg + M[5] * bb;
        const float Z = M[6] * rr + M[7] * gg + M[8] * bb;

        float r_lin =  3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
        float g_lin = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
        float b_lin =  0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;

        auto gamma_encode = [](float v) -> float {
            if (v <= 0.0f) {
                return 0.0f;
            }
            if (v < 0.0031308f) {
                return 12.92f * v;
            }
            return 1.055f * static_cast<float>(std::pow(static_cast<double>(v), 1.0 / 2.4)) - 0.055f;
        };

        r_lin = gamma_encode(r_lin);
        g_lin = gamma_encode(g_lin);
        b_lin = gamma_encode(b_lin);

        return (to_u8(r_lin) << 16) | (to_u8(g_lin) << 8) | to_u8(b_lin);
    };

    auto set_color_from_components = [&](WinColorSpaceKind kind,
                                         const std::vector<float>& comps,
                                         const WinColorSpaceDef* space_def,
                                         uint32_t& out_color) -> bool {
        switch (kind) {
            case WinColorSpaceKind::DeviceGray:
                if (!comps.empty()) {
                    out_color = rgb_from_gray(comps[0]);
                    return true;
                }
                return false;
            case WinColorSpaceKind::DeviceRGB:
                if (comps.size() >= 3) {
                    out_color = (to_u8(comps[0]) << 16) | (to_u8(comps[1]) << 8) | to_u8(comps[2]);
                    return true;
                }
                if (!comps.empty()) {
                    out_color = rgb_from_gray(comps[0]);
                    return true;
                }
                return false;
            case WinColorSpaceKind::DeviceCMYK:
                if (comps.size() >= 4) {
                    out_color = rgb_from_cmyk(comps[0], comps[1], comps[2], comps[3]);
                    return true;
                }
                if (comps.size() >= 3) {
                    out_color = (to_u8(comps[0]) << 16) | (to_u8(comps[1]) << 8) | to_u8(comps[2]);
                    return true;
                }
                if (!comps.empty()) {
                    out_color = rgb_from_gray(comps[0]);
                    return true;
                }
                return false;
            case WinColorSpaceKind::Lab:
                if (comps.size() >= 3) {
                    out_color = rgb_from_lab(comps[0], comps[1], comps[2], space_def);
                    return true;
                }
                return false;
            case WinColorSpaceKind::ICCBased:
                if (space_def != nullptr && space_def->has_icc_rgb_profile && comps.size() >= 3) {
                    out_color = rgb_from_icc_profile(space_def, comps[0], comps[1], comps[2]);
                    return true;
                }
                if (space_def != nullptr) {
                    if (space_def->component_count == 1 && !comps.empty()) {
                        out_color = rgb_from_gray(comps[0]);
                        return true;
                    }
                    if (space_def->component_count == 3 && comps.size() >= 3) {
                        out_color = (to_u8(comps[0]) << 16) | (to_u8(comps[1]) << 8) | to_u8(comps[2]);
                        return true;
                    }
                    if (space_def->component_count == 4 && comps.size() >= 4) {
                        out_color = rgb_from_cmyk(comps[0], comps[1], comps[2], comps[3]);
                        return true;
                    }
                }
                return false;
            default:
                break;
        }
        return false;
    };

    auto resolve_named_color_space = [&](const std::string& name,
                                         const WinColorSpaceDef& fallback) -> WinColorSpaceDef {
        WinColorSpaceDef direct = colorspace_from_name(name);
        if (direct.kind != WinColorSpaceKind::Unknown) {
            return direct;
        }

        auto it = color_space_map.find(name);
        if (it != color_space_map.end()) {
            return it->second;
        }

        return fallback;
    };

    auto apply_color_operands = [&](const WinColorSpaceDef& active_space,
                                    const std::vector<PdfToken>& op_tokens,
                                    uint32_t& out_color) -> bool {
        std::vector<float> comps;
        comps.reserve(op_tokens.size());
        for (const auto& tok : op_tokens) {
            if (tok.type == PdfToken::Type::Number) {
                comps.push_back(static_cast<float>(tok.number));
            }
        }
        if (comps.empty()) {
            return false;
        }

        WinColorSpaceKind kind = active_space.kind;
        if (kind == WinColorSpaceKind::Pattern && active_space.alt_kind != WinColorSpaceKind::Unknown) {
            kind = active_space.alt_kind;
        }

        if (kind == WinColorSpaceKind::ICCBased &&
            !(active_space.has_icc_rgb_profile && active_space.component_count == 3)) {
            if (active_space.component_count == 1) {
                kind = WinColorSpaceKind::DeviceGray;
            } else if (active_space.component_count == 3) {
                kind = WinColorSpaceKind::DeviceRGB;
            } else if (active_space.component_count == 4) {
                kind = WinColorSpaceKind::DeviceCMYK;
            }
        }

        if ((kind == WinColorSpaceKind::Separation || kind == WinColorSpaceKind::DeviceN) &&
            active_space.has_tint_transform) {
            std::vector<float> tint_values;
            if (evaluate_tint_transform_fitz(active_space, comps, tint_values) && !tint_values.empty()) {
                WinColorSpaceKind tint_kind = active_space.alt_kind;
                if (tint_kind == WinColorSpaceKind::Unknown) {
                    const int n = static_cast<int>(tint_values.size());
                    if (n == 1) {
                        tint_kind = WinColorSpaceKind::DeviceGray;
                    } else if (n == 3) {
                        tint_kind = WinColorSpaceKind::DeviceRGB;
                    } else if (n >= 4) {
                        tint_kind = WinColorSpaceKind::DeviceCMYK;
                    }
                }

                if (set_color_from_components(tint_kind, tint_values, &active_space, out_color)) {
                    return true;
                }
                if (set_color_from_components(active_space.alt_kind, tint_values, &active_space, out_color)) {
                    return true;
                }
            }
        }

        if (kind == WinColorSpaceKind::Separation ||
            kind == WinColorSpaceKind::DeviceN ||
            kind == WinColorSpaceKind::Indexed) {
            if (active_space.alt_kind != WinColorSpaceKind::Unknown) {
                kind = active_space.alt_kind;
            }
        }

        if (set_color_from_components(kind, comps, &active_space, out_color)) {
            return true;
        }

        if (set_color_from_components(active_space.alt_kind, comps, &active_space, out_color)) {
            return true;
        }

        if (comps.size() >= 4) {
            return set_color_from_components(WinColorSpaceKind::DeviceCMYK, comps, &active_space, out_color);
        }
        if (comps.size() >= 3) {
            return set_color_from_components(WinColorSpaceKind::DeviceRGB, comps, &active_space, out_color);
        }
        return set_color_from_components(WinColorSpaceKind::DeviceGray, comps, &active_space, out_color);
    };

    auto guess_bidi_level = [](int bidiclass, int cur_bidi) -> int {
        switch (bidiclass) {
            case UCDN_BIDI_CLASS_L: return 0;
            case UCDN_BIDI_CLASS_R:
            case UCDN_BIDI_CLASS_AL: return 1;
            case UCDN_BIDI_CLASS_EN:
            case UCDN_BIDI_CLASS_ES:
            case UCDN_BIDI_CLASS_ET: return 0;
            case UCDN_BIDI_CLASS_AN: return 1;
            case UCDN_BIDI_CLASS_CS:
            case UCDN_BIDI_CLASS_NSM:
            case UCDN_BIDI_CLASS_BN:
            case UCDN_BIDI_CLASS_B:
            case UCDN_BIDI_CLASS_S:
            case UCDN_BIDI_CLASS_WS:
            case UCDN_BIDI_CLASS_ON:
                return cur_bidi;
            default:
                return 0;
        }
    };

    auto current_text_color = [&]() -> uint32_t {
        switch (text_render_mode) {
            case 1:
            case 5:
                return current_stroke_color;
            default:
                return current_fill_color;
        }
    };

    auto flush_remaining_actualtext = [&](std::vector<int>& text, float adv) {
        if (text.empty()) {
            return;
        }

        const float fs = st.font_size;
        const float rise = st.text_rise;
        const float h_scale = st.h_scale / 100.0f;
        float stm[6] = {
            fs * h_scale * st.tm[0], fs * h_scale * st.tm[1],
            fs * st.tm[2], fs * st.tm[3],
            rise * st.tm[2] + st.tm[4],
            rise * st.tm[3] + st.tm[5]
        };

        float m[6];
        m[0] = stm[0] * ctm[0] + stm[1] * ctm[2];
        m[1] = stm[0] * ctm[1] + stm[1] * ctm[3];
        m[2] = stm[2] * ctm[0] + stm[3] * ctm[2];
        m[3] = stm[2] * ctm[1] + stm[3] * ctm[3];
        m[4] = stm[4] * ctm[0] + stm[5] * ctm[2] + ctm[4];
        m[5] = stm[4] * ctm[1] + stm[5] * ctm[3] + ctm[5];

        const bool actualtext_clipped = clip_has_box && glyph_entirely_outside_box(
            m,
            adv,
            st.wmode,
            active_ascender,
            active_descender,
            current_clip_box);

        if (!actualtext_clipped) {
            for (int cp : text) {
                if (cp <= 0 || cp > 0x10FFFF) {
                    continue;
                }

                int bidi_class = ucdn_get_bidi_class(static_cast<uint32_t>(cp));
                active_bidi = guess_bidi_level(bidi_class, active_bidi);

                float m_copy[6] = {m[0], m[1], m[2], m[3], m[4], m[5]};
                extractor.add_char(
                    cp,
                    0.0f,
                    0.0f,
                    adv,
                    m_copy,
                    st.font_name,
                    st.font_size,
                    current_text_color(),
                    active_is_bold, active_is_italic, active_is_serif, active_is_mono, st.wmode,
                    active_ascender, active_descender, active_bidi,
                    true);
            }
        }

        text.clear();
    };

    auto emit_text = [&](const std::vector<uint8_t>& bytes) {
        struct TextGlyphItem {
            int primary = 0;
            std::vector<int> unicode_seq;
            float adv = 0.0f;
            float m[6] = {1, 0, 0, 1, 0, 0};
            bool clipped = false;
            bool cid_fallback = false;
        };

        auto code_in_codespace = [](uint32_t code_value, int nbytes, const std::vector<WinCodeSpaceRange>& ranges) {
            for (const auto& r : ranges) {
                if (r.nbytes == nbytes && code_value >= r.low && code_value <= r.high) {
                    return true;
                }
            }
            return false;
        };

        // [PATCH 4 FIX]: sanitize_unicode_sequence — giữ nguyên U+FFFD như Fitz.
        // Fitz KHÔNG thay U+FFFD bằng raw code. Đây là ký tự thay thế chuẩn Unicode.
        auto sanitize_unicode_sequence = [](int code, std::vector<int>& seq) -> bool {
            auto is_valid_scalar = [](int cp) {
                return cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
            };

            if (seq.empty()) {
                // Không có mapping → dùng U+FFFD như Fitz
                seq.push_back(0xFFFD);
                return false;
            }

            // [PATCH 4 FIX]: KHÔNG ghi đè U+FFFD bằng raw code.
            // Fitz giữ nguyên U+FFFD làm ký tự thay thế chuẩn.
            // (Xóa logic sai: if (seq.front() == 0xFFFD) seq.front() = code;)

            std::vector<int> clean;
            clean.reserve(seq.size());
            for (int cp : seq) {
                if (is_valid_scalar(cp)) {
                    clean.push_back(cp);
                }
            }

            if (clean.empty()) {
                // Toàn bộ seq không hợp lệ → U+FFFD như Fitz
                clean.push_back(0xFFFD);
                seq.swap(clean);
                return false;
            }
            seq.swap(clean);
            return false;
        };

        auto emit_rune = [&](int rune, float adv, const float in_m[6], bool has_real_glyph, bool preserve_bidi = false) {
            auto dispatch_char = [&](int cp, float char_adv, bool is_primary, bool keep_current_bidi) {
                if (cp <= 0 || cp > 0x10FFFF) {
                    cp = 0xFFFD; // Ký tự thay thế
                }

                if (!keep_current_bidi) {
                    int bidi_class = UCDN_BIDI_CLASS_ON;
                    if (cp > 0 && cp <= 0x10FFFF) {
                        bidi_class = ucdn_get_bidi_class(static_cast<uint32_t>(cp));
                    }
                    active_bidi = guess_bidi_level(bidi_class, active_bidi);
                }

                const uint32_t used_color = current_text_color();

                float m_copy[6] = {in_m[0], in_m[1], in_m[2], in_m[3], in_m[4], in_m[5]};
                extractor.add_char(
                    cp, 0.0f, 0.0f, char_adv, m_copy,
                    st.font_name, st.font_size, used_color,
                    active_is_bold, active_is_italic, active_is_serif, active_is_mono, st.wmode,
                    active_ascender, active_descender, active_bidi,
                    is_primary);
            };

                    dispatch_char(rune, adv, has_real_glyph, preserve_bidi);
        };

        const float h_scale = st.h_scale / 100.0f;
        std::vector<TextGlyphItem> glyphs;
        glyphs.reserve(bytes.size());

        size_t bi = 0;
        while (bi < bytes.size()) {
            const size_t code_start = bi;
            int code = static_cast<int>(bytes[bi]);
            int consumed = 1;

            int max_len = std::min<int>(active_code_bytes, static_cast<int>(bytes.size() - bi));
            if (max_len > 1) {
                bool matched = false;

                if (active_codespace_ranges && !active_codespace_ranges->empty()) {
                    for (int len = max_len; len >= 1; --len) {
                        uint32_t candidate = 0;
                        for (int k = 0; k < len; ++k) {
                            candidate = (candidate << 8) | static_cast<uint32_t>(bytes[bi + k]);
                        }
                        if (code_in_codespace(candidate, len, *active_codespace_ranges)) {
                            code = static_cast<int>(candidate);
                            consumed = len;
                            matched = true;
                            break;
                        }
                    }
                }

                if (!matched && (!active_codespace_ranges || active_codespace_ranges->empty())) {
                    uint32_t candidate = 0;
                    for (int k = 0; k < max_len; ++k) {
                        candidate = (candidate << 8) | static_cast<uint32_t>(bytes[bi + k]);
                    }
                    code = static_cast<int>(candidate);
                    consumed = max_len;
                    matched = true;
                }

                if (!matched) {
                    code = static_cast<int>(bytes[bi]);
                    consumed = 1;
                }
            }
            bi += static_cast<size_t>(consumed);

            float glyph_adv = active_default_advance;
            if (active_width_map) {
                auto wit = active_width_map->find(code);
                if (wit != active_width_map->end()) {
                    glyph_adv = wit->second;
                } else {
                    auto def_it = active_width_map->find(-1);
                    if (def_it != active_width_map->end()) {
                        glyph_adv = def_it->second;
                    } else if (code == ' ') {
                        glyph_adv = 0.33f;
                    }
                }
            } else if (code == ' ') {
                glyph_adv = 0.33f;
            }

            const float fs = st.font_size;
            const float rise = st.text_rise;
            float stm[6] = {
                fs * h_scale * st.tm[0], fs * h_scale * st.tm[1],
                fs * st.tm[2], fs * st.tm[3],
                rise * st.tm[2] + st.tm[4],
                rise * st.tm[3] + st.tm[5]
            };

            float m[6];
            m[0] = stm[0] * ctm[0] + stm[1] * ctm[2];
            m[1] = stm[0] * ctm[1] + stm[1] * ctm[3];
            m[2] = stm[2] * ctm[0] + stm[3] * ctm[2];
            m[3] = stm[2] * ctm[1] + stm[3] * ctm[3];
            m[4] = stm[4] * ctm[0] + stm[5] * ctm[2] + ctm[4];
            m[5] = stm[4] * ctm[1] + stm[5] * ctm[3] + ctm[5];

            bool glyph_clipped = false;
            if (clip_has_box) {
                glyph_clipped = glyph_entirely_outside_box(
                    m, glyph_adv, st.wmode, active_ascender, active_descender, current_clip_box);
            }

            std::vector<int> unicode_seq;
            if (active_font_map) {
                auto it = active_font_map->find(code);
                if (it != active_font_map->end()) {
                    unicode_seq = it->second;

                    // [PATCH 1]: Normalize whitespace control chars (8–13) → space, như Fitz.
                    // Fitz: if (ucs >= 8 && ucs <= 13) ucs = 32;
                    for (int& ucs : unicode_seq) {
                        if (ucs >= 8 && ucs <= 13) {
                            ucs = ' ';
                        }
                    }

                    // [PATCH 2]: Lọc Unicode rác từ ToUnicode CMap, nhưng giữ lại các ký tự WinAnsi hữu ích.
                    bool has_invalid = false;
                    for (int& ucs : unicode_seq) {
                        // [FIX]: Remap common WinAnsi characters in the 128-159 range
                        if (ucs >= 128 && ucs <= 159) {
                            static const unsigned short win_ansi_to_unicode[32] = {
                                0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
                                0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
                            };
                            unsigned short mapped = win_ansi_to_unicode[ucs - 128];
                            if (mapped != 0) {
                                ucs = mapped;
                            }
                        }

                        if (ucs >= 0 && ucs < 32 && ucs != 9 && ucs != 10 && ucs != 13) {
                            has_invalid = true;
                            break;
                        }
                    }
                    if (has_invalid) {
                        unicode_seq.clear(); // ép fallback
                    }
                }
            }

            bool cid_fallback = false;

            // [PATCH 3]: Fallback → U+FFFD (chuẩn Unicode/Fitz), không dùng raw code.
            if (unicode_seq.empty()) {
                unicode_seq.push_back(0xFFFD);
                cid_fallback = false; // U+FFFD là ký tự hợp lệ, không phải CID fallback
            } else {
                cid_fallback = sanitize_unicode_sequence(code, unicode_seq);
            }

            TextGlyphItem item;
            item.primary = unicode_seq.front();
            item.unicode_seq = std::move(unicode_seq);
            item.adv = glyph_adv;
            item.m[0] = m[0]; item.m[1] = m[1]; item.m[2] = m[2];
            item.m[3] = m[3]; item.m[4] = m[4]; item.m[5] = m[5];
            item.clipped = glyph_clipped;
            item.cid_fallback = cid_fallback;
            glyphs.push_back(std::move(item));

            // [PATCH 1]: Khoảng cách từ (Word Spacing)
            const bool is_word_space = ((consumed == 1 && code == 0x20) || (consumed == 2 && code == 0x0020)) && st.wmode == 0;
            if (st.wmode == 0) {
                float tx = (glyph_adv * fs + st.char_spacing) * h_scale;
                if (is_word_space) {
                    tx += st.word_spacing * h_scale;
                }
                st.tm[4] += tx * st.tm[0];
                st.tm[5] += tx * st.tm[1];
            } else {
                float ty = (glyph_adv * fs + st.char_spacing);
                if (is_word_space) {
                    ty += st.word_spacing;
                }
                st.tm[4] += ty * st.tm[2];
                st.tm[5] += ty * st.tm[3];
            }
        }

        // [PATCH 3]: Đồng bộ 100% logic ActualText của Fitz (hàm do_extract_within_actualtext)
        ActualTextState* at = active_actualtext();
        
        // Nếu không có ActualText, in chữ như bình thường
        if (at == nullptr || at->text.empty()) {
            for (const auto& item : glyphs) {
                if (item.clipped) continue;
                emit_rune(item.primary, item.adv, item.m, true, item.cid_fallback);
                for (size_t si = 1; si < item.unicode_seq.size(); ++si) {
                    emit_rune(item.unicode_seq[si], 0.0f, item.m, false);
                }
            }
            return;
        }

        size_t start = 0;
        size_t end = glyphs.size();
        size_t actual_len = at->text.size();
        size_t actual_start = 0;
        size_t actual_end = actual_len;

        // 1. Spot a matching prefix (Dò tiền tố trùng khớp)
        for (start = 0; start < glyphs.size(); ++start) {
            if (actual_start >= actual_len || glyphs[start].primary != at->text[actual_start]) {
                break;
            }
            actual_start++;
        }

        if (start != 0) {
            for (size_t i = 0; i < start; ++i) {
                if (!glyphs[i].clipped) emit_rune(glyphs[i].primary, glyphs[i].adv, glyphs[i].m, true);
            }
        }

        if (start == glyphs.size()) {
            // Rút gọn actualtext và thoát
            at->text.erase(at->text.begin(), at->text.begin() + actual_start);
            return;
        }

        // 2. Spot a matching postfix (Dò hậu tố trùng khớp)
        for (end = glyphs.size(); end > start; --end) {
            if (actual_end <= actual_start || glyphs[end - 1].primary != at->text[actual_end - 1]) {
                break;
            }
            actual_end--;
        }

        // 3. Do the difficult bit in the middle (Xử lý phần không khớp ở giữa)
        for (size_t i = start; i < end; ++i) {
            if (glyphs[i].clipped) continue;

            int rune = -1;
            if (actual_start < actual_end) {
                rune = at->text[actual_start++];
            }

            if (rune != -1) {
                emit_rune(rune, glyphs[i].adv, glyphs[i].m, true);
            } else {
                emit_rune(glyphs[i].primary, glyphs[i].adv, glyphs[i].m, false);
            }
        }

        if (end == glyphs.size()) {
            at->text.erase(at->text.begin(), at->text.begin() + actual_start);
            return;
        }

        // 4. Flush remaining actualtext (Xả nốt ActualText thừa giống hàm flush_actualtext)
        if (actual_start < actual_end) {
            float base_m[6] = {1, 0, 0, 1, 0, 0};
            if (end > 0 && end <= glyphs.size()) {
                copy_matrix(base_m, glyphs[end - 1].m);
                // Dịch bút (pen) tới cuối ký tự vừa in
                if (st.wmode == 0) {
                    base_m[4] += glyphs[end - 1].adv * base_m[0];
                    base_m[5] += glyphs[end - 1].adv * base_m[1];
                } else {
                    base_m[4] += glyphs[end - 1].adv * base_m[2];
                    base_m[5] += glyphs[end - 1].adv * base_m[3];
                }
            }
            while (actual_start < actual_end) {
                // Advance = 0 vì đây là các rune bị dồn lại
                emit_rune(at->text[actual_start++], 0.0f, base_m, false);
            }
        }

        // 5. Send postfix (Đẩy hậu tố ra)
        if (end != glyphs.size()) {
            for (size_t i = end; i < glyphs.size(); ++i) {
                if (!glyphs[i].clipped) emit_rune(glyphs[i].primary, glyphs[i].adv, glyphs[i].m, true);
            }
        }

        at->text.clear();
    };


    for (size_t i = 0; i < stream.size();) {
        skip_ws_and_comments(stream, i);
        if (i >= stream.size()) {
            break;
        }

        PdfToken tok;
        if (parse_operand(stream, i, tok)) {
            operands.push_back(std::move(tok));
            continue;
        }

        if (is_delimiter(stream[i])) {
            ++i;
            continue;
        }

        std::string op = parse_operator(stream, i);
        if (op.empty()) {
            ++i;
            continue;
        }

        if (op == "BI") {
            skip_inline_image(stream, i);
        } else if (op == "BT") {
            in_text_object = true;
            set_identity(st.tm);
            set_identity(st.tlm);
            active_bidi = 0;
            extractor.hint_new_text_obj();
            // Fitz: dev->new_obj chỉ bật khi bắt đầu một lần paint text (sau flush, vd. ET),
            // không phải mỗi Tj — trong BT..ET nhiều Tf/Tj vẫn một chuỗi ký tự liên tục.
        } else if (op == "q") {
            GraphicsStateSnapshot snap;
            snap.ctm = {ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]};
            snap.has_clip = clip_has_box;
            snap.clip = current_clip_box;
            snap.text_state = st;
            snap.font_map = active_font_map;
            snap.width_map = active_width_map;
            snap.codespace_ranges = active_codespace_ranges;
            snap.code_bytes = active_code_bytes;
            snap.default_advance = active_default_advance;
            snap.is_italic = active_is_italic;
            snap.is_bold = active_is_bold;
            snap.ascender = active_ascender;
            snap.descender = active_descender;
            snap.bidi = active_bidi;
            snap.fill_color = current_fill_color;
            snap.stroke_color = current_stroke_color;
            snap.fill_space = current_fill_space;
            snap.stroke_space = current_stroke_space;
            snap.render_mode = text_render_mode;
            gstate_stack.push_back(snap);
        } else if (op == "Q") {
            if (!gstate_stack.empty()) {
                GraphicsStateSnapshot top = gstate_stack.back();
                gstate_stack.pop_back();
                for (int k = 0; k < 6; ++k) {
                    ctm[k] = top.ctm[k];
                }
                clip_has_box = top.has_clip;
                current_clip_box = top.clip;
                st = std::move(top.text_state);
                active_font_map = top.font_map;
                active_width_map = top.width_map;
                active_codespace_ranges = top.codespace_ranges;
                active_code_bytes = top.code_bytes;
                active_default_advance = top.default_advance;
                active_is_italic = top.is_italic;
                active_is_bold = top.is_bold;
                active_ascender = top.ascender;
                active_descender = top.descender;
                active_bidi = top.bidi;
                current_fill_color = top.fill_color;
                current_stroke_color = top.stroke_color;
                current_fill_space = top.fill_space;
                current_stroke_space = top.stroke_space;
                text_render_mode = top.render_mode;
            }
        } else if (op == "cm") {
            if (operands.size() >= 6) {
                bool ok = true;
                float m2[6];
                for (int k = 0; k < 6; ++k) {
                    const PdfToken& t = operands[operands.size() - 6 + k];
                    if (t.type != PdfToken::Type::Number) {
                        ok = false;
                        break;
                    }
                    m2[k] = static_cast<float>(t.number);
                }
                if (ok) {
                    float out[6];
                    matrix_multiply(m2, ctm, out);
                    for (int k = 0; k < 6; ++k) {
                        ctm[k] = out[k];
                    }
                }
            }
        } else if (op == "m") {
            if (operands.size() >= 2) {
                const PdfToken& x = operands[operands.size() - 2];
                const PdfToken& y = operands[operands.size() - 1];
                if (x.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number) {
                    Vec2 p = apply_matrix_to_point(ctm, static_cast<float>(x.number), static_cast<float>(y.number));
                    add_path_point(p);
                    path_current = p;
                    path_subpath_start = p;
                    path_has_current = true;
                }
            }
        } else if (op == "l") {
            if (operands.size() >= 2) {
                const PdfToken& x = operands[operands.size() - 2];
                const PdfToken& y = operands[operands.size() - 1];
                if (x.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number) {
                    Vec2 p = apply_matrix_to_point(ctm, static_cast<float>(x.number), static_cast<float>(y.number));
                    add_path_point(p);
                    path_current = p;
                    path_has_current = true;
                }
            }
        } else if (op == "c") {
            if (operands.size() >= 6) {
                const PdfToken& x1 = operands[operands.size() - 6];
                const PdfToken& y1 = operands[operands.size() - 5];
                const PdfToken& x2 = operands[operands.size() - 4];
                const PdfToken& y2 = operands[operands.size() - 3];
                const PdfToken& x3 = operands[operands.size() - 2];
                const PdfToken& y3 = operands[operands.size() - 1];
                if (x1.type == PdfToken::Type::Number && y1.type == PdfToken::Type::Number &&
                    x2.type == PdfToken::Type::Number && y2.type == PdfToken::Type::Number &&
                    x3.type == PdfToken::Type::Number && y3.type == PdfToken::Type::Number) {
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x1.number), static_cast<float>(y1.number)));
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x2.number), static_cast<float>(y2.number)));
                    Vec2 p3 = apply_matrix_to_point(ctm, static_cast<float>(x3.number), static_cast<float>(y3.number));
                    add_path_point(p3);
                    path_current = p3;
                    path_has_current = true;
                }
            }
        } else if (op == "v") {
            if (operands.size() >= 4) {
                const PdfToken& x2 = operands[operands.size() - 4];
                const PdfToken& y2 = operands[operands.size() - 3];
                const PdfToken& x3 = operands[operands.size() - 2];
                const PdfToken& y3 = operands[operands.size() - 1];
                if (x2.type == PdfToken::Type::Number && y2.type == PdfToken::Type::Number &&
                    x3.type == PdfToken::Type::Number && y3.type == PdfToken::Type::Number) {
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x2.number), static_cast<float>(y2.number)));
                    Vec2 p3 = apply_matrix_to_point(ctm, static_cast<float>(x3.number), static_cast<float>(y3.number));
                    add_path_point(p3);
                    path_current = p3;
                    path_has_current = true;
                }
            }
        } else if (op == "y") {
            if (operands.size() >= 4) {
                const PdfToken& x1 = operands[operands.size() - 4];
                const PdfToken& y1 = operands[operands.size() - 3];
                const PdfToken& x3 = operands[operands.size() - 2];
                const PdfToken& y3 = operands[operands.size() - 1];
                if (x1.type == PdfToken::Type::Number && y1.type == PdfToken::Type::Number &&
                    x3.type == PdfToken::Type::Number && y3.type == PdfToken::Type::Number) {
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x1.number), static_cast<float>(y1.number)));
                    Vec2 p3 = apply_matrix_to_point(ctm, static_cast<float>(x3.number), static_cast<float>(y3.number));
                    add_path_point(p3);
                    path_current = p3;
                    path_has_current = true;
                }
            }
        } else if (op == "h") {
            if (path_has_current) {
                add_path_point(path_subpath_start);
                path_current = path_subpath_start;
            }
        } else if (op == "re") {
            if (operands.size() >= 4) {
                const PdfToken& x = operands[operands.size() - 4];
                const PdfToken& y = operands[operands.size() - 3];
                const PdfToken& w = operands[operands.size() - 2];
                const PdfToken& h = operands[operands.size() - 1];
                if (x.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number &&
                    w.type == PdfToken::Type::Number && h.type == PdfToken::Type::Number) {
                    const float xf = static_cast<float>(x.number);
                    const float yf = static_cast<float>(y.number);
                    const float wf = static_cast<float>(w.number);
                    const float hf = static_cast<float>(h.number);

                    Vec2 p0 = apply_matrix_to_point(ctm, xf, yf);
                    Vec2 p1 = apply_matrix_to_point(ctm, xf + wf, yf);
                    Vec2 p2 = apply_matrix_to_point(ctm, xf + wf, yf + hf);
                    Vec2 p3 = apply_matrix_to_point(ctm, xf, yf + hf);
                    add_path_point(p0);
                    add_path_point(p1);
                    add_path_point(p2);
                    add_path_point(p3);
                    path_current = p0;
                    path_subpath_start = p0;
                    path_has_current = true;
                }
            }
        } else if (op == "W" || op == "W*") {
            pending_clip = true;
        } else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" ||
                   op == "B" || op == "B*" || op == "b" || op == "b*" || op == "n") {
            consume_current_path();
        } else if (op == "Do") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Name) {
                const std::string& xobj_name = operands.back().name;
                auto fit = form_xobject_map.find(xobj_name);
                if (fit != form_xobject_map.end() && !fit->second.stream.empty()) {
                    float form_ctm[6];
                    matrix_multiply(fit->second.matrix.data(), ctm, form_ctm);
                    WinPdfInterpreter::run(fit->second.stream,
                                           extractor,
                                           fit->second.font_unicode_map,
                                           fit->second.font_width_map,
                                           fit->second.font_code_bytes_map,
                                           fit->second.font_codespace_map,
                                           fit->second.font_matrix_map,
                                           fit->second.font_vertical_metrics_map,
                                           fit->second.color_space_map,
                                           fit->second.children ? *fit->second.children : WinFormXObjectMap(),
                                           form_ctm,
                                           recursion_depth + 1,
                                           page_mediabox,
                                           clip_has_box ? &current_clip_box : nullptr,
                                           &current_fill_color,
                                           &current_stroke_color,
                                           &current_fill_space,
                                           &current_stroke_space,
                                           &text_render_mode);
                }
            }
        } else if (op == "BDC") {
            ActualTextState* active = active_actualtext();
            if (active != nullptr && !active->text.empty()) {
                flush_remaining_actualtext(active->text, 0.0f);
            }

            ActualTextState state;
            state.text = decode_actualtext_from_operands(operands);
            actual_text_stack.push_back(std::move(state));
        } else if (op == "BMC") {
            actual_text_stack.push_back(ActualTextState{});
        } else if (op == "DP") {
            ActualTextState* active = active_actualtext();
            if (active != nullptr && !active->text.empty()) {
                flush_remaining_actualtext(active->text, 0.0f);
            }

            std::vector<int> point_actualtext = decode_actualtext_from_operands(operands);
            if (!point_actualtext.empty()) {
                flush_remaining_actualtext(point_actualtext, 0.0f);
            }
        } else if (op == "MP") {
            // Marked-content points have no text payload for this extractor path.
        } else if (op == "EMC") {
            if (!actual_text_stack.empty()) {
                ActualTextState state = std::move(actual_text_stack.back());
                actual_text_stack.pop_back();

                if (!state.text.empty()) {
                    flush_remaining_actualtext(state.text, 0.0f);
                }
            }
        } else if (op == "ET") {
            in_text_object = false;
        } else if (op == "Tf") {
            if (operands.size() >= 2) {
                const PdfToken& font = operands[operands.size() - 2];
                const PdfToken& size = operands[operands.size() - 1];
                if (font.type == PdfToken::Type::Name && size.type == PdfToken::Type::Number) {
                    st.font_name = font.name;
                    st.font_size = static_cast<float>(size.number);
                    active_font_map  = nullptr;
                    active_width_map = nullptr;
                    active_codespace_ranges = nullptr;
                    active_code_bytes = 1;
                    active_default_advance = 0.55f;
                    active_is_italic = false;
                    active_is_bold = false;
                    active_is_serif = true;
                    active_is_mono = false;
                    active_ascender = 0.8f;
                    active_descender = -0.2f;

                    const std::string lower_font = to_lower_ascii(normalize_pdf_font_name(st.font_name));
                    if (lower_font.find("bold") != std::string::npos ||
                        lower_font.find("black") != std::string::npos ||
                        lower_font.find("heavy") != std::string::npos ||
                        lower_font.find("demi") != std::string::npos) {
                        active_is_bold = true;
                    }

                    auto mit = font_matrix_map.find(st.font_name);
                    if (mit != font_matrix_map.end()) {
                        active_is_italic = std::fabs(mit->second[2]) > 0.0001f;
                    }

                    auto cbit = font_code_bytes_map.find(st.font_name);
                    if (cbit != font_code_bytes_map.end() && cbit->second > 0) {
                        active_code_bytes = cbit->second;
                    }

                    auto csit = font_codespace_map.find(st.font_name);
                    if (csit != font_codespace_map.end() && !csit->second->empty()) {
                        active_codespace_ranges = csit->second.get();
                        for (const auto& r : *active_codespace_ranges) {
                            if (r.nbytes > active_code_bytes) {
                                active_code_bytes = r.nbytes;
                            }
                        }
                    }

                    auto fit = font_unicode_map.find(st.font_name);
                    if (fit != font_unicode_map.end()) {
                        active_font_map = fit->second.get();
                    }

                    auto vit = font_vertical_metrics_map.find(st.font_name);
                    if (vit != font_vertical_metrics_map.end()) {
                        active_ascender = vit->second.ascender;
                        active_descender = vit->second.descender;
                        active_is_bold = vit->second.is_bold;
                        active_is_italic = vit->second.is_italic;
                        active_is_serif = vit->second.is_serif;
                        active_is_mono = vit->second.is_mono;
                    }

                    auto wit = font_width_map.find(st.font_name);
                    if (wit != font_width_map.end()) {
                        active_width_map = wit->second.get();
                        auto def_it = wit->second->find(-1);
                        if (def_it != wit->second->end()) {
                            active_default_advance = def_it->second;
                        } else {
                            active_default_advance = 0.55f;
                        }

                        auto italic_it = wit->second->find(-2);
                        if (italic_it != wit->second->end() && italic_it->second > 0.0f) {
                            active_is_italic = true;
                        }

                        auto bold_it = wit->second->find(-3);
                        if (bold_it != wit->second->end() && bold_it->second > 0.0f) {
                            active_is_bold = true;
                        }
                    }

                }
            }
        } else if (op == "Tc") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.char_spacing = static_cast<float>(operands.back().number);
            }
        } else if (op == "Tw") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.word_spacing = static_cast<float>(operands.back().number);
            }
        } else if (op == "Tz") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.h_scale = static_cast<float>(operands.back().number);
            }
        } else if (op == "TL") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.leading = static_cast<float>(operands.back().number);
            }
        } else if (op == "Ts") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.text_rise = static_cast<float>(operands.back().number);
            }
        } else if (op == "Tr") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                int mode = static_cast<int>(std::lround(operands.back().number));
                if (mode < 0) mode = 0;
                if (mode > 7) mode = 7;
                text_render_mode = mode;
            }
        } else if (op == "cs") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Name) {
                current_fill_space = resolve_named_color_space(operands.back().name, current_fill_space);
            }
        } else if (op == "CS") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Name) {
                current_stroke_space = resolve_named_color_space(operands.back().name, current_stroke_space);
            }
        } else if (op == "sc" || op == "scn") {
            apply_color_operands(current_fill_space, operands, current_fill_color);
        } else if (op == "SC" || op == "SCN") {
            apply_color_operands(current_stroke_space, operands, current_stroke_color);
        } else if (op == "g") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                current_fill_color = rgb_from_gray(static_cast<float>(operands.back().number));
                current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
            }
        } else if (op == "G") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                current_stroke_color = rgb_from_gray(static_cast<float>(operands.back().number));
                current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
            }
        } else if (op == "rg") {
            if (operands.size() >= 3) {
                const PdfToken& r = operands[operands.size() - 3];
                const PdfToken& g = operands[operands.size() - 2];
                const PdfToken& b = operands[operands.size() - 1];
                if (r.type == PdfToken::Type::Number && g.type == PdfToken::Type::Number && b.type == PdfToken::Type::Number) {
                    current_fill_color = (to_u8(static_cast<float>(r.number)) << 16) |
                                         (to_u8(static_cast<float>(g.number)) << 8) |
                                         to_u8(static_cast<float>(b.number));
                    current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceRGB, 3);
                }
            }
        } else if (op == "RG") {
            if (operands.size() >= 3) {
                const PdfToken& r = operands[operands.size() - 3];
                const PdfToken& g = operands[operands.size() - 2];
                const PdfToken& b = operands[operands.size() - 1];
                if (r.type == PdfToken::Type::Number && g.type == PdfToken::Type::Number && b.type == PdfToken::Type::Number) {
                    current_stroke_color = (to_u8(static_cast<float>(r.number)) << 16) |
                                           (to_u8(static_cast<float>(g.number)) << 8) |
                                           to_u8(static_cast<float>(b.number));
                    current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceRGB, 3);
                }
            }
        } else if (op == "k") {
            if (operands.size() >= 4) {
                const PdfToken& c = operands[operands.size() - 4];
                const PdfToken& m = operands[operands.size() - 3];
                const PdfToken& y = operands[operands.size() - 2];
                const PdfToken& k = operands[operands.size() - 1];
                if (c.type == PdfToken::Type::Number && m.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number && k.type == PdfToken::Type::Number) {
                    current_fill_color = rgb_from_cmyk(
                        static_cast<float>(c.number),
                        static_cast<float>(m.number),
                        static_cast<float>(y.number),
                        static_cast<float>(k.number));
                    current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
                }
            }
        } else if (op == "K") {
            if (operands.size() >= 4) {
                const PdfToken& c = operands[operands.size() - 4];
                const PdfToken& m = operands[operands.size() - 3];
                const PdfToken& y = operands[operands.size() - 2];
                const PdfToken& k = operands[operands.size() - 1];
                if (c.type == PdfToken::Type::Number && m.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number && k.type == PdfToken::Type::Number) {
                    current_stroke_color = rgb_from_cmyk(
                        static_cast<float>(c.number),
                        static_cast<float>(m.number),
                        static_cast<float>(y.number),
                        static_cast<float>(k.number));
                    current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
                }
            }
        } else if (op == "Tm") {
            if (operands.size() >= 6) {
                bool ok = true;
                float m[6];
                for (int k = 0; k < 6; ++k) {
                    const PdfToken& t = operands[operands.size() - 6 + k];
                    if (t.type != PdfToken::Type::Number) {
                        ok = false;
                        break;
                    }
                    m[k] = static_cast<float>(t.number);
                }
                if (ok) {
                    for (int k = 0; k < 6; ++k) {
                        st.tm[k] = m[k];
                        st.tlm[k] = m[k];
                    }
                }
            }
        } else if (op == "Td") {
            if (operands.size() >= 2) {
                const PdfToken& tx = operands[operands.size() - 2];
                const PdfToken& ty = operands[operands.size() - 1];
                if (tx.type == PdfToken::Type::Number && ty.type == PdfToken::Type::Number) {
                    move_text_position(st, static_cast<float>(tx.number), static_cast<float>(ty.number));
                }
            }
        } else if (op == "TD") {
            if (operands.size() >= 2) {
                const PdfToken& tx = operands[operands.size() - 2];
                const PdfToken& ty = operands[operands.size() - 1];
                if (tx.type == PdfToken::Type::Number && ty.type == PdfToken::Type::Number) {
                    st.leading = -static_cast<float>(ty.number);
                    move_text_position(st, static_cast<float>(tx.number), static_cast<float>(ty.number));
                }
            }
        } else if (op == "T*") {
            move_text_position(st, 0.0f, -st.leading);
        } else if (op == "Tj") {
    if (in_text_object && !operands.empty() && operands.back().type == PdfToken::Type::String) { 
        emit_text(operands.back().bytes);   
    }
} else if (op == "TJ") {
    if (in_text_object && !operands.empty() && operands.back().type == PdfToken::Type::Array) {
        for (const PdfToken& item : operands.back().items) {
            if (item.type == PdfToken::Type::String) {
                emit_text(item.bytes);
            } else if (item.type == PdfToken::Type::Number) {
                float tadj = static_cast<float>(-item.number * st.font_size * 0.001);
                if (st.wmode == 0) {
                    const float tx = tadj * (st.h_scale / 100.0f);
                    st.tm[4] += tx * st.tm[0];
                    st.tm[5] += tx * st.tm[1];
                } else {
                    st.tm[4] += tadj * st.tm[2];
                    st.tm[5] += tadj * st.tm[3];
                }
            }
        }
    }
        } else if (op == "'") {
            move_text_position(st, 0.0f, -st.leading);
            if (in_text_object && !operands.empty() && operands.back().type == PdfToken::Type::String) {
                emit_text(operands.back().bytes);
            }
        } else if (op == "\"") {
            if (operands.size() >= 3) {
                const PdfToken& aw = operands[operands.size() - 3];
                const PdfToken& ac = operands[operands.size() - 2];
                const PdfToken& text = operands[operands.size() - 1];
                if (aw.type == PdfToken::Type::Number && ac.type == PdfToken::Type::Number) {
                    st.word_spacing = static_cast<float>(aw.number);
                    st.char_spacing = static_cast<float>(ac.number);
                    move_text_position(st, 0.0f, -st.leading);
                    if (in_text_object && text.type == PdfToken::Type::String) {
                        emit_text(text.bytes);
                    }
                }
            }
        }

        operands.clear();
    }

    while (!actual_text_stack.empty()) {
        ActualTextState state = std::move(actual_text_stack.back());
        actual_text_stack.pop_back();
        if (!state.text.empty()) {
            flush_remaining_actualtext(state.text, 0.0f);
        }
    }
}


std::shared_ptr<WinPdfDocument> WinPdfDocument::open_from_memory(const std::vector<uint8_t>& data) {
    auto doc = std::make_shared<WinPdfDocument>();
    doc->data_ = data;
    doc->file_view_ = std::string_view(
        reinterpret_cast<const char*>(doc->data_.data()), doc->data_.size());

    doc->parse_xref();

    const std::string_view sv = doc->file_view_;
    if (doc->root_id <= 0) {
        const size_t last_trailer = sv.rfind("trailer");
        if (last_trailer != std::string_view::npos) {
            doc->root_id = parse_ref_id_after_key(
                std::string(sv.substr(last_trailer)), "/Root");
        }
    }
    if (doc->root_id <= 0) {
        doc->root_id = parse_ref_id_after_key(std::string(sv), "/Root");
    }
    if (doc->root_id > 0) {
        doc->find_pages();
    }
    if (doc->page_ids.empty()) {
        doc->build_fallback_pages_from_streams();
    }
    return doc;
}

std::shared_ptr<WinPdfDocument> WinPdfDocument::open(const std::string& path) {
    auto doc = std::make_shared<WinPdfDocument>();

    // ─── ZERO-COPY THẬT SỰ: MappedFile → file_view_ → không vector shim ────────
    doc->mapped_file_ = MappedFile(path);

    if (!doc->mapped_file_.ok()) {
        // Fallback: mmap thất bại (network drive, permission...) → ifstream
#if defined(_WIN32) || defined(_WIN64)
        int size_w = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(size_w, 0);
        ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], size_w);
        wpath.resize(size_w > 0 ? size_w - 1 : 0);
        std::ifstream file(wpath, std::ios::binary | std::ios::ate);
#else
        std::ifstream file(path, std::ios::binary | std::ios::ate);
#endif
        if (!file.is_open()) {
            return nullptr;
        }
        const size_t size = static_cast<size_t>(file.tellg());
        doc->data_.resize(size);
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(doc->data_.data()),
                  static_cast<std::streamsize>(size));
        doc->file_view_ = std::string_view(
            reinterpret_cast<const char*>(doc->data_.data()), size);
    } else {
        // Bình thường: file_view_ trỏ thẳng vào bộ nhớ mmap — ZERO COPY.
        doc->file_view_ = doc->mapped_file_.view();
        // data_ RỖNG — parse_xref / read_obj_from_offset đọc từ file_view_.
    }

    doc->parse_xref();

    const std::string_view sv = doc->file_view_;
    if (doc->root_id <= 0) {
        const size_t last_trailer = sv.rfind("trailer");
        if (last_trailer != std::string_view::npos) {
            doc->root_id = parse_ref_id_after_key(
                std::string(sv.substr(last_trailer)), "/Root");
        }
    }
    if (doc->root_id <= 0) {
        doc->root_id = parse_ref_id_after_key(std::string(sv), "/Root");
    }

    if (doc->root_id > 0) {
        doc->find_pages();
    }

    if (doc->page_ids.empty()) {
        doc->build_fallback_pages_from_streams();
    }
    return doc;
}

void WinPdfDocument::find_pages() {
    page_ids.clear();
    if (root_id <= 0) {
        return;
    }

    WinPdfObject root_obj = read_obj(root_id);
    int pages_id = parse_ref_id_after_key(root_obj.dict, "/Pages");
    if (pages_id > 0) {
        collect_page_nodes(pages_id);
    }
}

void WinPdfDocument::build_fallback_pages_from_streams() {
    fallback_content_ids.clear();

    for (const auto& kv : xref) {
        int obj_id = kv.first;
        WinPdfObject obj = read_obj(obj_id);
        if (!obj.is_stream || obj.stream.empty()) {
            continue;
        }

        std::vector<uint8_t> bytes = decode_stream_data(obj.stream, obj.dict, this);
        if (bytes.empty()) {
            bytes = obj.stream;
        }

        if (looks_like_text_content_stream(bytes)) {
            fallback_content_ids.push_back(obj_id);
        }
    }
}

void WinPdfDocument::collect_page_nodes(int node_id) {
    if (node_id <= 0) {
        return;
    }

    WinPdfObject node = read_obj(node_id);
    if (node.dict.empty()) {
        return;
    }

    if (is_page_object(node.dict)) {
        page_ids.push_back(node_id);
        return;
    }

    std::vector<int> kids = parse_ref_array_after_key(node.dict, "/Kids");
    for (int kid : kids) {
        collect_page_nodes(kid);
    }
}

int WinPdfDocument::count_pages() const {
    if (!page_ids.empty()) {
        return static_cast<int>(page_ids.size());
    }
    return static_cast<int>(fallback_content_ids.size());
}

std::vector<uint8_t> WinPdfDocument::get_page_content(int page_idx) {
    if (page_idx < 0) {
        return {};
    }

    if (!page_ids.empty()) {
        if (page_idx >= static_cast<int>(page_ids.size())) {
            return {};
        }

        WinPdfObject page_obj = read_obj(page_ids[page_idx]);
        std::vector<int> content_refs = parse_ref_array_after_key(page_obj.dict, "/Contents");
        if (content_refs.empty()) {
            int single_ref = parse_ref_id_after_key(page_obj.dict, "/Contents");
            if (single_ref > 0) {
                content_refs.push_back(single_ref);
            }
        }

        auto parse_array_refs = [&](const std::string& text) {
            std::vector<int> out;
            size_t arr_start = text.find('[');
            size_t arr_end = text.find(']', arr_start == std::string::npos ? 0 : arr_start);
            if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start + 1) {
                return out;
            }

            const std::string refs_text = text.substr(arr_start + 1, arr_end - arr_start - 1);
            const char* p = refs_text.c_str();
            while (*p) {
                int id = 0;
                int gen = 0;
                char r = 0;
                int consumed = 0;
                if (!wz_parse_xref_line_3_fitz(p, id, gen, r, consumed) || consumed <= 0) {
                    break;
                }
                if (r == 'R' && id > 0) {
                    out.push_back(id);
                }
                p += consumed;
            }

            return out;
        };

        std::vector<int> resolved_content_refs;
        std::set<int> content_guard;
        std::function<void(int)> resolve_content_ref = [&](int obj_id) {
            if (obj_id <= 0 || content_guard.find(obj_id) != content_guard.end()) {
                return;
            }
            content_guard.insert(obj_id);

            WinPdfObject content = read_obj(obj_id);
            if (content.is_stream) {
                resolved_content_refs.push_back(obj_id);
                return;
            }

            std::vector<int> nested_refs = parse_array_refs(content.body);
            if (nested_refs.empty()) {
                nested_refs = parse_array_refs(content.dict);
            }
            for (int nested_id : nested_refs) {
                resolve_content_ref(nested_id);
            }
        };

        for (int obj_id : content_refs) {
            resolve_content_ref(obj_id);
        }

        std::vector<uint8_t> merged;
        for (int obj_id : resolved_content_refs) {
            WinPdfObject content = read_obj(obj_id);
            if (!content.is_stream || content.stream.empty()) {
                continue;
            }

            std::vector<uint8_t> bytes = decode_stream_data(content.stream, content.dict, this);
            if (bytes.empty()) {
                bytes = content.stream;
            }

            merged.insert(merged.end(), bytes.begin(), bytes.end());
            merged.push_back('\n');
        }

        return merged;
    }

    if (page_idx >= static_cast<int>(fallback_content_ids.size())) {
        return {};
    }

    WinPdfObject content = read_obj(fallback_content_ids[page_idx]);
    if (!content.is_stream || content.stream.empty()) {
        return {};
    }

    std::vector<uint8_t> decoded = decode_stream_data(content.stream, content.dict, this);
    if (!decoded.empty()) {
        return decoded;
    }
    return content.stream;
}


std::vector<uint8_t> WinPdfDocument::save_multiple_pages_content_incremental_to_bytes(
        const std::map<int, std::vector<uint8_t>>& pages_streams) {
    if (pages_streams.empty() || page_ids.empty()) {
        return {};
    }
    if (file_view_.empty()) {
        return {};
    }

    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
            char* end_ptr = nullptr;
            long v = wz_strtol_fitz(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) return v;
            if (pos == 0) break;
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    auto replace_contents_entry = [&](const std::string& dict, const std::string& new_ref) {
        auto append_at_end = [&](const std::string& src) {
            std::string out = src;
            size_t dict_end = out.rfind(">>");
            if (dict_end == std::string::npos) return out;
            out.insert(dict_end, " /Contents " + new_ref + " ");
            return out;
        };

        size_t key_pos = std::string::npos;
        size_t search_pos = 0;
        while (search_pos < dict.size()) {
            size_t found = dict.find("/Contents", search_pos);
            if (found == std::string::npos) break;
            const size_t after = found + 9;
            const char c = (after < dict.size()) ? dict[after] : ' ';
            if (after >= dict.size() || std::isspace(static_cast<unsigned char>(c)) ||
                c == '[' || c == '<' || c == '/' || std::isdigit(static_cast<unsigned char>(c))) {
                key_pos = found;
                break;
            }
            search_pos = after;
        }

        if (key_pos == std::string::npos) return append_at_end(dict);

        size_t value_start = key_pos + 9;
        while (value_start < dict.size() && std::isspace(static_cast<unsigned char>(dict[value_start]))) ++value_start;
        size_t value_end = value_start;

        if (value_start < dict.size() && dict[value_start] == '[') {
            int depth = 0;
            for (size_t i = value_start; i < dict.size(); ++i) {
                if (dict[i] == '[') ++depth;
                else if (dict[i] == ']') {
                    --depth;
                    if (depth == 0) { value_end = i + 1; break; }
                }
            }
        }

        if (value_end == value_start) {
            const char* p = dict.c_str() + value_start;
            char* e1 = nullptr;
            long v1 = wz_strtol_fitz(p, &e1, 10);
            if (e1 != p && v1 > 0) {
                char* e2 = nullptr;
                long v2 = wz_strtol_fitz(e1, &e2, 10);
                if (e2 != e1 && v2 >= 0) {
                    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) ++e2;
                    if (*e2 == 'R') {
                        ++e2;
                        value_end = static_cast<size_t>(e2 - dict.c_str());
                    }
                }
            }
        }

        if (value_end == value_start) {
            while (value_end < dict.size() && !std::isspace(static_cast<unsigned char>(dict[value_end])) &&
                   dict[value_end] != '/' && dict[value_end] != '>') {
                ++value_end;
            }
        }

        if (value_end <= value_start) return append_at_end(dict);

        std::string out = dict;
        out.replace(value_start, value_end - value_start, new_ref);
        return out;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) root_id = parse_ref_id_after_key(raw, "/Root");
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) return {};

    build_objstm_index();

    int max_obj_id = 0;
    for (const auto& kv : xref) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (const auto& kv : objstm_lookup) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (int pid : page_ids) max_obj_id = (std::max)(max_obj_id, pid);
    max_obj_id = (std::max)(max_obj_id, root_id);

    std::string out;
    out.reserve(raw.size() + pages_streams.size() * 500000);
    out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') out.push_back('\n');

    struct XrefEntry { int id; size_t offset; };
    std::vector<XrefEntry> changed;

    int current_new_id = max_obj_id + 1;

    for (const auto& kv : pages_streams) {
        int page_idx = kv.first;
        const auto& decoded_stream = kv.second;

        if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size())) continue;
        WinPdfObject page_obj = read_obj(page_ids[page_idx]);
        if (page_obj.id <= 0 || page_obj.dict.empty()) continue;

        const int new_stream_id = current_new_id++;
        const std::string new_ref = std::to_string(new_stream_id) + " 0 R";
        const std::string updated_page_dict = replace_contents_entry(page_obj.dict, new_ref);

        const size_t stream_offset = out.size();
        out += std::to_string(new_stream_id) + " 0 obj\n<< /Length " + std::to_string(decoded_stream.size()) + " >>\nstream\n";
        if (!decoded_stream.empty()) out.append(reinterpret_cast<const char*>(decoded_stream.data()), decoded_stream.size());
        if (decoded_stream.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endstream\nendobj\n";

        const size_t page_offset = out.size();
        out += std::to_string(page_obj.id) + " " + std::to_string(page_obj.gen) + " obj\n";
        out += updated_page_dict;
        if (updated_page_dict.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endobj\n";

        changed.push_back({page_obj.id, page_offset});
        changed.push_back({new_stream_id, stream_offset});
    }

    if (changed.empty()) return {};

    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) { return a.id < b.id; });

    const size_t xref_offset = out.size();
    out += "xref\n";
    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) ++j;
        out += std::to_string(changed[i].id) + " " + std::to_string(j - i) + "\n";
        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n", static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    const int size_value = current_new_id;
    out += "trailer\n<< /Size " + std::to_string(size_value) + " /Root " + std::to_string(root_id) +
           " 0 R /Prev " + std::to_string(prev_startxref) + " >>\nstartxref\n" +
           std::to_string(xref_offset) + "\n%%EOF\n";

    return std::vector<uint8_t>(out.begin(), out.end());
}


bool WinPdfDocument::save_multiple_pages_content_incremental(
        const std::map<int, std::vector<uint8_t>>& pages_streams,
        const std::string& output_path) {
    if (pages_streams.empty() || page_ids.empty()) {
        return false;
    }
    if (output_path.empty() || file_view_.empty()) {
        return false;
    }

    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
            char* end_ptr = nullptr;
            long v = wz_strtol_fitz(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) return v;
            if (pos == 0) break;
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    auto replace_contents_entry = [&](const std::string& dict, const std::string& new_ref) {
        auto append_at_end = [&](const std::string& src) {
            std::string out = src;
            size_t dict_end = out.rfind(">>");
            if (dict_end == std::string::npos) return out;
            out.insert(dict_end, " /Contents " + new_ref + " ");
            return out;
        };

        size_t key_pos = std::string::npos;
        size_t search_pos = 0;
        while (search_pos < dict.size()) {
            size_t found = dict.find("/Contents", search_pos);
            if (found == std::string::npos) break;
            const size_t after = found + 9;
            const char c = (after < dict.size()) ? dict[after] : ' ';
            if (after >= dict.size() || std::isspace(static_cast<unsigned char>(c)) ||
                c == '[' || c == '<' || c == '/' || std::isdigit(static_cast<unsigned char>(c))) {
                key_pos = found;
                break;
            }
            search_pos = after;
        }

        if (key_pos == std::string::npos) return append_at_end(dict);

        size_t value_start = key_pos + 9;
        while (value_start < dict.size() && std::isspace(static_cast<unsigned char>(dict[value_start]))) ++value_start;
        size_t value_end = value_start;

        if (value_start < dict.size() && dict[value_start] == '[') {
            int depth = 0;
            for (size_t i = value_start; i < dict.size(); ++i) {
                if (dict[i] == '[') ++depth;
                else if (dict[i] == ']') {
                    --depth;
                    if (depth == 0) { value_end = i + 1; break; }
                }
            }
        }

        if (value_end == value_start) {
            const char* p = dict.c_str() + value_start;
            char* e1 = nullptr;
            long v1 = wz_strtol_fitz(p, &e1, 10);
            if (e1 != p && v1 > 0) {
                char* e2 = nullptr;
                long v2 = wz_strtol_fitz(e1, &e2, 10);
                if (e2 != e1 && v2 >= 0) {
                    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) ++e2;
                    if (*e2 == 'R') {
                        ++e2;
                        value_end = static_cast<size_t>(e2 - dict.c_str());
                    }
                }
            }
        }

        if (value_end == value_start) {
            while (value_end < dict.size() && !std::isspace(static_cast<unsigned char>(dict[value_end])) &&
                   dict[value_end] != '/' && dict[value_end] != '>') {
                ++value_end;
            }
        }

        if (value_end <= value_start) return append_at_end(dict);

        std::string out = dict;
        out.replace(value_start, value_end - value_start, new_ref);
        return out;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) root_id = parse_ref_id_after_key(raw, "/Root");
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) return false;

    build_objstm_index();

    int max_obj_id = 0;
    for (const auto& kv : xref) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (const auto& kv : objstm_lookup) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (int pid : page_ids) max_obj_id = (std::max)(max_obj_id, pid);
    max_obj_id = (std::max)(max_obj_id, root_id);

    std::string out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') out.push_back('\n');

    struct XrefEntry { int id; size_t offset; };
    std::vector<XrefEntry> changed;

    int current_new_id = max_obj_id + 1;

    for (const auto& kv : pages_streams) {
        int page_idx = kv.first;
        const auto& decoded_stream = kv.second;

        if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size())) continue;
        WinPdfObject page_obj = read_obj(page_ids[page_idx]);
        if (page_obj.id <= 0 || page_obj.dict.empty()) continue;

        const int new_stream_id = current_new_id++;
        const std::string new_ref = std::to_string(new_stream_id) + " 0 R";
        const std::string updated_page_dict = replace_contents_entry(page_obj.dict, new_ref);

        const size_t stream_offset = out.size();
        out += std::to_string(new_stream_id) + " 0 obj\n<< /Length " + std::to_string(decoded_stream.size()) + " >>\nstream\n";
        if (!decoded_stream.empty()) out.append(reinterpret_cast<const char*>(decoded_stream.data()), decoded_stream.size());
        if (decoded_stream.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endstream\nendobj\n";

        const size_t page_offset = out.size();
        out += std::to_string(page_obj.id) + " " + std::to_string(page_obj.gen) + " obj\n";
        out += updated_page_dict;
        if (updated_page_dict.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endobj\n";

        changed.push_back({page_obj.id, page_offset});
        changed.push_back({new_stream_id, stream_offset});
    }

    if (changed.empty()) return false;

    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) { return a.id < b.id; });

    const size_t xref_offset = out.size();
    out += "xref\n";
    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) ++j;
        out += std::to_string(changed[i].id) + " " + std::to_string(j - i) + "\n";
        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n", static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    const int size_value = current_new_id;
    out += "trailer\n<< /Size " + std::to_string(size_value) + " /Root " + std::to_string(root_id) +
           " 0 R /Prev " + std::to_string(prev_startxref) + " >>\nstartxref\n" +
           std::to_string(xref_offset) + "\n%%EOF\n";

    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file.good();
}

bool WinPdfDocument::save_page_content_incremental(
        int page_idx,
        const std::vector<uint8_t>& decoded_stream,
        const std::string& output_path) {
    if (page_idx < 0 || page_ids.empty() || page_idx >= static_cast<int>(page_ids.size())) {
        return false;
    }
    if (output_path.empty() || file_view_.empty()) {
        return false;
    }

    WinPdfObject page_obj = read_obj(page_ids[page_idx]);
    if (page_obj.id <= 0 || page_obj.dict.empty()) {
        return false;
    }

    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) {
                ++p;
            }

            char* end_ptr = nullptr;
            long v = wz_strtol_fitz(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) {
                return v;
            }

            if (pos == 0) {
                break;
            }
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    auto replace_contents_entry = [&](const std::string& dict, const std::string& new_ref) {
        auto append_at_end = [&](const std::string& src) {
            std::string out = src;
            size_t dict_end = out.rfind(">>");
            if (dict_end == std::string::npos) {
                return out;
            }
            out.insert(dict_end, " /Contents " + new_ref + " ");
            return out;
        };

        size_t key_pos = std::string::npos;
        size_t search_pos = 0;
        while (search_pos < dict.size()) {
            size_t found = dict.find("/Contents", search_pos);
            if (found == std::string::npos) {
                break;
            }

            const size_t after = found + 9;
            const char c = (after < dict.size()) ? dict[after] : ' ';
            if (after >= dict.size() || std::isspace(static_cast<unsigned char>(c)) ||
                c == '[' || c == '<' || c == '/' || std::isdigit(static_cast<unsigned char>(c))) {
                key_pos = found;
                break;
            }
            search_pos = after;
        }

        if (key_pos == std::string::npos) {
            return append_at_end(dict);
        }

        size_t value_start = key_pos + 9;
        while (value_start < dict.size() && std::isspace(static_cast<unsigned char>(dict[value_start]))) {
            ++value_start;
        }

        size_t value_end = value_start;

        if (value_start < dict.size() && dict[value_start] == '[') {
            int depth = 0;
            for (size_t i = value_start; i < dict.size(); ++i) {
                if (dict[i] == '[') {
                    ++depth;
                } else if (dict[i] == ']') {
                    --depth;
                    if (depth == 0) {
                        value_end = i + 1;
                        break;
                    }
                }
            }
        }

        if (value_end == value_start) {
            const char* p = dict.c_str() + value_start;
            char* e1 = nullptr;
            long v1 = wz_strtol_fitz(p, &e1, 10);
            if (e1 != p && v1 > 0) {
                char* e2 = nullptr;
                long v2 = wz_strtol_fitz(e1, &e2, 10);
                if (e2 != e1 && v2 >= 0) {
                    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) {
                        ++e2;
                    }
                    if (*e2 == 'R') {
                        ++e2;
                        value_end = static_cast<size_t>(e2 - dict.c_str());
                    }
                }
            }
        }

        if (value_end == value_start) {
            while (value_end < dict.size() &&
                   !std::isspace(static_cast<unsigned char>(dict[value_end])) &&
                   dict[value_end] != '/' && dict[value_end] != '>') {
                ++value_end;
            }
        }

        if (value_end <= value_start) {
            return append_at_end(dict);
        }

        std::string out = dict;
        out.replace(value_start, value_end - value_start, new_ref);
        return out;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) {
            root_id = parse_ref_id_after_key(raw, "/Root");
        }
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) {
        return false;
    }

    build_objstm_index();

    int max_obj_id = 0;
    for (const auto& kv : xref) {
        max_obj_id = (std::max)(max_obj_id, kv.first);
    }
    for (const auto& kv : objstm_lookup) {
        max_obj_id = (std::max)(max_obj_id, kv.first);
    }
    for (int pid : page_ids) {
        max_obj_id = (std::max)(max_obj_id, pid);
    }
    max_obj_id = (std::max)(max_obj_id, root_id);

    const int new_stream_id = max_obj_id + 1;
    const std::string new_ref = std::to_string(new_stream_id) + " 0 R";
    const std::string updated_page_dict = replace_contents_entry(page_obj.dict, new_ref);

    std::string out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') {
        out.push_back('\n');
    }

    const size_t stream_offset = out.size();
    out += std::to_string(new_stream_id);
    out += " 0 obj\n";
    out += "<< /Length ";
    out += std::to_string(decoded_stream.size());
    out += " >>\nstream\n";
    if (!decoded_stream.empty()) {
        out.append(reinterpret_cast<const char*>(decoded_stream.data()), decoded_stream.size());
    }
    if (decoded_stream.empty() || (out.back() != '\n' && out.back() != '\r')) {
        out.push_back('\n');
    }
    out += "endstream\nendobj\n";

    const size_t page_offset = out.size();
    out += std::to_string(page_obj.id);
    out += " ";
    out += std::to_string(page_obj.gen);
    out += " obj\n";
    out += updated_page_dict;
    if (updated_page_dict.empty() || (out.back() != '\n' && out.back() != '\r')) {
        out.push_back('\n');
    }
    out += "endobj\n";

    struct XrefEntry {
        int id;
        size_t offset;
    };

    std::vector<XrefEntry> changed = {
        {page_obj.id, page_offset},
        {new_stream_id, stream_offset},
    };
    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) {
        return a.id < b.id;
    });

    const size_t xref_offset = out.size();
    out += "xref\n";

    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) {
            ++j;
        }

        out += std::to_string(changed[i].id);
        out += " ";
        out += std::to_string(j - i);
        out += "\n";

        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                          static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    const int size_value = (std::max)(new_stream_id, max_obj_id) + 1;
    out += "trailer\n<< /Size ";
    out += std::to_string(size_value);
    out += " /Root ";
    out += std::to_string(root_id);
    out += " 0 R /Prev ";
    out += std::to_string(prev_startxref);
    out += " >>\n";
    out += "startxref\n";
    out += std::to_string(xref_offset);
    out += "\n%%EOF\n";

    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file.good();
}

WinFontUnicodeMap WinPdfDocument::get_page_font_unicode_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontUnicodeMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

#ifdef WINEXTRACT_USE_FREETYPE
    struct CidToGidMapData {
        bool has_map = false;
        bool identity = false;
        std::vector<uint16_t> values;
    };

    auto resolve_type0_descendant_font = [&](const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj) -> bool {
        descendant_font_obj = {};
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") {
            return false;
        }

        std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
        if (descendant_ids.empty()) {
            int single_descendant = parse_ref_id_after_key(font_obj.dict, "/DescendantFonts");
            if (single_descendant > 0) {
                descendant_ids.push_back(single_descendant);
            }
        }
        if (descendant_ids.empty() || descendant_ids.front() <= 0) {
            return false;
        }

        descendant_font_obj = read_obj(descendant_ids.front());
        return descendant_font_obj.id > 0 || !descendant_font_obj.dict.empty() || !descendant_font_obj.body.empty();
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        WinPdfObject descendant_font_obj;
        if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return read_descriptor_from_dict(descendant_font_obj.dict);
        }

        return false;
    };

    auto load_cid_to_gid_map = [&](const WinPdfObject& font_obj) -> CidToGidMapData {
        CidToGidMapData cid_to_gid;

        WinPdfObject descendant_font_obj;
        if (!resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return cid_to_gid;
        }

        const std::string direct_map_name = parse_name_value_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (direct_map_name == "Identity") {
            cid_to_gid.has_map = true;
            cid_to_gid.identity = true;
            return cid_to_gid;
        }

        const int map_ref = parse_ref_id_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (map_ref <= 0) {
            if (direct_map_name.empty()) {
                cid_to_gid.has_map = true;
                cid_to_gid.identity = true;
            }
            return cid_to_gid;
        }

        WinPdfObject map_obj = read_obj(map_ref);
        if (!map_obj.is_stream || map_obj.stream.empty()) {
            return cid_to_gid;
        }

        std::vector<uint8_t> decoded = decode_stream_data(map_obj.stream, map_obj.dict, this);
        if (decoded.empty()) {
            decoded = map_obj.stream;
        }
        if (decoded.size() < 2) {
            return cid_to_gid;
        }

        cid_to_gid.values.reserve(decoded.size() / 2);
        for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
            const uint16_t gid = static_cast<uint16_t>((static_cast<uint16_t>(decoded[i]) << 8) | decoded[i + 1]);
            cid_to_gid.values.push_back(gid);
        }

        cid_to_gid.has_map = !cid_to_gid.values.empty();
        return cid_to_gid;
    };

    auto fill_missing_unicode_from_freetype = [&](const WinPdfObject& font_obj,
                                                  std::unordered_map<int, std::vector<int>>& unicode_map,
                                                  const std::map<int, std::string>& diff_names) {
        FT_Library library = get_freetype_library();
        if (!library) {
            return;
        }

        const bool is_type0_subtype = parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0";
        const CidToGidMapData cid_to_gid = load_cid_to_gid_map(font_obj);

        std::string descriptor_dict;
        resolve_font_descriptor_dict(font_obj, descriptor_dict);

        std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        if (base_font_name.empty() && is_type0_subtype) {
            WinPdfObject descendant_font_obj;
            if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
                base_font_name = parse_name_value_after_key(descendant_font_obj.dict, "/BaseFont");
            }
        }
        base_font_name = normalize_pdf_font_name(base_font_name);

        FT_Face face = nullptr;
        std::vector<uint8_t> font_bytes;

        if (!descriptor_dict.empty()) {
            int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

            if (font_file_ref > 0) {
                WinPdfObject font_file_obj = read_obj(font_file_ref);
                if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                    font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                    if (font_bytes.empty()) {
                        font_bytes = font_file_obj.stream;
                    }
                    if (!font_bytes.empty()) {
                        if (FT_New_Memory_Face(library,
                                               reinterpret_cast<const FT_Byte*>(font_bytes.data()),
                                               static_cast<FT_Long>(font_bytes.size()),
                                               0,
                                               &face) != 0) {
                            face = nullptr;
                        }
                    }
                }
            }
        }

        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    break;
                }
            }
        }

        if (!face) {
            return;
        }

        std::unordered_map<unsigned int, int> gid_to_unicode;

        auto collect_gid_unicode = [&]() {
            if (face->charmap == nullptr) {
                return;
            }

            FT_UInt gid = 0;
            FT_ULong charcode = FT_Get_First_Char(face, &gid);
            while (gid != 0) {
                if (charcode > 0 && charcode <= 0x10FFFFUL && gid_to_unicode.find(gid) == gid_to_unicode.end()) {
                    gid_to_unicode[gid] = static_cast<int>(charcode);
                }
                charcode = FT_Get_Next_Char(face, charcode, &gid);
            }

            // [FIX Identity-H]: Nếu vẫn thiếu, thử dùng Glyph Names
            if (FT_HAS_GLYPH_NAMES(face)) {
                for (FT_UInt g = 0; g < (FT_UInt)face->num_glyphs; ++g) {
                    if (gid_to_unicode.find(g) == gid_to_unicode.end()) {
                        char gname[64];
                        if (FT_Get_Glyph_Name(face, g, gname, sizeof(gname)) == 0) {
                            int cp = glyph_name_to_unicode_fitz(std::string(gname));
                            if (cp > 0) {
                                gid_to_unicode[g] = cp;
                            }
                        }
                    }
                }
            }
        };

        FT_CharMap saved_charmap = face->charmap;
        if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
            collect_gid_unicode();
        }
        if (face->num_charmaps > 0 && face->charmaps != nullptr) {
            for (int ci = 0; ci < face->num_charmaps; ++ci) {
                FT_CharMap cmap = face->charmaps[ci];
                if (cmap == nullptr || cmap == face->charmap) {
                    continue;
                }
                if (FT_Set_Charmap(face, cmap) != 0) {
                    continue;
                }
                collect_gid_unicode();
            }
        }
        if (saved_charmap != nullptr && face->charmap != saved_charmap) {
            FT_Set_Charmap(face, saved_charmap);
        }

        auto lookup_gid_for_code = [&](int code) -> FT_UInt {
            if (code < 0) {
                return 0;
            }

            if (is_type0_subtype) {
                if (cid_to_gid.has_map) {
                    if (cid_to_gid.identity) {
                        const FT_UInt gid = static_cast<FT_UInt>(code);
                        if (face->num_glyphs <= 0 || gid < static_cast<FT_UInt>(face->num_glyphs)) {
                            return gid;
                        }
                        return 0;
                    }
                    if (static_cast<size_t>(code) < cid_to_gid.values.size()) {
                        const FT_UInt gid = static_cast<FT_UInt>(cid_to_gid.values[static_cast<size_t>(code)]);
                        return gid;
                    }
                }
                return 0;
            }

            FT_UInt gid = 0;
            auto diff_it = diff_names.find(code);
            if (diff_it != diff_names.end() && FT_HAS_GLYPH_NAMES(face)) {
                gid = FT_Get_Name_Index(face, const_cast<FT_String*>(diff_it->second.c_str()));
            }

            if (gid == 0) {
                gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
            }
            if (gid != 0) {
                return gid;
            }

            FT_CharMap saved_charmap = face->charmap;
            if (face->num_charmaps > 0 && face->charmaps != nullptr) {
                for (int ci = 0; ci < face->num_charmaps; ++ci) {
                    FT_CharMap cmap = face->charmaps[ci];
                    if (cmap == nullptr || cmap == face->charmap) {
                        continue;
                    }
                    if (FT_Set_Charmap(face, cmap) != 0) {
                        continue;
                    }
                    gid = 0;
                    if (diff_it != diff_names.end() && FT_HAS_GLYPH_NAMES(face)) {
                        gid = FT_Get_Name_Index(face, const_cast<FT_String*>(diff_it->second.c_str()));
                    }
                    if (gid == 0) {
                        gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
                    }
                    if (gid != 0) {
                        break;
                    }
                }
            }

            if (saved_charmap != nullptr && face->charmap != saved_charmap) {
                FT_Set_Charmap(face, saved_charmap);
            }

            return gid;
        };

        int max_code = 255;
        if (is_type0_subtype) {
            max_code = 65535;
            if (cid_to_gid.has_map && !cid_to_gid.identity && !cid_to_gid.values.empty()) {
                const size_t capped_size = (std::min)(cid_to_gid.values.size(), static_cast<size_t>(65536));
                if (capped_size > 0) {
                    max_code = static_cast<int>(capped_size - 1);
                }
            }
        }

        // [Micro-OCR]: Xóa tối ưu hóa thoát sớm ở đây để đảm bảo các font Identity-H bị thiếu ToUnicode
        // vẫn chạy xuống vòng lặp bên dưới để được quét OCR.

        int consecutive_ocr_failures = 0;

        for (int code = 0; code <= max_code; ++code) {
            FT_UInt gid = lookup_gid_for_code(code);
            if (gid == 0) {
                continue;
            }

            auto it_check = unicode_map.find(code);
            bool is_missing_in_pdf_cmap = (it_check == unicode_map.end() || it_check->second.empty() || 
                                          (it_check->second.size() == 1 && (it_check->second[0] == 0xFFFD || it_check->second[0] == 0)));
            if (!is_missing_in_pdf_cmap) {
                // PDF's ToUnicode table already provides a valid mapping. Do nothing.
                continue;
            }

            int cp = 0;
            auto unicode_it = gid_to_unicode.find(gid);
            if (unicode_it != gid_to_unicode.end()) {
                cp = unicode_it->second;
            }

            auto is_invalid_cp = [](int c) {
                return c <= 0 || c > 0x10FFFF || c == 0xFFFD || (c < 32 && c != 9 && c != 10 && c != 13);
            };

            if (is_invalid_cp(cp) && FT_HAS_GLYPH_NAMES(face)) {
                char glyph_name[256] = {0};
                if (FT_Get_Glyph_Name(face, gid, glyph_name, static_cast<FT_UInt>(sizeof(glyph_name))) == 0 && glyph_name[0] != '\0') {
                    cp = glyph_name_to_unicode_fitz(std::string(glyph_name));
                }
            }

            if (is_invalid_cp(cp)) {
                if (apply_heuristic_vni_tcvn3(base_font_name, code, cp)) {
                    // handled
                } else if (consecutive_ocr_failures < 20) {
                    std::vector<int> ocr_cps = run_micro_ocr_on_glyph(face, gid);
                    if (!ocr_cps.empty()) {
                        unicode_map[code] = ocr_cps;
                        cp = ocr_cps.front(); // just for flow below, though map is already updated
                        consecutive_ocr_failures = 0; // Reset counter on success!
                    } else {
                        consecutive_ocr_failures++; // Increment on failure
                    }
                }
            }

            if (cp > 0 && cp <= 0x10FFFF) {
                auto it = unicode_map.find(code);
                bool should_update = false;
                if (it == unicode_map.end() || it->second.empty()) {
                    should_update = true;
                } else if (it->second.size() == 1 && (it->second[0] == 0xFFFD || it->second[0] == 0)) {
                    should_update = true;
                }
                if (should_update) {
                    unicode_map[code] = {cp};
                }
            }
        }

        if (face) {
            FT_Done_Face(face);
        }
    };
#endif

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_unicode_maps.count(font_obj_id)) {
            out[it.first] = cached_unicode_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        const std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
        const bool is_type0_subtype = (subtype == "Type0");
        const bool is_type3_subtype = is_type3_font_subtype_fitz(subtype);
        std::unordered_map<int, std::vector<int>> cmap;
        std::string cid_collection;
        int cmap_ref = parse_ref_id_after_key(font_obj.dict, "/ToUnicode");
        if (cmap_ref > 0) {
            WinPdfObject cmap_obj = read_obj(cmap_ref);
            if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                if (cmap_stream.empty()) {
                    cmap_stream = cmap_obj.stream;
                }

                cmap = parse_tounicode_cmap_fitz(cmap_stream);
            }
        }

        if (cmap.empty()) {
            std::string to_unicode_name = parse_name_value_after_key(font_obj.dict, "/ToUnicode");
            if (!to_unicode_name.empty()) {
                const auto& named_cmap = load_system_unicode_cmap_by_name_fitz(to_unicode_name);
                if (!named_cmap.empty()) {
                    cmap = named_cmap;
                }
            }
        }

        for (auto it_cmap = cmap.begin(); it_cmap != cmap.end(); ) {
            bool has_invalid = false;
            for (int& ucs : it_cmap->second) {
                // [FIX]: Remap common WinAnsi characters in the 128-159 range
                // which are often incorrectly used in ToUnicode maps.
                if (ucs >= 128 && ucs <= 159) {
                    static const unsigned short win_ansi_to_unicode[32] = {
                        0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
                        0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
                    };
                    unsigned short mapped = win_ansi_to_unicode[ucs - 128];
                    if (mapped != 0) {
                        ucs = mapped;
                    }
                }

                if (ucs >= 0 && ucs < 32 && ucs != 9 && ucs != 10 && ucs != 13) {
                    has_invalid = true;
                    break;
                }
            }
            if (has_invalid) {
                it_cmap = cmap.erase(it_cmap);
            } else {
                ++it_cmap;
            }
        }

        if (is_type0_subtype && cmap.empty()) {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());

                std::string cid_system_info_dict;
                int cid_system_info_ref = parse_ref_id_after_key(cid_font_obj.dict, "/CIDSystemInfo");
                if (cid_system_info_ref > 0) {
                    WinPdfObject cid_system_info_obj = read_obj(cid_system_info_ref);
                    cid_system_info_dict = cid_system_info_obj.dict;
                }
                if (cid_system_info_dict.empty()) {
                    extract_inline_dict_after_key(cid_font_obj.dict, "/CIDSystemInfo", cid_system_info_dict);
                }

                cid_collection = build_cid_collection_name_from_cidsysteminfo_fitz(cid_system_info_dict);
                if (!cid_collection.empty()) {
                    const auto& cid_fallback = load_collection_unicode_cmap_fitz(cid_collection);
                    if (!cid_fallback.empty()) {
                        cmap = cid_fallback;
                    }
                }
            }
        }

        std::string encoding_dict;
        std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
        if (encoding_name.empty()) {
            int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
            if (encoding_ref > 0) {
                WinPdfObject enc_obj = read_obj(encoding_ref);
                encoding_dict = enc_obj.dict;
                encoding_name = parse_name_value_after_key(enc_obj.dict, "/BaseEncoding");
                if (encoding_name.empty()) {
                    encoding_name = parse_name_value_after_key(enc_obj.body, "/BaseEncoding");
                }
            }
        }
        if (encoding_dict.empty()) {
            extract_inline_dict_after_key(font_obj.dict, "/Encoding", encoding_dict);
            if (!encoding_dict.empty() && encoding_name.empty()) {
                encoding_name = parse_name_value_after_key(encoding_dict, "/BaseEncoding");
            }
        }

        std::map<int, std::string> diff_names;
        if (!encoding_dict.empty()) {
            std::map<int, int> dummy;
            apply_differences_to_map(encoding_dict, dummy, &diff_names);
        }

        if (!is_type0_subtype) {
            std::map<int, int> fallback = build_encoding_map(encoding_name);
            if (fallback.empty()) {
                fallback = make_identity_encoding_map();
            }

            if (!encoding_dict.empty()) {
                apply_differences_to_map(encoding_dict, fallback);
            }

            if (!fallback.empty()) {
                for (const auto& kv : fallback) {
                    if (cmap.find(kv.first) == cmap.end()) {
                        cmap[kv.first] = {kv.second};
                    }
                }
            }
        }

#ifdef WINEXTRACT_USE_FREETYPE
        // Run FreeType fallback to fill any gaps, even if the font has a partial ToUnicode cmap
        // or a maliciously broken one.
        fill_missing_unicode_from_freetype(font_obj, cmap, diff_names);
#endif

        if (is_type3_subtype) {
            apply_type3_ascii_fallback_fitz(cmap);
        }

        if (!cmap.empty()) {
            cached_unicode_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>(std::move(cmap));
            out[it.first] = cached_unicode_maps[font_obj_id];
        } else {
            cached_unicode_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>();
            out[it.first] = cached_unicode_maps[font_obj_id];
        }
    }

    return out;
}

WinFontCodeBytesMap WinPdfDocument::get_page_font_code_bytes_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontCodeBytesMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    const WinFontCodeSpaceMap code_space_map = get_page_font_codespace_map(page_idx);

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_code_bytes_maps.count(font_obj_id)) {
            out[it.first] = cached_code_bytes_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        int code_bytes = 1;
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            code_bytes = 2;
            auto cs_it = code_space_map.find(it.first);
            if (cs_it != code_space_map.end()) {
                for (const auto& r : *cs_it->second) {
                    if (r.nbytes > code_bytes) {
                        code_bytes = r.nbytes;
                    }
                }
            }
        }

        int result = std::max(1, code_bytes);
        cached_code_bytes_maps[font_obj_id] = result;
        out[it.first] = result;
    }

    return out;
}

WinFontCodeSpaceMap WinPdfDocument::get_page_font_codespace_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontCodeSpaceMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_codespace_maps.count(font_obj_id)) {
            out[it.first] = cached_codespace_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") {
            continue;
        }

        std::vector<WinCodeSpaceRange> ranges;

        int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
        if (encoding_ref > 0) {
            WinPdfObject enc_obj = read_obj(encoding_ref);
            if (enc_obj.is_stream && !enc_obj.stream.empty()) {
                std::vector<uint8_t> cmap_stream = decode_stream_data(enc_obj.stream, enc_obj.dict, this);
                if (cmap_stream.empty()) {
                    cmap_stream = enc_obj.stream;
                }
                if (!cmap_stream.empty()) {
                    ranges = parse_cmap_codespace_ranges_fitz(cmap_stream);
                }
            }
        }

        if (ranges.empty()) {
            std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
            if (ranges.empty() && (encoding_name == "Identity-H" || encoding_name == "Identity-V")) {
                WinCodeSpaceRange r;
                r.nbytes = 2;
                r.low = 0x0000u;
                r.high = 0xFFFFu;
                ranges.push_back(r);
            }
        }

        if (ranges.empty()) {
            int cmap_ref = parse_ref_id_after_key(font_obj.dict, "/ToUnicode");
            if (cmap_ref > 0) {
                WinPdfObject cmap_obj = read_obj(cmap_ref);
                if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                    std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                    if (cmap_stream.empty()) {
                        cmap_stream = cmap_obj.stream;
                    }
                    if (!cmap_stream.empty()) {
                        ranges = parse_cmap_codespace_ranges_fitz(cmap_stream);
                    }
                }
            }
        }

        if (!ranges.empty()) {
            cached_codespace_maps[font_obj_id] = std::make_shared<const std::vector<WinCodeSpaceRange>>(std::move(ranges));
            out[it.first] = cached_codespace_maps[font_obj_id];
        } else {
            cached_codespace_maps[font_obj_id] = std::make_shared<const std::vector<WinCodeSpaceRange>>();
            out[it.first] = cached_codespace_maps[font_obj_id];
        }
    }

    return out;
}

WinFontMatrixMap WinPdfDocument::get_page_font_matrix_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontMatrixMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }
        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref_fitz(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_matrix_array = [](const std::string& array_text, std::array<float, 6>& out_matrix) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[6] = {0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 6; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = std::strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_matrix = {
            static_cast<float>(v[0]), static_cast<float>(v[1]),
            static_cast<float>(v[2]), static_cast<float>(v[3]),
            static_cast<float>(v[4]), static_cast<float>(v[5])
        };
        return true;
    };

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_matrix_maps.count(font_obj_id)) {
            out[it.first] = cached_matrix_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        std::array<float, 6> matrix = {1, 0, 0, 1, 0, 0};

        std::string matrix_array;
        bool has_matrix = false;
        if (extract_array_after_key(font_obj.dict, "/FontMatrix", matrix_array)) {
            has_matrix = parse_matrix_array(matrix_array, matrix);
        }

        if (!has_matrix && parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                if (extract_array_after_key(cid_font_obj.dict, "/FontMatrix", matrix_array)) {
                    has_matrix = parse_matrix_array(matrix_array, matrix);
                }
            }
        }

        cached_matrix_maps[font_obj_id] = matrix;
        out[it.first] = matrix;
    }

    return out;
}

WinFontVerticalMetricsMap WinPdfDocument::get_page_font_vertical_metrics_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontVerticalMetricsMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    const WinFontMatrixMap font_matrix_map = get_page_font_matrix_map(page_idx);

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }

        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref_fitz(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_bbox_array = [](const std::string& array_text, std::array<float, 4>& out_bbox) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = std::strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_bbox = {
            static_cast<float>(v[0]),
            static_cast<float>(v[1]),
            static_cast<float>(v[2]),
            static_cast<float>(v[3])
        };
        return true;
    };

    auto parse_float_after = [&](const std::string& dict, const std::string& key, double fallback) -> double {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return fallback;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return fallback;
        }

        char* end_ptr = nullptr;
        double v = std::strtod(dict.c_str() + pos, &end_ptr);
        if (end_ptr == dict.c_str() + pos) {
            return fallback;
        }
        return v;
    };

    auto parse_metrics_from_descriptor = [&](const std::string& descriptor_dict,
                                             WinFontVerticalMetrics& metrics) -> bool {
        if (descriptor_dict.empty()) {
            return false;
        }

        bool has_value = false;
        const double ascent = parse_float_after(descriptor_dict, "/Ascent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(ascent) && std::fabs(ascent) > 0.001) {
            metrics.ascender = static_cast<float>(std::fabs(ascent / 1000.0));
            has_value = true;
        }

        const double descent = parse_float_after(descriptor_dict, "/Descent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(descent) && std::fabs(descent) > 0.001) {
            metrics.descender = static_cast<float>(descent / 1000.0);
            if (metrics.descender > 0.0f) {
                metrics.descender = -metrics.descender;
            }
            has_value = true;
        }

        metrics.flags = static_cast<int>(parse_float_after(descriptor_dict, "/Flags", 0.0));
        metrics.font_weight = static_cast<float>(parse_float_after(descriptor_dict, "/FontWeight", 400.0));

        return has_value;
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        if (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                if (read_descriptor_from_dict(cid_font_obj.dict)) {
                    return true;
                }
            }
        }

        return false;
    };

#ifdef WINEXTRACT_USE_FREETYPE
    auto fill_metrics_from_freetype = [&](const WinPdfObject& font_obj, WinFontVerticalMetrics& metrics) -> bool {
        FT_Library library = get_freetype_library();
        if (!library) {
            return false;
        }

        std::string descriptor_dict;
        resolve_font_descriptor_dict(font_obj, descriptor_dict);

        std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        base_font_name = normalize_pdf_font_name(base_font_name);

        FT_Face face = nullptr;
        std::vector<uint8_t> font_bytes;

        if (!descriptor_dict.empty()) {
            int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

            if (font_file_ref > 0) {
                WinPdfObject font_file_obj = read_obj(font_file_ref);
                if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                    font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                    if (font_bytes.empty()) {
                        font_bytes = font_file_obj.stream;
                    }
                    if (!font_bytes.empty()) {
                        if (FT_New_Memory_Face(library,
                                               reinterpret_cast<const FT_Byte*>(font_bytes.data()),
                                               static_cast<FT_Long>(font_bytes.size()),
                                               0,
                                               &face) != 0) {
                            face = nullptr;
                        }
                    }
                }
            }
        }

        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    break;
                }
            }
        }

        if (!face) {
            return false;
        }

        const float units_per_em = (face->units_per_EM > 0) ? static_cast<float>(face->units_per_EM) : 1000.0f;
        if (units_per_em <= 0.0f) {
            FT_Done_Face(face);
            return false;
        }

        if (face->ascender != 0) {
            metrics.ascender = static_cast<float>(face->ascender) / units_per_em;
            if (metrics.ascender < 0.0f) {
                metrics.ascender = -metrics.ascender;
            }
        }

        if (face->descender != 0) {
            metrics.descender = static_cast<float>(face->descender) / units_per_em;
            if (metrics.descender > 0.0f) {
                metrics.descender = -metrics.descender;
            }
        }

        FT_Done_Face(face);
        return true;
    };
#endif

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& entry : font_refs) {
        int font_obj_id = entry.second;
        if (cached_vertical_metrics_maps.count(font_obj_id)) {
            out[entry.first] = cached_vertical_metrics_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        const std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        const bool is_type3_subtype = is_type3_font_subtype_fitz(parse_name_value_after_key(font_obj.dict, "/Subtype"));

        WinFontVerticalMetrics metrics = get_base14_vertical_metrics(base_font_name);
        metrics.base_font = base_font_name;

        std::string descriptor_dict;
        const bool has_descriptor = resolve_font_descriptor_dict(font_obj, descriptor_dict);
        bool has_metrics = false;
        if (has_descriptor) {
            has_metrics = parse_metrics_from_descriptor(descriptor_dict, metrics);
        }

        if (!has_metrics && is_type3_subtype) {
            std::string bbox_array;
            std::array<float, 4> bbox = {0, 0, 0, 0};
            if (extract_array_after_key(font_obj.dict, "/FontBBox", bbox_array) && parse_bbox_array(bbox_array, bbox)) {
                std::array<float, 6> matrix = {0.001f, 0, 0, 0.001f, 0, 0};
                auto mit = font_matrix_map.find(entry.first);
                if (mit != font_matrix_map.end()) {
                    matrix = mit->second;
                }

                auto transform_point = [&](float x, float y) -> Vec2 {
                    return {
                        x * matrix[0] + y * matrix[2] + matrix[4],
                        x * matrix[1] + y * matrix[3] + matrix[5]
                    };
                };

                Vec2 p0 = transform_point(bbox[0], bbox[1]);
                Vec2 p1 = transform_point(bbox[0], bbox[3]);
                Vec2 p2 = transform_point(bbox[2], bbox[1]);
                Vec2 p3 = transform_point(bbox[2], bbox[3]);

                metrics.ascender = std::max({p0.y, p1.y, p2.y, p3.y});
                metrics.descender = std::min({p0.y, p1.y, p2.y, p3.y});
                has_metrics = true;
            }
        }

#ifdef WINEXTRACT_USE_FREETYPE
        if (!has_metrics) {
            fill_metrics_from_freetype(font_obj, metrics);
        }
#endif

        if (!std::isfinite(metrics.ascender) || metrics.ascender <= 0.01f) {
            metrics.ascender = 0.8f;
        }
        if (!std::isfinite(metrics.descender) || metrics.descender >= -0.01f) {
            metrics.descender = -0.2f;
        }

        metrics.ascender = std::min(metrics.ascender, 3.0f);
        metrics.descender = std::max(metrics.descender, -3.0f);

        populate_font_flags_and_properties(metrics);
        cached_vertical_metrics_maps[font_obj_id] = metrics;
        out[entry.first] = metrics;
    }

    return out;
}

WinColorSpaceMap WinPdfDocument::get_page_color_space_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinColorSpaceMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);
        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }
        if (!resources_dict.empty()) {
            break;
        }
        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string colorspace_dict;
    int colorspace_ref = parse_ref_id_after_key(resources_dict, "/ColorSpace");
    if (colorspace_ref > 0) {
        WinPdfObject colorspace_obj = read_obj(colorspace_ref);
        colorspace_dict = colorspace_obj.dict;
        if (colorspace_dict.empty()) {
            colorspace_dict = extract_first_dict_fragment(colorspace_obj.body);
        }
    }
    if (colorspace_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/ColorSpace", colorspace_dict);
    }

    if (!colorspace_dict.empty()) {
        parse_colorspace_dict_into_map(this, colorspace_dict, out);
    }

    return out;
}

WinFormXObjectMap WinPdfDocument::get_page_form_xobject_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFormXObjectMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string page_resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && page_resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            page_resources_dict = resources_obj.dict;
        }

        if (page_resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", page_resources_dict);
        }

        if (!page_resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (page_resources_dict.empty()) {
        return out;
    }

    const WinFontUnicodeMap page_unicode_map = get_page_font_unicode_map(page_idx);
    const WinFontWidthMap page_width_map = get_page_font_width_map(page_idx);
    const WinFontCodeBytesMap page_code_bytes_map = get_page_font_code_bytes_map(page_idx);
    const WinFontCodeSpaceMap page_codespace_map = get_page_font_codespace_map(page_idx);
    const WinFontMatrixMap page_matrix_map = get_page_font_matrix_map(page_idx);
    const WinFontVerticalMetricsMap page_vertical_metrics_map = get_page_font_vertical_metrics_map(page_idx);
    const WinColorSpaceMap page_color_space_map = get_page_color_space_map(page_idx);

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }

        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref_fitz(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_matrix_array = [](const std::string& array_text, std::array<float, 6>& out_matrix) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[6] = {0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 6; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = std::strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_matrix = {
            static_cast<float>(v[0]), static_cast<float>(v[1]),
            static_cast<float>(v[2]), static_cast<float>(v[3]),
            static_cast<float>(v[4]), static_cast<float>(v[5])
        };
        return true;
    };

    auto parse_bbox_array = [](const std::string& array_text, std::array<float, 4>& out_bbox) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = std::strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_bbox = {
            static_cast<float>(v[0]),
            static_cast<float>(v[1]),
            static_cast<float>(v[2]),
            static_cast<float>(v[3])
        };
        return true;
    };

    auto parse_float_after = [](const std::string& dict, const std::string& key, double fallback) -> double {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return fallback;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return fallback;
        }

        char* end_ptr = nullptr;
        double v = std::strtod(dict.c_str() + pos, &end_ptr);
        if (end_ptr == dict.c_str() + pos) {
            return fallback;
        }
        return v;
    };

    auto parse_vertical_metrics_from_descriptor = [&](const std::string& descriptor_dict,
                                                      WinFontVerticalMetrics& metrics) -> bool {
        if (descriptor_dict.empty()) {
            return false;
        }

        bool has_value = false;
        const double ascent = parse_float_after(descriptor_dict, "/Ascent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(ascent) && std::fabs(ascent) > 0.001) {
            metrics.ascender = static_cast<float>(std::fabs(ascent / 1000.0));
            has_value = true;
        }

        const double descent = parse_float_after(descriptor_dict, "/Descent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(descent) && std::fabs(descent) > 0.001) {
            metrics.descender = static_cast<float>(descent / 1000.0);
            if (metrics.descender > 0.0f) {
                metrics.descender = -metrics.descender;
            }
            has_value = true;
        }

        return has_value;
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        if (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                if (read_descriptor_from_dict(cid_font_obj.dict)) {
                    return true;
                }
            }
        }

        return false;
    };

    auto resolve_resources_dict = [&](const std::string& object_dict, const std::string& inherited_resources) -> std::string {
        std::string resources_dict;

        int resources_ref = parse_ref_id_after_key(object_dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(object_dict, "/Resources", resources_dict);
        }

        if (resources_dict.empty()) {
            resources_dict = inherited_resources;
        }

        return resources_dict;
    };

    auto merge_fonts_from_resources = [&](const std::string& resources_dict,
                                          WinFontUnicodeMap& unicode_map,
                                          WinFontWidthMap& width_map,
                                          WinFontCodeBytesMap& code_bytes_map,
                                          WinFontCodeSpaceMap& codespace_map,
                                          WinFontMatrixMap& matrix_map,
                                          WinFontVerticalMetricsMap& vertical_metrics_map) {
        if (resources_dict.empty()) {
            return;
        }

        std::string font_dict;
        int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
        if (font_ref > 0) {
            WinPdfObject font_obj = read_obj(font_ref);
            font_dict = font_obj.dict;
        }
        if (font_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
        }
        if (font_dict.empty()) {
            return;
        }

        std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
        for (const auto& it : font_refs) {
            int font_obj_id = it.second;
            
            if (cached_unicode_maps.count(font_obj_id)) {
                unicode_map[it.first] = cached_unicode_maps[font_obj_id];
                if (cached_width_maps.count(font_obj_id)) width_map[it.first] = cached_width_maps[font_obj_id];
                if (cached_code_bytes_maps.count(font_obj_id)) code_bytes_map[it.first] = cached_code_bytes_maps[font_obj_id];
                if (cached_codespace_maps.count(font_obj_id)) codespace_map[it.first] = cached_codespace_maps[font_obj_id];
                if (cached_matrix_maps.count(font_obj_id)) matrix_map[it.first] = cached_matrix_maps[font_obj_id];
                if (cached_vertical_metrics_maps.count(font_obj_id)) vertical_metrics_map[it.first] = cached_vertical_metrics_maps[font_obj_id];
                continue;
            }

            WinPdfObject font_obj = read_obj(font_obj_id);
            const std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
            const bool is_type0_subtype = (subtype == "Type0");
            const bool is_type3_subtype = is_type3_font_subtype_fitz(subtype);
            std::string cid_collection;

            int cmap_ref = parse_ref_id_after_key(font_obj.dict, "/ToUnicode");
            std::unordered_map<int, std::vector<int>> cmap;
            if (cmap_ref > 0) {
                WinPdfObject cmap_obj = read_obj(cmap_ref);
                if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                    std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                    if (cmap_stream.empty()) {
                        cmap_stream = cmap_obj.stream;
                    }
                    cmap = parse_tounicode_cmap_fitz(cmap_stream);
                }
            }

            if (cmap.empty()) {
                std::string to_unicode_name = parse_name_value_after_key(font_obj.dict, "/ToUnicode");
                if (!to_unicode_name.empty()) {
                    const auto& named_cmap = load_system_unicode_cmap_by_name_fitz(to_unicode_name);
                    if (!named_cmap.empty()) {
                        cmap = named_cmap;
                    }
                }
            }

            for (auto it_cmap = cmap.begin(); it_cmap != cmap.end(); ) {
                bool has_invalid = false;
                for (int ucs : it_cmap->second) {
                    if ((ucs < 32 && ucs != 9 && ucs != 10 && ucs != 13) || (ucs >= 127 && ucs < 160)) {
                        has_invalid = true;
                        break;
                    }
                }
                if (has_invalid) {
                    it_cmap = cmap.erase(it_cmap);
                } else {
                    ++it_cmap;
                }
            }

            if (is_type0_subtype && cmap.empty()) {
                std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
                if (!descendant_ids.empty()) {
                    WinPdfObject cid_font_obj = read_obj(descendant_ids.front());

                    std::string cid_system_info_dict;
                    int cid_system_info_ref = parse_ref_id_after_key(cid_font_obj.dict, "/CIDSystemInfo");
                    if (cid_system_info_ref > 0) {
                        WinPdfObject cid_system_info_obj = read_obj(cid_system_info_ref);
                        cid_system_info_dict = cid_system_info_obj.dict;
                    }
                    if (cid_system_info_dict.empty()) {
                        extract_inline_dict_after_key(cid_font_obj.dict, "/CIDSystemInfo", cid_system_info_dict);
                    }

                    cid_collection = build_cid_collection_name_from_cidsysteminfo_fitz(cid_system_info_dict);
                    if (!cid_collection.empty()) {
                        const auto& cid_fallback = load_collection_unicode_cmap_fitz(cid_collection);
                        if (!cid_fallback.empty()) {
                            cmap = cid_fallback;
                        }
                    }
                }
            }

            std::string encoding_dict;
            std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
            if (encoding_name.empty()) {
                int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
                if (encoding_ref > 0) {
                    WinPdfObject enc_obj = read_obj(encoding_ref);
                    encoding_dict = enc_obj.dict;
                    encoding_name = parse_name_value_after_key(enc_obj.dict, "/BaseEncoding");
                    if (encoding_name.empty()) {
                        encoding_name = parse_name_value_after_key(enc_obj.body, "/BaseEncoding");
                    }
                }
            }
            if (encoding_dict.empty()) {
                extract_inline_dict_after_key(font_obj.dict, "/Encoding", encoding_dict);
                if (!encoding_dict.empty() && encoding_name.empty()) {
                    encoding_name = parse_name_value_after_key(encoding_dict, "/BaseEncoding");
                }
            }

            if (!is_type0_subtype) {
                std::map<int, int> fallback = build_encoding_map(encoding_name);
                if (fallback.empty()) {
                    fallback = make_identity_encoding_map();
                }
                if (!encoding_dict.empty()) {
                    apply_differences_to_map(encoding_dict, fallback);
                }

                if (!fallback.empty()) {
                    for (const auto& kv : fallback) {
                        if (cmap.find(kv.first) == cmap.end()) {
                            cmap[kv.first] = {kv.second};
                        }
                    }
                }
            }

            if (is_type3_subtype) {
                apply_type3_ascii_fallback_fitz(cmap);
            }

            if (!cmap.empty()) {
                auto ptr = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>(std::move(cmap));
                unicode_map[it.first] = ptr;
                cached_unicode_maps[font_obj_id] = ptr;
            } else {
                auto ptr = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>();
                unicode_map[it.first] = ptr;
                cached_unicode_maps[font_obj_id] = ptr;
            }

            int code_bytes = 1;
            std::vector<WinCodeSpaceRange> ranges;
            if (is_type0_subtype) {
                code_bytes = 2;

                int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
                if (encoding_ref > 0) {
                    WinPdfObject enc_obj = read_obj(encoding_ref);
                    if (enc_obj.is_stream && !enc_obj.stream.empty()) {
                        std::vector<uint8_t> cmap_stream = decode_stream_data(enc_obj.stream, enc_obj.dict, this);
                        if (cmap_stream.empty()) {
                            cmap_stream = enc_obj.stream;
                        }
                        if (!cmap_stream.empty()) {
                            ranges = parse_cmap_codespace_ranges_fitz(cmap_stream);
                        }
                    }
                }

                if (ranges.empty() && (encoding_name == "Identity-H" || encoding_name == "Identity-V")) {
                    WinCodeSpaceRange r;
                    r.nbytes = 2;
                    r.low = 0x0000u;
                    r.high = 0xFFFFu;
                    ranges.push_back(r);
                }

                if (ranges.empty() && cmap_ref > 0) {
                    WinPdfObject cmap_obj = read_obj(cmap_ref);
                    if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                        std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                        if (cmap_stream.empty()) {
                            cmap_stream = cmap_obj.stream;
                        }
                        if (!cmap_stream.empty()) {
                            ranges = parse_cmap_codespace_ranges_fitz(cmap_stream);
                        }
                    }
                }

                for (const auto& r : ranges) {
                    if (r.nbytes > code_bytes) {
                        code_bytes = r.nbytes;
                    }
                }
            }

            int result_code_bytes = std::max(1, code_bytes);
            code_bytes_map[it.first] = result_code_bytes;
            cached_code_bytes_maps[font_obj_id] = result_code_bytes;

            if (!ranges.empty()) {
                auto ptr = std::make_shared<const std::vector<WinCodeSpaceRange>>(std::move(ranges));
                codespace_map[it.first] = ptr;
                cached_codespace_maps[font_obj_id] = ptr;
            } else {
                auto ptr = std::make_shared<const std::vector<WinCodeSpaceRange>>();
                codespace_map[it.first] = ptr;
                cached_codespace_maps[font_obj_id] = ptr;
            }

            std::array<float, 6> matrix = {1, 0, 0, 1, 0, 0};
            std::string matrix_array;
            bool has_matrix = false;
            if (extract_array_after_key(font_obj.dict, "/FontMatrix", matrix_array)) {
                has_matrix = parse_matrix_array(matrix_array, matrix);
            }
            if (!has_matrix && parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
                std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
                if (!descendant_ids.empty()) {
                    WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                    if (extract_array_after_key(cid_font_obj.dict, "/FontMatrix", matrix_array)) {
                        has_matrix = parse_matrix_array(matrix_array, matrix);
                    }
                }
            }
            matrix_map[it.first] = matrix;
            cached_matrix_maps[font_obj_id] = matrix;
            {
                // Local Form resources can reuse font aliases from the parent scope
                // (for example /T1_0). Recompute metrics for the local binding.
                WinFontVerticalMetrics metrics = get_base14_vertical_metrics(parse_name_value_after_key(font_obj.dict, "/BaseFont"));
                std::string descriptor_dict;
                bool has_metrics = false;
                if (resolve_font_descriptor_dict(font_obj, descriptor_dict)) {
                    has_metrics = parse_vertical_metrics_from_descriptor(descriptor_dict, metrics);
                }

                const bool is_type3_subtype = is_type3_font_subtype_fitz(parse_name_value_after_key(font_obj.dict, "/Subtype"));
                if (!has_metrics && is_type3_subtype) {
                    std::string bbox_array;
                    std::array<float, 4> bbox = {0, 0, 0, 0};
                    if (extract_array_after_key(font_obj.dict, "/FontBBox", bbox_array) && parse_bbox_array(bbox_array, bbox)) {
                        std::array<float, 6> t3m = matrix;
                        if (!has_matrix) {
                            t3m = {0.001f, 0, 0, 0.001f, 0, 0};
                        }

                        auto transform_point = [&](float x, float y) -> Vec2 {
                            return {
                                x * t3m[0] + y * t3m[2] + t3m[4],
                                x * t3m[1] + y * t3m[3] + t3m[5]
                            };
                        };

                        Vec2 p0 = transform_point(bbox[0], bbox[1]);
                        Vec2 p1 = transform_point(bbox[0], bbox[3]);
                        Vec2 p2 = transform_point(bbox[2], bbox[1]);
                        Vec2 p3 = transform_point(bbox[2], bbox[3]);

                        metrics.ascender = std::max({p0.y, p1.y, p2.y, p3.y});
                        metrics.descender = std::min({p0.y, p1.y, p2.y, p3.y});
                        has_metrics = true;
                    }
                }
                if (!std::isfinite(metrics.ascender) || metrics.ascender <= 0.01f) {
                    metrics.ascender = 0.8f;
                }
                if (!std::isfinite(metrics.descender) || metrics.descender >= -0.01f) {
                    metrics.descender = -0.2f;
                }
                metrics.ascender = std::min(metrics.ascender, 3.0f);
                metrics.descender = std::max(metrics.descender, -3.0f);
                vertical_metrics_map[it.first] = metrics;
                cached_vertical_metrics_maps[font_obj_id] = metrics;
            }

            {
                // Same alias shadowing rule as above: local Form font widths must
                // replace inherited widths for the same resource name.
                std::unordered_map<int, float> widths;
                const std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
                const bool is_type3_subtype = is_type3_font_subtype_fitz(subtype);
                std::string descriptor_dict;
                int descriptor_ref = parse_ref_id_after_key(font_obj.dict, "/FontDescriptor");
                if (descriptor_ref > 0) {
                    WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                    descriptor_dict = descriptor_obj.dict;
                }
                if (descriptor_dict.empty()) {
                    extract_inline_dict_after_key(font_obj.dict, "/FontDescriptor", descriptor_dict);
                }

                float default_width = -1.0f;
                if (!descriptor_dict.empty()) {
                    double missing = parse_float_after(descriptor_dict, "/MissingWidth", -1.0);
                    if (missing > 0.0) {
                        default_width = static_cast<float>(missing / 1000.0);
                    }
                }

                const std::string base_font = normalize_pdf_font_name(parse_name_value_after_key(font_obj.dict, "/BaseFont"));

                auto parse_cid_width_array_local = [](const std::string& array_text, std::unordered_map<int, float>& out_widths) {
                    std::vector<std::string> tokens;
                    for (size_t i = 0; i < array_text.size();) {
                        if (std::isspace(static_cast<unsigned char>(array_text[i]))) {
                            ++i;
                            continue;
                        }
                        if (array_text[i] == '[' || array_text[i] == ']') {
                            tokens.push_back(array_text.substr(i, 1));
                            ++i;
                            continue;
                        }

                        size_t start = i;
                        if (array_text[i] == '+' || array_text[i] == '-') {
                            ++i;
                        }
                        while (i < array_text.size() && (std::isdigit(static_cast<unsigned char>(array_text[i])) || array_text[i] == '.')) {
                            ++i;
                        }
                        if (i > start) {
                            tokens.push_back(array_text.substr(start, i - start));
                        } else {
                            ++i;
                        }
                    }

                    size_t i = 0;
                    while (i < tokens.size()) {
                        if (tokens[i] == "[" || tokens[i] == "]") {
                            ++i;
                            continue;
                        }

                        int c0 = std::atoi(tokens[i].c_str());
                        ++i;
                        if (i >= tokens.size()) {
                            break;
                        }

                        if (tokens[i] == "[") {
                            ++i;
                            int cid = c0;
                            while (i < tokens.size() && tokens[i] != "]") {
                                if (tokens[i] != "[") {
                                    out_widths[cid] = static_cast<float>(std::atof(tokens[i].c_str()) / 1000.0);
                                    ++cid;
                                }
                                ++i;
                            }
                            if (i < tokens.size() && tokens[i] == "]") {
                                ++i;
                            }
                        } else {
                            if (i + 1 >= tokens.size()) {
                                break;
                            }
                            int c1 = std::atoi(tokens[i].c_str());
                            ++i;
                            float w = static_cast<float>(std::atof(tokens[i].c_str()) / 1000.0);
                            ++i;

                            if (c1 < c0) {
                                std::swap(c0, c1);
                            }
                            for (int cid = c0; cid <= c1; ++cid) {
                                out_widths[cid] = w;
                            }
                        }
                    }
                };

                if (subtype == "Type0") {
                    std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
                    if (!descendant_ids.empty()) {
                        WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                        double dw = parse_float_after(cid_font_obj.dict, "/DW", 1000.0);
                        widths[-1] = static_cast<float>(dw / 1000.0);

                        std::string w_array;
                        if (extract_array_after_key(cid_font_obj.dict, "/W", w_array)) {
                            parse_cid_width_array_local(w_array, widths);
                        }
                    }

                    if (widths.find(-1) == widths.end()) {
                        widths[-1] = default_width > 0.0f ? default_width : get_base14_width(base_font, 'n');
                    }
                } else if (is_type3_subtype) {
                    int first_char = static_cast<int>(parse_float_after(font_obj.dict, "/FirstChar", -1.0));
                    int last_char = static_cast<int>(parse_float_after(font_obj.dict, "/LastChar", -1.0));
                    if (first_char < 0 || last_char > 255 || first_char > last_char) {
                        first_char = 0;
                        last_char = 0;
                    }

                    std::string widths_array;
                    const float type3_width_scale = matrix[0];
                    if (extract_array_after_key(font_obj.dict, "/Widths", widths_array)) {
                        size_t arr_start = widths_array.find('[');
                        size_t arr_end = widths_array.rfind(']');
                        if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
                            const char* p = widths_array.c_str() + arr_start + 1;
                            const char* end = widths_array.c_str() + arr_end;
                            int code = first_char;
                            while (code <= last_char) {
                                while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                                    ++p;
                                }
                                if (p >= end) {
                                    break;
                                }

                                char* end_ptr = nullptr;
                                const double w = std::strtod(p, &end_ptr);
                                if (end_ptr == p || end_ptr > end) {
                                    break;
                                }
                                p = end_ptr;
                                widths[code] = scale_type3_width_fitz(static_cast<float>(w), type3_width_scale);
                                ++code;
                            }
                        }
                    }

                    widths[-1] = 0.0f;
                } else {
                    int first_char = static_cast<int>(parse_float_after(font_obj.dict, "/FirstChar", 0.0));
                    int last_char = static_cast<int>(parse_float_after(font_obj.dict, "/LastChar", 255.0));
                    if (first_char < 0) first_char = 0;
                    if (last_char < first_char || last_char > 255) last_char = 255;

                    std::string widths_array;
                    if (extract_array_after_key(font_obj.dict, "/Widths", widths_array)) {
                        size_t arr_start = widths_array.find('[');
                        size_t arr_end = widths_array.rfind(']');
                        if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
                            const char* p = widths_array.c_str() + arr_start + 1;
                            const char* end = widths_array.c_str() + arr_end;
                            int code = first_char;
                            while (code <= last_char) {
                                while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                                    ++p;
                                }
                                if (p >= end) {
                                    break;
                                }

                                char* end_ptr = nullptr;
                                const double w = std::strtod(p, &end_ptr);
                                if (end_ptr == p || end_ptr > end) {
                                    break;
                                }
                                p = end_ptr;
                                widths[code] = static_cast<float>(w / 1000.0);
                                ++code;
                            }
                        }
                    }

                    if (default_width <= 0.0f) {
                        default_width = get_base14_width(base_font, 'n');
                    }

                    widths[-1] = default_width;
                    for (int c = first_char; c <= last_char; ++c) {
                        if (widths.find(c) == widths.end()) {
                            widths[c] = get_base14_width(base_font, c);
                        }
                    }
                }
                auto ptr = std::make_shared<const std::unordered_map<int, float>>(std::move(widths));
                width_map[it.first] = ptr;
                cached_width_maps[font_obj_id] = ptr;
            }
        }
    };

    auto merge_color_spaces_from_resources = [&](const std::string& resources_dict,
                                                 WinColorSpaceMap& color_space_map_out) {
        if (resources_dict.empty()) {
            return;
        }

        std::string colorspace_dict;
        int colorspace_ref = parse_ref_id_after_key(resources_dict, "/ColorSpace");
        if (colorspace_ref > 0) {
            WinPdfObject colorspace_obj = read_obj(colorspace_ref);
            colorspace_dict = colorspace_obj.dict;
            if (colorspace_dict.empty()) {
                colorspace_dict = extract_first_dict_fragment(colorspace_obj.body);
            }
        }

        if (colorspace_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/ColorSpace", colorspace_dict);
        }

        if (!colorspace_dict.empty()) {
            parse_colorspace_dict_into_map(this, colorspace_dict, color_space_map_out);
        }
    };

    std::function<WinFormXObjectMap(const std::string&,
                                    const WinFontUnicodeMap&,
                                    const WinFontWidthMap&,
                                    const WinFontCodeBytesMap&,
                                    const WinFontCodeSpaceMap&,
                                    const WinFontMatrixMap&,
                                    const WinFontVerticalMetricsMap&,
                                    const WinColorSpaceMap&,
                                    std::set<int>&)> build_forms;

    build_forms = [&](const std::string& resources_dict,
                      const WinFontUnicodeMap& inherited_unicode,
                      const WinFontWidthMap& inherited_width,
                      const WinFontCodeBytesMap& inherited_code_bytes,
                      const WinFontCodeSpaceMap& inherited_codespace,
                      const WinFontMatrixMap& inherited_matrix,
                      const WinFontVerticalMetricsMap& inherited_vertical_metrics,
                      const WinColorSpaceMap& inherited_color_space,
                      std::set<int>& recursion_guard) -> WinFormXObjectMap {
        WinFormXObjectMap forms;
        if (resources_dict.empty()) {
            return forms;
        }

        std::string xobject_dict;
        int xobject_ref = parse_ref_id_after_key(resources_dict, "/XObject");
        if (xobject_ref > 0) {
            WinPdfObject xobject_obj = read_obj(xobject_ref);
            xobject_dict = xobject_obj.dict;
        }
        if (xobject_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/XObject", xobject_dict);
        }
        if (xobject_dict.empty()) {
            return forms;
        }

        std::map<std::string, int> xobject_refs = parse_font_refs_from_dict(xobject_dict);
        for (const auto& it : xobject_refs) {
            const int xobj_id = it.second;
            if (xobj_id <= 0 || recursion_guard.find(xobj_id) != recursion_guard.end()) {
                continue;
            }

            WinPdfObject xobj = read_obj(xobj_id);
            std::string xobj_dict = xobj.dict;
            if (xobj_dict.empty() || xobj_dict.find("/Subtype") == std::string::npos || xobj_dict.find("/Resources") == std::string::npos) {
                std::string dict_from_body = extract_first_dict_fragment(xobj.body);
                if (!dict_from_body.empty()) {
                    xobj_dict = std::move(dict_from_body);
                }
            }

            if (!xobj.is_stream || xobj.stream.empty()) {
                continue;
            }
            if (xobj_dict.empty() || parse_name_value_after_key(xobj_dict, "/Subtype") != "Form") {
                continue;
            }

            std::shared_ptr<std::vector<uint8_t>> decoded_ptr;
            if (cached_decoded_streams.count(xobj_id)) {
                decoded_ptr = cached_decoded_streams[xobj_id];
            } else {
                std::vector<uint8_t> decoded = decode_stream_data(xobj.stream, xobj_dict, this);
                if (decoded.empty()) {
                    decoded = xobj.stream;
                }
                decoded_ptr = std::make_shared<std::vector<uint8_t>>(std::move(decoded));
                cached_decoded_streams[xobj_id] = decoded_ptr;
            }

            if (!decoded_ptr || decoded_ptr->empty()) {
                continue;
            }

            WinFormXObject form;
            form.stream = *decoded_ptr;
            form.font_unicode_map = inherited_unicode;
            form.font_width_map = inherited_width;
            form.font_code_bytes_map = inherited_code_bytes;
            form.font_codespace_map = inherited_codespace;
            form.font_matrix_map = inherited_matrix;
            form.font_vertical_metrics_map = inherited_vertical_metrics;
            form.color_space_map = inherited_color_space;

            std::string child_resources = resolve_resources_dict(xobj_dict, resources_dict);
            merge_fonts_from_resources(child_resources,
                                       form.font_unicode_map,
                                       form.font_width_map,
                                       form.font_code_bytes_map,
                                       form.font_codespace_map,
                                       form.font_matrix_map,
                                       form.font_vertical_metrics_map);
            merge_color_spaces_from_resources(child_resources, form.color_space_map);

            std::string matrix_array;
            if (extract_array_after_key(xobj_dict, "/Matrix", matrix_array)) {
                std::array<float, 6> m = {1, 0, 0, 1, 0, 0};
                if (parse_matrix_array(matrix_array, m)) {
                    form.matrix = m;
                }
            }

            std::string bbox_array;
            if (extract_array_after_key(xobj_dict, "/BBox", bbox_array)) {
                std::array<float, 6> b_tmp = {0, 0, 0, 0, 0, 0};
                if (parse_matrix_array(bbox_array, b_tmp)) {
                    form.bbox = {b_tmp[0], b_tmp[1], b_tmp[2], b_tmp[3]};
                    form.has_bbox = true;
                }
            }

            recursion_guard.insert(xobj_id);
            form.children = std::make_shared<WinFormXObjectMap>(build_forms(child_resources,
                                        form.font_unicode_map,
                                        form.font_width_map,
                                        form.font_code_bytes_map,
                                        form.font_codespace_map,
                                        form.font_matrix_map,
                                        form.font_vertical_metrics_map,
                                        form.color_space_map,
                                        recursion_guard));
            recursion_guard.erase(xobj_id);

            forms[it.first] = std::move(form);
        }

        return forms;
    };

    std::set<int> recursion_guard;
    out = build_forms(page_resources_dict,
                      page_unicode_map,
                      page_width_map,
                      page_code_bytes_map,
                      page_codespace_map,
                      page_matrix_map,
                      page_vertical_metrics_map,
                      page_color_space_map,
                      recursion_guard);
    return out;
}

WinFontWidthMap WinPdfDocument::get_page_font_width_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontWidthMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    WinFontUnicodeMap unicode_map = get_page_font_unicode_map(page_idx);

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);
        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }
        if (!resources_dict.empty()) break;
        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }
    if (resources_dict.empty()) return out;

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) return out;

    auto parse_int_after = [&](const std::string& dict, const std::string& key) -> int {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) return -1;
        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) ++pos;
        if (pos >= dict.size()) return -1;
        return std::atoi(dict.c_str() + pos);
    };

    auto parse_float_after = [&](const std::string& dict, const std::string& key, double fallback) -> double {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) return fallback;
        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) ++pos;
        if (pos >= dict.size()) return fallback;
        char* end_ptr = nullptr;
        double v = std::strtod(dict.c_str() + pos, &end_ptr);
        if (end_ptr == dict.c_str() + pos) {
            return fallback;
        }
        return v;
    };

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }
        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref_fitz(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_type3_font_matrix_scale = [&](const WinPdfObject& font_obj) -> float {
        std::string matrix_array;
        if (!extract_array_after_key(font_obj.dict, "/FontMatrix", matrix_array)) {
            return 1.0f;
        }

        size_t arr_start = matrix_array.find('[');
        size_t arr_end = matrix_array.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return 1.0f;
        }

        const char* p = matrix_array.c_str() + arr_start + 1;
        const char* end = matrix_array.c_str() + arr_end;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (p >= end) {
            return 1.0f;
        }

        char* end_ptr = nullptr;
        double a = std::strtod(p, &end_ptr);
        if (end_ptr == p || end_ptr > end || !std::isfinite(a)) {
            return 1.0f;
        }

        return static_cast<float>(a);
    };

    auto parse_cid_width_array = [](const std::string& array_text, std::unordered_map<int, float>& widths) {
        std::vector<std::string> tokens;
        for (size_t i = 0; i < array_text.size();) {
            if (std::isspace(static_cast<unsigned char>(array_text[i]))) {
                ++i;
                continue;
            }
            if (array_text[i] == '[' || array_text[i] == ']') {
                tokens.push_back(array_text.substr(i, 1));
                ++i;
                continue;
            }

            size_t start = i;
            if (array_text[i] == '+' || array_text[i] == '-') {
                ++i;
            }
            while (i < array_text.size() && (std::isdigit(static_cast<unsigned char>(array_text[i])) || array_text[i] == '.')) {
                ++i;
            }
            if (i > start) {
                tokens.push_back(array_text.substr(start, i - start));
            } else {
                ++i;
            }
        }

        size_t i = 0;
        while (i < tokens.size()) {
            if (tokens[i] == "[" || tokens[i] == "]") {
                ++i;
                continue;
            }

            int c0 = std::atoi(tokens[i].c_str());
            ++i;
            if (i >= tokens.size()) {
                break;
            }

            if (tokens[i] == "[") {
                ++i;
                int cid = c0;
                while (i < tokens.size() && tokens[i] != "]") {
                    if (tokens[i] != "[") {
                        widths[cid] = static_cast<float>(std::atof(tokens[i].c_str()) / 1000.0);
                        ++cid;
                    }
                    ++i;
                }
                if (i < tokens.size() && tokens[i] == "]") {
                    ++i;
                }
            } else {
                if (i + 1 >= tokens.size()) {
                    break;
                }
                int c1 = std::atoi(tokens[i].c_str());
                ++i;
                float w = static_cast<float>(std::atof(tokens[i].c_str()) / 1000.0);
                ++i;

                if (c1 < c0) {
                    std::swap(c0, c1);
                }
                for (int cid = c0; cid <= c1; ++cid) {
                    widths[cid] = w;
                }
            }
        }
    };

#ifdef WINEXTRACT_USE_FREETYPE
    struct CidToGidMapData {
        bool has_map = false;
        bool identity = false;
        std::vector<uint16_t> values;
    };

    auto resolve_type0_descendant_font = [&](const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj) -> bool {
        descendant_font_obj = {};
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") {
            return false;
        }

        std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
        if (descendant_ids.empty()) {
            int single_descendant = parse_ref_id_after_key(font_obj.dict, "/DescendantFonts");
            if (single_descendant > 0) {
                descendant_ids.push_back(single_descendant);
            }
        }
        if (descendant_ids.empty() || descendant_ids.front() <= 0) {
            return false;
        }

        descendant_font_obj = read_obj(descendant_ids.front());
        return descendant_font_obj.id > 0 || !descendant_font_obj.dict.empty() || !descendant_font_obj.body.empty();
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        WinPdfObject descendant_font_obj;
        if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return read_descriptor_from_dict(descendant_font_obj.dict);
        }

        return false;
    };

    auto load_cid_to_gid_map = [&](const WinPdfObject& font_obj) -> CidToGidMapData {
        CidToGidMapData cid_to_gid;

        WinPdfObject descendant_font_obj;
        if (!resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return cid_to_gid;
        }

        const std::string direct_map_name = parse_name_value_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (direct_map_name == "Identity") {
            cid_to_gid.has_map = true;
            cid_to_gid.identity = true;
            return cid_to_gid;
        }

        const int map_ref = parse_ref_id_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (map_ref <= 0) {
            if (direct_map_name.empty()) {
                cid_to_gid.has_map = true;
                cid_to_gid.identity = true;
            }
            return cid_to_gid;
        }

        WinPdfObject map_obj = read_obj(map_ref);
        if (!map_obj.is_stream || map_obj.stream.empty()) {
            return cid_to_gid;
        }

        std::vector<uint8_t> decoded = decode_stream_data(map_obj.stream, map_obj.dict, this);
        if (decoded.empty()) {
            decoded = map_obj.stream;
        }
        if (decoded.size() < 2) {
            return cid_to_gid;
        }

        cid_to_gid.values.reserve(decoded.size() / 2);
        for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
            const uint16_t gid = static_cast<uint16_t>((static_cast<uint16_t>(decoded[i]) << 8) | decoded[i + 1]);
            cid_to_gid.values.push_back(gid);
        }

        cid_to_gid.has_map = !cid_to_gid.values.empty();
        return cid_to_gid;
    };

    auto fill_missing_widths_from_freetype = [&](const WinPdfObject& font_obj,
                                                 const std::unordered_map<int, std::vector<int>>* code_to_unicode,
                                                 std::unordered_map<int, float>& widths) {
        FT_Library library = get_freetype_library();
        if (!library) {
            return;
        }

        const bool is_type0_subtype = parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0";
        const CidToGidMapData cid_to_gid = load_cid_to_gid_map(font_obj);

        std::string descriptor_dict;
        resolve_font_descriptor_dict(font_obj, descriptor_dict);
        std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        if (base_font_name.empty() && is_type0_subtype) {
            WinPdfObject descendant_font_obj;
            if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
                base_font_name = parse_name_value_after_key(descendant_font_obj.dict, "/BaseFont");
            }
        }
        base_font_name = normalize_pdf_font_name(base_font_name);

        FT_Face face = nullptr;
        std::vector<uint8_t> font_bytes;

        if (!descriptor_dict.empty()) {
            int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

            if (font_file_ref > 0) {
                WinPdfObject font_file_obj = read_obj(font_file_ref);
                if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                    font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                    if (font_bytes.empty()) {
                        font_bytes = font_file_obj.stream;
                    }
                    if (!font_bytes.empty()) {
                        if (FT_New_Memory_Face(library,
                                               reinterpret_cast<const FT_Byte*>(font_bytes.data()),
                                               static_cast<FT_Long>(font_bytes.size()),
                                               0,
                                               &face) != 0) {
                            face = nullptr;
                        }
                    }
                }
            }
        }

        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    break;
                }
            }
        }

        if (!face) {
            return;
        }

        FT_Select_Charmap(face, FT_ENCODING_UNICODE);

        const float units_per_em = (face->units_per_EM > 0) ? static_cast<float>(face->units_per_EM) : 1000.0f;

        auto lookup_gid_for_code = [&](int code) -> FT_UInt {
            if (code < 0) {
                return 0;
            }

            if (is_type0_subtype) {
                if (cid_to_gid.has_map) {
                    if (cid_to_gid.identity) {
                        const FT_UInt gid = static_cast<FT_UInt>(code);
                        if (face->num_glyphs <= 0 || gid < static_cast<FT_UInt>(face->num_glyphs)) {
                            return gid;
                        }
                        return 0;
                    }
                    if (static_cast<size_t>(code) < cid_to_gid.values.size()) {
                        return static_cast<FT_UInt>(cid_to_gid.values[static_cast<size_t>(code)]);
                    }
                }
                return 0;
            }

            FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
            if (gid != 0) {
                return gid;
            }

            FT_CharMap saved_charmap = face->charmap;
            if (face->num_charmaps > 0 && face->charmaps != nullptr) {
                for (int ci = 0; ci < face->num_charmaps; ++ci) {
                    FT_CharMap cmap = face->charmaps[ci];
                    if (cmap == nullptr || cmap == face->charmap) {
                        continue;
                    }
                    if (FT_Set_Charmap(face, cmap) != 0) {
                        continue;
                    }
                    gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
                    if (gid != 0) {
                        break;
                    }
                }
            }

            if (saved_charmap != nullptr && face->charmap != saved_charmap) {
                FT_Set_Charmap(face, saved_charmap);
            }

            return gid;
        };

        if (code_to_unicode) {
            for (const auto& cu : *code_to_unicode) {
                const int code = cu.first;
                if (code < 0 || widths.find(code) != widths.end()) {
                    continue;
                }

                if (cu.second.empty()) {
                    continue;
                }

                FT_UInt glyph_index = 0;
                if (is_type0_subtype) {
                    glyph_index = lookup_gid_for_code(code);
                }
                if (glyph_index == 0) {
                    glyph_index = FT_Get_Char_Index(face, static_cast<FT_ULong>(cu.second.front()));
                }
                if (glyph_index == 0) {
                    continue;
                }
                if (face->num_glyphs > 0 && glyph_index >= static_cast<FT_UInt>(face->num_glyphs)) {
                    continue;
                }
                if (FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) {
                    continue;
                }

                widths[code] = static_cast<float>(face->glyph->metrics.horiAdvance) / units_per_em;
            }
        }

        int max_code = 255;
        if (is_type0_subtype) {
            max_code = 65535;
            if (cid_to_gid.has_map && !cid_to_gid.identity && !cid_to_gid.values.empty()) {
                const size_t capped_size = (std::min)(cid_to_gid.values.size(), static_cast<size_t>(65536));
                if (capped_size > 0) {
                    max_code = static_cast<int>(capped_size - 1);
                }
            }
        }

        for (int code = 0; code <= max_code; ++code) {
            if (widths.find(code) != widths.end()) {
                continue;
            }

            FT_UInt glyph_index = lookup_gid_for_code(code);
            if (code_to_unicode) {
                auto u = code_to_unicode->find(code);
                if (u != code_to_unicode->end() && !u->second.empty()) {
                    FT_UInt from_unicode = FT_Get_Char_Index(face, static_cast<FT_ULong>(u->second.front()));
                    if (glyph_index == 0 && from_unicode != 0) {
                        glyph_index = from_unicode;
                    }
                }
            }
            if (glyph_index == 0) {
                continue;
            }
            if (face->num_glyphs > 0 && glyph_index >= static_cast<FT_UInt>(face->num_glyphs)) {
                continue;
            }

            if (FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) {
                continue;
            }

            widths[code] = static_cast<float>(face->glyph->metrics.horiAdvance) / units_per_em;
        }

        FT_Done_Face(face);
    };
#endif

    auto parse_missing_width_from_descriptor = [&](const WinPdfObject& font_obj) -> float {
        std::string descriptor_dict;
        auto read_descriptor_from_dict = [&](const std::string& dict) {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
        };

        read_descriptor_from_dict(font_obj.dict);

        if (descriptor_dict.empty() && parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                read_descriptor_from_dict(cid_font_obj.dict);
            }
        }
        if (descriptor_dict.empty()) {
            return -1.0f;
        }

        const double missing_width = parse_float_after(descriptor_dict, "/MissingWidth", -1.0);
        if (missing_width <= 0.0) {
            return -1.0f;
        }
        return static_cast<float>(missing_width / 1000.0);
    };

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& entry : font_refs) {
        int font_obj_id = entry.second;
        if (cached_width_maps.count(font_obj_id)) {
            out[entry.first] = cached_width_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
        const bool is_type3_subtype = is_type3_font_subtype_fitz(subtype);
        const float type3_width_scale = is_type3_subtype ? parse_type3_font_matrix_scale(font_obj) : 1.0f;
        std::string base_font = normalize_pdf_font_name(parse_name_value_after_key(font_obj.dict, "/BaseFont"));

        std::unordered_map<int, float> widths;

        const float missing_width = parse_missing_width_from_descriptor(font_obj);

        if (subtype == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                double dw = parse_float_after(cid_font_obj.dict, "/DW", 1000.0);
                widths[-1] = static_cast<float>(dw / 1000.0);

                std::string w_array;
                if (extract_array_after_key(cid_font_obj.dict, "/W", w_array)) {
                    parse_cid_width_array(w_array, widths);
                }
            }

            if (widths.find(-1) == widths.end() && missing_width > 0.0f) {
                widths[-1] = missing_width;
            }

#ifdef WINEXTRACT_USE_FREETYPE
            const std::unordered_map<int, std::vector<int>>* code_to_unicode = nullptr;
            auto uit = unicode_map.find(entry.first);
            if (uit != unicode_map.end()) {
                code_to_unicode = uit->second.get();
            }
            fill_missing_widths_from_freetype(font_obj, code_to_unicode, widths);
#endif
        } else {
            int first_char = parse_int_after(font_obj.dict, "/FirstChar");
            int last_char  = parse_int_after(font_obj.dict, "/LastChar");

            if (is_type3_subtype && (first_char < 0 || last_char > 255 || first_char > last_char)) {
                first_char = 0;
                last_char = 0;
            }

            if (first_char >= 0 && last_char >= first_char) {
                std::string widths_array;
                if (extract_array_after_key(font_obj.dict, "/Widths", widths_array)) {
                    size_t arr_start = widths_array.find('[');
                    size_t arr_end   = widths_array.rfind(']');
                    if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
                        const char* p = widths_array.c_str() + arr_start + 1;
                        const char* end = widths_array.c_str() + arr_end;
                        int code = first_char;
                        while (code <= last_char) {
                            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                                ++p;
                            }
                            if (p >= end) {
                                break;
                            }

                            char* end_ptr = nullptr;
                            const double w = std::strtod(p, &end_ptr);
                            if (end_ptr == p || end_ptr > end) {
                                break;
                            }
                            p = end_ptr;
                            if (is_type3_subtype) {
                                widths[code] = scale_type3_width_fitz(static_cast<float>(w), type3_width_scale);
                            } else {
                                const float parsed_width = static_cast<float>(w / 1000.0);
                                if (parsed_width > 0.0f) {
                                    widths[code] = parsed_width;
                                }
                            }
                            ++code;
                        }
                    }
                }
            }

            if (is_type3_subtype) {
                if (widths.find(-1) == widths.end()) {
                    widths[-1] = 0.0f;
                }
            } else if (widths.find(-1) == widths.end() && missing_width > 0.0f) {
                widths[-1] = missing_width;
            }

#ifdef WINEXTRACT_USE_FREETYPE
            if (!is_type3_subtype) {
                const std::unordered_map<int, std::vector<int>>* code_to_unicode = nullptr;
                auto uit = unicode_map.find(entry.first);
                if (uit != unicode_map.end()) {
                    code_to_unicode = uit->second.get();
                }
                fill_missing_widths_from_freetype(font_obj, code_to_unicode, widths);
            }
#endif

            if (!is_type3_subtype && !base_font.empty()) {
                const std::string lower_base = to_lower_ascii(base_font);
                if (lower_base.find("italic") != std::string::npos || lower_base.find("oblique") != std::string::npos) {
                    widths[-2] = 1.0f;
                }
            }

            if (!is_type3_subtype) {
                int fallback_first_char = std::max(0, parse_int_after(font_obj.dict, "/FirstChar"));
                int fallback_last_char  = parse_int_after(font_obj.dict, "/LastChar");
                if (fallback_last_char < fallback_first_char || fallback_last_char > 255) {
                    fallback_last_char = 255;
                }

                for (int c = fallback_first_char; c <= fallback_last_char; ++c) {
                    if (widths.find(c) == widths.end()) {
                        widths[c] = get_base14_width(base_font, c);
                    }
                }
            }
        }

        if (!widths.empty()) {
            auto def_it = widths.find(-1);
            if (def_it != widths.end() && def_it->second <= 0.0f && !is_type3_subtype) {
                widths.erase(def_it);
            }
        }

        if (!widths.empty()) {
            cached_width_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, float>>(std::move(widths));
            out[entry.first] = cached_width_maps[font_obj_id];
        } else {
            cached_width_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, float>>();
            out[entry.first] = cached_width_maps[font_obj_id];
        }
    }

    return out;
}

Rect WinPdfDocument::get_page_mediabox(int page_idx) {
    Rect fallback = {0, 0, 595, 842};
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return fallback;
    }

    auto extract_balanced_array = [&](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }

        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref_fitz(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_rect_from_array = [&](const std::string& array_text, Rect& out_rect) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        float v[4] = {0, 0, 595, 842};
        for (int k = 0; k < 4; ++k) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            const double d = std::strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
            v[k] = static_cast<float>(d);
        }

        out_rect = {v[0], v[1], v[2], v[3]};
        return true;
    };

    Rect crop_box = fallback;
    Rect media_box = fallback;
    bool has_crop = false;
    bool has_media = false;

    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        if (!has_crop) {
            std::string crop_array;
            if (extract_array_after_key(node.dict, "/CropBox", crop_array) ||
                extract_array_after_key(node.dict, "/cropbox", crop_array)) {
                has_crop = parse_rect_from_array(crop_array, crop_box);
            }
        }

        if (!has_media) {
            std::string media_array;
            if (extract_array_after_key(node.dict, "/MediaBox", media_array) ||
                extract_array_after_key(node.dict, "/mediabox", media_array)) {
                has_media = parse_rect_from_array(media_array, media_box);
            }
        }

        if (has_crop && has_media) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (has_crop) {
        return crop_box;
    }
    if (has_media) {
        return media_box;
    }
    return fallback;
}

void WinPdfDocument::clear_page_cache() {
    // DO NOT CLEAR ANY CATCHES! 
    // object_cache.clear();
    // Do NOT clear objstm_infos, objstm_lookup, or objstm_index_ready.
    // They are structural document-level caches. Clearing them forces
    // O(N^2) zlib decompressions for all Object Streams on every page.
}

WinPdfObject WinPdfDocument::read_obj(int id) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    auto cached = object_cache.find(id);
    if (cached != object_cache.end()) {
        return cached->second;
    }

    WinPdfObject obj;
    obj.id = id;

    auto it = xref.find(id);
    if (it != xref.end()) {
        obj = read_obj_from_offset(id, it->second);
        if (object_has_payload(obj)) {
            object_cache[id] = obj;
            return obj;
        }
    }

    obj = read_obj_from_objstm(id);
    if (object_has_payload(obj)) {
        object_cache[id] = obj;
    }
    return obj;
}

WinPdfObject WinPdfDocument::read_obj_from_offset(int id, size_t offset) {
    WinPdfObject obj;
    obj.id = id;

    // ─── Zero-copy: đọc trực tiếp từ file_view_ (mmap hoặc fallback vector) ────
    const std::string_view fv = file_view_;
    const size_t fv_size = fv.size();
    const char* fv_data  = fv.data();

    if (offset >= fv_size) {
        return obj;
    }

    // raw là string_view — không copy thêm byte nào
    const std::string_view raw = fv;

    auto find_token = [&](const char* token, size_t start_pos, size_t end_pos) -> size_t {
        if (start_pos > end_pos || start_pos > fv_size) {
            return std::string_view::npos;
        }
        end_pos = std::min(end_pos, fv_size);
        size_t p = raw.find(token, start_pos);
        if (p == std::string_view::npos || p > end_pos) {
            return std::string_view::npos;
        }
        return p;
    };

    size_t header_scan_end = std::min(fv_size, offset + static_cast<size_t>(128));
    size_t header_obj_pos = find_token("obj", offset, header_scan_end);
    if (header_obj_pos == std::string_view::npos) {
        return obj;
    }

    // Header: đọc trực tiếp từ mmap — chỉ copy cái header nhỏ để parse
    std::string header(fv_data + offset, header_obj_pos + 3 - offset);
    wz_parse_obj_header_fitz(header.c_str(), obj.id, obj.gen);

    size_t body_start = header_obj_pos + 3;
    while (body_start < fv_size &&
           std::isspace(static_cast<unsigned char>(fv_data[body_start]))) {
        ++body_start;
    }

    size_t initial_endobj = fv_size;
    size_t endobj_pos = raw.find("endobj", body_start);
    if (endobj_pos != std::string_view::npos) {
        initial_endobj = endobj_pos;
    }

    size_t obj_end = initial_endobj;
    size_t stream_pos = find_token("stream", body_start, initial_endobj);
    if (stream_pos != std::string_view::npos) {
        // pre_stream: view — zero copy
        const std::string_view pre_stream_sv = raw.substr(body_start, stream_pos - body_start);
        obj.dict = extract_first_dict_fragment(std::string(pre_stream_sv));

        size_t stream_abs_start = stream_pos + 6;
        if (stream_abs_start < fv_size && fv_data[stream_abs_start] == '\r') {
            ++stream_abs_start;
        }
        if (stream_abs_start < fv_size && fv_data[stream_abs_start] == '\n') {
            ++stream_abs_start;
        }

        size_t stream_abs_end = fv_size;
        int stream_len = -1;
        size_t len_pos = obj.dict.find("/Length");
        if (len_pos != std::string::npos) {
            const char* ptr = obj.dict.c_str() + len_pos + 7;
            while (*ptr && std::isspace(static_cast<unsigned char>(*ptr))) {
                ++ptr;
            }

            int ref_id = 0;
            int ref_gen = 0;
            if (wz_parse_obj_ref_fitz(ptr, ref_id, ref_gen) && ref_id > 0 && ref_id != id) {
                WinPdfObject len_obj = read_obj(ref_id);
                stream_len = parse_first_int(len_obj.body, -1);
            } else {
                char* end_ptr = nullptr;
                long v = wz_strtol(ptr, &end_ptr, 10);
                if (end_ptr != ptr) {
                    stream_len = static_cast<int>(v);
                }
            }
        }

        if (stream_len >= 0) {
            size_t candidate_end = stream_abs_start + static_cast<size_t>(stream_len);
            if (candidate_end <= fv_size) {
                stream_abs_end = candidate_end;
            }
        }

        if (stream_abs_end <= stream_abs_start || stream_abs_end > fv_size) {
            size_t endstream_pos = raw.find("endstream", stream_abs_start);
            if (endstream_pos != std::string_view::npos) {
                stream_abs_end = endstream_pos;
            }
        }

        if (stream_abs_start <= fv_size &&
            stream_abs_end > stream_abs_start &&
            stream_abs_end <= fv_size) {
            obj.is_stream = true;
            const size_t stream_size = stream_abs_end - stream_abs_start;
            // stream vẫn phải copy vào vector vì FlateDecode cần mutate buffer
            obj.stream.resize(stream_size);
            if (stream_size > 0) {
                std::memcpy(obj.stream.data(), fv_data + stream_abs_start, stream_size);
            }

            size_t endobj_after_stream = raw.find("endobj", stream_abs_end);
            if (endobj_after_stream != std::string_view::npos) {
                obj_end = endobj_after_stream;
            }
        }
    }

    if (obj_end <= body_start || obj_end > fv_size) {
        obj_end = fv_size;
    }

    // body: copy nhỏ — chỉ đoạn giữa obj...endobj
    obj.body.assign(fv_data + body_start, obj_end - body_start);
    if (obj.dict.empty()) {
        obj.dict = extract_first_dict_fragment(obj.body);
    }

    return obj;
}

void WinPdfDocument::build_objstm_index() {
    objstm_index_ready = true;
}

void WinPdfDocument::load_objstm(int container_id) {
    if (objstm_infos.find(container_id) != objstm_infos.end()) {
        return;
    }

    auto it = xref.find(container_id);
    if (it == xref.end()) {
        return;
    }

    WinPdfObject container = read_obj_from_offset(container_id, it->second);
    if (!container.is_stream || container.dict.empty()) {
        return;
    }

    std::vector<uint8_t> decoded = decode_stream_data(container.stream, container.dict, this);
    if (decoded.empty()) {
        decoded = container.stream;
    }
    if (decoded.empty()) {
        return;
    }

    const int n = parse_int_after_key(container.dict, "/N", -1);
    const int first = parse_int_after_key(container.dict, "/First", -1);
    if (n <= 0 || first < 0 || static_cast<size_t>(first) > decoded.size()) {
        return;
    }

    ObjStreamInfo info;
    info.first = first;
    info.decoded = std::move(decoded);

    std::string table(reinterpret_cast<const char*>(info.decoded.data()), static_cast<size_t>(first));
    const char* p = table.c_str();
    for (int i = 0; i < n; ++i) {
        char* end_ptr = nullptr;
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (!*p) {
            break;
        }

        const long obj_id_long = wz_strtol(p, &end_ptr, 10);
        if (end_ptr == p) {
            break;
        }
        p = end_ptr;

        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (!*p) {
            break;
        }

        const long rel_off_long = wz_strtol(p, &end_ptr, 10);
        if (end_ptr == p) {
            break;
        }
        p = end_ptr;

        const int obj_id = static_cast<int>(obj_id_long);
        const int rel_off = static_cast<int>(rel_off_long);
        if (obj_id > 0 && rel_off >= 0) {
            info.items.push_back({obj_id, rel_off});
            objstm_lookup[obj_id] = {container_id, static_cast<int>(info.items.size() - 1)};
        }
    }

    if (info.items.empty()) {
        return;
    }

    objstm_infos[container_id] = std::move(info);
}

WinPdfObject WinPdfDocument::read_obj_from_objstm(int id) {
    WinPdfObject obj;
    obj.id = id;

    auto hit = objstm_lookup.find(id);
    if (hit == objstm_lookup.end()) {
        return obj;
    }

    const int container_id = hit->second.container_id;
    if (objstm_infos.find(container_id) == objstm_infos.end()) {
        load_objstm(container_id);
    }

    auto info_it = objstm_infos.find(container_id);
    if (info_it == objstm_infos.end()) {
        return obj;
    }

    const ObjStreamInfo& info = info_it->second;
    int idx = hit->second.item_index;
    if (idx < 0 || idx >= static_cast<int>(info.items.size()) || info.items[idx].first != id) {
        idx = -1;
        for (int i = 0; i < static_cast<int>(info.items.size()); ++i) {
            if (info.items[i].first == id) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0 || idx >= static_cast<int>(info.items.size())) {
        return obj;
    }

    const int rel_off = info.items[idx].second;
    if (rel_off < 0) {
        return obj;
    }

    size_t start = static_cast<size_t>(info.first + rel_off);
    size_t end = info.decoded.size();

    // Find the closest following object start in byte order.
    for (const auto& item : info.items) {
        if (item.second <= rel_off) {
            continue;
        }
        const size_t candidate = static_cast<size_t>(info.first + item.second);
        if (candidate < end) {
            end = candidate;
        }
    }

    if (start >= end || end > info.decoded.size()) {
        return obj;
    }

    obj.body = trim_copy(std::string(reinterpret_cast<const char*>(info.decoded.data() + start), end - start));
    obj.dict = extract_first_dict_fragment(obj.body);
    return obj;
}

void WinPdfDocument::parse_xref() {
    xref.clear();
    object_cache.clear();
    objstm_infos.clear();
    objstm_lookup.clear();
    objstm_index_ready = false;

    root_id = 0;
    if (file_view_.empty()) {
        return;
    }

    // Zero-copy: raw là string_view trỏ thẳng vào mmap — không tốn thêm RAM
    const std::string_view raw = file_view_;

    auto skip_ws = [&](size_t& pos) {
        while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) {
            ++pos;
        }
    };

    auto read_int = [&](size_t& pos, long& out) -> bool {
        skip_ws(pos);
        if (pos >= raw.size()) {
            return false;
        }
        char* end_ptr = nullptr;
        long v = wz_strtol_fitz(raw.data() + pos, &end_ptr, 10);
        if (end_ptr == raw.data() + pos) {
            return false;
        }
        out = v;
        pos = static_cast<size_t>(end_ptr - raw.data());
        return true;
    };

    auto find_startxref = [&]() -> long {
        size_t pos = raw.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            skip_ws(p);

            char* end_ptr = nullptr;
            long v = wz_strtol_fitz(raw.data() + p, &end_ptr, 10);
            if (end_ptr != raw.data() + p && v >= 0) {
                return v;
            }

            if (pos == 0) {
                break;
            }
            pos = raw.rfind("startxref", pos - 1);
        }
        return -1;
    };

    std::set<size_t> visited;
    std::function<void(size_t)> parse_chain;
    parse_chain = [&](size_t offset) {
        if (offset >= raw.size() || visited.find(offset) != visited.end()) {
            return;
        }
        visited.insert(offset);

        size_t pos = offset;
        skip_ws(pos);

        const bool looks_like_xref_table =
            (pos + 4 <= raw.size() && raw.compare(pos, 4, "xref") == 0 &&
             (pos + 4 == raw.size() || std::isspace(static_cast<unsigned char>(raw[pos + 4]))));

        if (looks_like_xref_table) {
            pos += 4;

            while (pos < raw.size()) {
                skip_ws(pos);
                if (pos >= raw.size()) {
                    break;
                }
                if (pos + 7 <= raw.size() && raw.compare(pos, 7, "trailer") == 0) {
                    pos += 7;
                    break;
                }

                size_t hdr_pos = pos;
                long first_obj = 0;
                long obj_count = 0;
                if (!read_int(hdr_pos, first_obj) || !read_int(hdr_pos, obj_count) || obj_count < 0) {
                    break;
                }
                pos = hdr_pos;

                for (long i = 0; i < obj_count && pos < raw.size(); ++i) {
                    skip_ws(pos);
                    size_t line_end = pos;
                    while (line_end < raw.size() && raw[line_end] != '\n' && raw[line_end] != '\r') {
                        ++line_end;
                    }
                    if (line_end <= pos) {
                        while (pos < raw.size() && (raw[pos] == '\r' || raw[pos] == '\n')) {
                            ++pos;
                        }
                        continue;
                    }

                    const std::string line(raw.substr(pos, line_end - pos));
                    long obj_off = -1;
                    int obj_gen = 0;
                    char in_use = 'f';
                    if (wz_parse_xref_line_fitz(line.c_str(), obj_off, obj_gen, in_use)) {
                        const int obj_id = static_cast<int>(first_obj + i);
                        if (in_use == 'n' && obj_id > 0 && obj_off >= 0) {
                            if (xref.find(obj_id) == xref.end()) {
                                xref[obj_id] = static_cast<size_t>(obj_off);
                            }
                        }
                    }

                    pos = line_end;
                    while (pos < raw.size() && (raw[pos] == '\r' || raw[pos] == '\n')) {
                        ++pos;
                    }
                }
            }

            std::string trailer_dict = extract_first_dict_fragment(std::string(raw.substr(pos)));
            if (!trailer_dict.empty()) {
                if (trailer_dict.find("/Encrypt") != std::string::npos) {
                    encrypted_ = true;
                }
                if (root_id <= 0) {
                    int rid = parse_ref_id_after_key(trailer_dict, "/Root");
                    if (rid > 0) {
                        root_id = rid;
                    }
                }

                int xref_stm = parse_int_after_key(trailer_dict, "/XRefStm", -1);
                if (xref_stm >= 0) {
                    parse_chain(static_cast<size_t>(xref_stm));
                }

                int prev = parse_int_after_key(trailer_dict, "/Prev", -1);
                if (prev >= 0) {
                    parse_chain(static_cast<size_t>(prev));
                }
            }

            return;
        }

        WinPdfObject xref_obj = read_obj_from_offset(0, offset);
        if (!xref_obj.is_stream || xref_obj.dict.empty()) {
            return;
        }

        const std::string type_name = parse_name_value_after_key(xref_obj.dict, "/Type");
        if (xref_obj.dict.find("/Encrypt") != std::string::npos) {
            encrypted_ = true;
        }
        const bool is_xref_stream =
            (type_name == "XRef") ||
            (xref_obj.dict.find("/Type/XRef") != std::string::npos) ||
            (xref_obj.dict.find("/Type /XRef") != std::string::npos);
        if (!is_xref_stream) {
            return;
        }

        std::vector<uint8_t> decoded = decode_stream_data(xref_obj.stream, xref_obj.dict, nullptr);
        if (decoded.empty()) {
            decoded = xref_obj.stream;
        }

        std::vector<int> w = parse_int_array_after_key(xref_obj.dict, "/W");
        if (w.size() >= 3 && !decoded.empty()) {
            std::vector<int> index = parse_int_array_after_key(xref_obj.dict, "/Index");
            if (index.empty()) {
                int size_val = parse_int_after_key(xref_obj.dict, "/Size", -1);
                if (size_val > 0) {
                    index.push_back(0);
                    index.push_back(size_val);
                }
            }

            const int w0 = std::max(0, w[0]);
            const int w1 = std::max(0, w[1]);
            const int w2 = std::max(0, w[2]);

            auto read_field = [&](size_t& p, int len) -> uint64_t {
                uint64_t v = 0;
                for (int i = 0; i < len && p < decoded.size(); ++i) {
                    v = (v << 8) | static_cast<uint64_t>(decoded[p++]);
                }
                return v;
            };

            size_t p = 0;
            for (size_t seg = 0; seg + 1 < index.size(); seg += 2) {
                const int start_obj = index[seg];
                const int count = index[seg + 1];
                if (count <= 0) {
                    continue;
                }

                for (int i = 0; i < count; ++i) {
                    if (p >= decoded.size()) {
                        break;
                    }

                    uint64_t f0 = read_field(p, w0);
                    uint64_t f1 = read_field(p, w1);
                    uint64_t f2 = read_field(p, w2);

                    const int type = (w0 == 0) ? 1 : static_cast<int>(f0);
                    const int obj_id = start_obj + i;

                    if (type == 1 && obj_id > 0) {
                        if (xref.find(obj_id) == xref.end()) {
                            xref[obj_id] = static_cast<size_t>(f1);
                        }
                    } else if (type == 2 && obj_id > 0 &&
                               f1 <= static_cast<uint64_t>(std::numeric_limits<int>::max()) &&
                               f2 <= static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                        if (objstm_lookup.find(obj_id) == objstm_lookup.end()) {
                            objstm_lookup[obj_id] = {static_cast<int>(f1), static_cast<int>(f2)};
                        }
                    }
                }
            }
        }

        if (root_id <= 0) {
            int rid = parse_ref_id_after_key(xref_obj.dict, "/Root");
            if (rid > 0) {
                root_id = rid;
            }
        }

        int xref_stm = parse_int_after_key(xref_obj.dict, "/XRefStm", -1);
        if (xref_stm >= 0) {
            parse_chain(static_cast<size_t>(xref_stm));
        }

        int prev = parse_int_after_key(xref_obj.dict, "/Prev", -1);
        if (prev >= 0) {
            parse_chain(static_cast<size_t>(prev));
        }
    };

    const long sx = find_startxref();
    if (sx >= 0) {
        parse_chain(static_cast<size_t>(sx));
    }

    // Fallback for damaged PDFs: scan for object headers and fill only missing entries.
    if (xref.empty()) {
        // Dùng file_view_ (zero-copy) thay vì data vector.
        const char* fv_data = file_view_.data();
        const size_t fv_size = file_view_.size();
        for (size_t i = 1; i + 3 < fv_size; ++i) {
            if (fv_data[i] != 'o' || fv_data[i + 1] != 'b' || fv_data[i + 2] != 'j') {
                continue;
            }
            if (!is_white(static_cast<uint8_t>(fv_data[i - 1]))) {
                continue;
            }

            size_t line_start = i;
            while (line_start > 0 &&
                   fv_data[line_start - 1] != '\n' &&
                   fv_data[line_start - 1] != '\r') {
                --line_start;
            }

            const size_t line_len = (i + 3) - line_start;
            std::string header(fv_data + line_start, line_len);
            int obj_id = 0;
            int obj_gen = 0;
            if (wz_parse_obj_header_fitz(header.c_str(), obj_id, obj_gen) && obj_id > 0) {
                if (xref.find(obj_id) == xref.end()) {
                    xref[obj_id] = line_start;
                }
            }
        }
    }
}

int WinPdfDocument::parse_ref_id_after_key(const std::string& dict, const std::string& key) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return -1;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }

    int id = 0;
    int gen = 0;
    if (wz_parse_obj_ref_fitz(dict.c_str() + pos, id, gen) && id > 0) {
        return id;
    }
    return -1;
}

std::vector<int> WinPdfDocument::parse_ref_array_after_key(const std::string& dict, const std::string& key) {
    std::vector<int> ids;
    auto parse_refs_from_array_text = [&](const std::string& array_src, std::vector<int>& out_ids) {
        size_t arr_start = array_src.find('[');
        size_t arr_end = array_src.find(']', arr_start == std::string::npos ? 0 : arr_start);
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start + 1) {
            return;
        }

        std::string refs_text = array_src.substr(arr_start + 1, arr_end - arr_start - 1);
        const char* p = refs_text.c_str();
        while (*p) {
            int id = 0;
            int gen = 0;
            char r = 0;
            int consumed = 0;
            if (!wz_parse_xref_line_3_fitz(p, id, gen, r, consumed) || consumed <= 0) {
                break;
            }
            if (r == 'R' && id > 0) {
                out_ids.push_back(id);
            }
            p += consumed;
        }
    };

    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return ids;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }

    if (pos < dict.size() && dict[pos] == '[') {
        parse_refs_from_array_text(dict.substr(pos), ids);
        return ids;
    }

    int single = parse_ref_id_after_key(dict, key);
    if (single > 0) {
        WinPdfObject arr_obj = read_obj(single);
        std::vector<int> nested_ids;
        if (!arr_obj.body.empty()) {
            parse_refs_from_array_text(arr_obj.body, nested_ids);
        }
        if (nested_ids.empty() && !arr_obj.dict.empty()) {
            parse_refs_from_array_text(arr_obj.dict, nested_ids);
        }

        if (!nested_ids.empty()) {
            ids.insert(ids.end(), nested_ids.begin(), nested_ids.end());
        } else {
            ids.push_back(single);
        }
    }
    return ids;
}

bool WinPdfDocument::looks_like_text_content_stream(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }

    const size_t sample_len = std::min<size_t>(bytes.size(), 4096);
    std::string s(reinterpret_cast<const char*>(bytes.data()), sample_len);

    const bool has_bt = (s.find("BT") != std::string::npos);
    const bool has_et = (s.find("ET") != std::string::npos);
    const bool has_text_op =
        (s.find("Tj") != std::string::npos) ||
        (s.find("TJ") != std::string::npos) ||
        (s.find("Tf") != std::string::npos) ||
        (s.find("Td") != std::string::npos) ||
        (s.find("Tm") != std::string::npos);

    return has_bt && has_et && has_text_op;
}

bool WinPdfDocument::is_page_object(const std::string& dict) {
    size_t type_pos = dict.find("/Type");
    if (type_pos == std::string::npos) {
        return false;
    }

    size_t page_pos = dict.find("/Page", type_pos);
    if (page_pos == std::string::npos) {
        return false;
    }
    if (page_pos + 5 < dict.size() && dict[page_pos + 5] == 's') {
        return false;
    }
    return true;
}

bool WinPdfDocument::has_flate_filter(const std::string& dict) {
    if (dict.find("/FlateDecode") != std::string::npos) {
        return true;
    }
    if (dict.find("/Filter /Fl") != std::string::npos) {
        return true;
    }
    if (dict.find("/Filter[/Fl") != std::string::npos) {
        return true;
    }
    return false;
}


bool WinPdfDocument::resolve_type0_descendant_font(const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj) {
    descendant_font_obj = {};
    if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") return false;

    std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
    if (descendant_ids.empty()) {
        int single_descendant = parse_ref_id_after_key(font_obj.dict, "/DescendantFonts");
        if (single_descendant > 0) descendant_ids.push_back(single_descendant);
    }
    if (descendant_ids.empty() || descendant_ids.front() <= 0) return false;

    descendant_font_obj = read_obj(descendant_ids.front());
    return descendant_font_obj.id > 0 || !descendant_font_obj.dict.empty() || !descendant_font_obj.body.empty();
}

bool WinPdfDocument::resolve_font_descriptor_dict(const WinPdfObject& font_obj, std::string& descriptor_dict) {
    descriptor_dict.clear();

    auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
        int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
        if (descriptor_ref > 0) {
            WinPdfObject descriptor_obj = read_obj(descriptor_ref);
            descriptor_dict = descriptor_obj.dict;
        }
        if (descriptor_dict.empty()) extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
        return !descriptor_dict.empty();
    };

    if (read_descriptor_from_dict(font_obj.dict)) return true;

    WinPdfObject descendant_font_obj;
    if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
        return read_descriptor_from_dict(descendant_font_obj.dict);
    }
    return false;
}

WinPdfDocument::CidToGidMapData WinPdfDocument::load_cid_to_gid_map(const WinPdfObject& font_obj) {
    CidToGidMapData cid_to_gid;

    WinPdfObject descendant_font_obj;
    if (!resolve_type0_descendant_font(font_obj, descendant_font_obj)) return cid_to_gid;

    std::string cid_to_gid_map_val = parse_name_value_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
    if (cid_to_gid_map_val == "Identity") {
        cid_to_gid.has_map = true;
        cid_to_gid.identity = true;
        return cid_to_gid;
    }

    int cid_to_gid_map_ref = parse_ref_id_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
    if (cid_to_gid_map_ref > 0) {
        WinPdfObject map_obj = read_obj(cid_to_gid_map_ref);
        if (map_obj.is_stream && !map_obj.stream.empty()) {
            std::vector<uint8_t> decoded_map = decode_stream_data(map_obj.stream, map_obj.dict, this);
            if (decoded_map.empty()) decoded_map = map_obj.stream;

            if (!decoded_map.empty() && decoded_map.size() % 2 == 0) {
                cid_to_gid.has_map = true;
                cid_to_gid.identity = false;
                size_t num_entries = decoded_map.size() / 2;
                cid_to_gid.values.resize(num_entries);
                for (size_t i = 0; i < num_entries; ++i) {
                    uint16_t gid = (static_cast<uint16_t>(decoded_map[2 * i]) << 8) | static_cast<uint16_t>(decoded_map[2 * i + 1]);
                    cid_to_gid.values[i] = gid;
                }
            }
        }
    }
    return cid_to_gid;
}

void WinPdfDocument::fill_missing_unicode_from_freetype(const WinPdfObject& font_obj,
                                                        std::unordered_map<int, std::vector<int>>& unicode_map,
                                                        const std::map<int, std::string>& diff_names) {
#ifdef WINEXTRACT_USE_FREETYPE
    FT_Library library = get_freetype_library();
    if (!library) return;

    const bool is_type0_subtype = parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0";
    const CidToGidMapData cid_to_gid = load_cid_to_gid_map(font_obj);

    std::string descriptor_dict;
    resolve_font_descriptor_dict(font_obj, descriptor_dict);

    std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
    if (base_font_name.empty() && is_type0_subtype) {
        WinPdfObject descendant_font_obj;
        if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            base_font_name = parse_name_value_after_key(descendant_font_obj.dict, "/BaseFont");
        }
    }
    base_font_name = normalize_pdf_font_name(base_font_name);

    FT_Face face = nullptr;
    std::vector<uint8_t> font_bytes;

    if (!descriptor_dict.empty()) {
        int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
        if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
        if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

        if (font_file_ref > 0) {
            WinPdfObject font_file_obj = read_obj(font_file_ref);
            if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                if (font_bytes.empty()) font_bytes = font_file_obj.stream;
            }
        }
    }

    uint64_t font_hash = fnv1a_hash_bytes(font_bytes);
    if (!base_font_name.empty()) font_hash ^= fnv1a_hash_string(base_font_name);
    std::shared_ptr<std::unordered_map<unsigned int, int>> cached_gid_map;

    {
        std::shared_lock<std::shared_mutex> lock(g_global_freetype_cache_mutex);
        auto it = g_global_freetype_cache.find(font_hash);
        if (it != g_global_freetype_cache.end()) cached_gid_map = it->second;
    }

    if (!cached_gid_map) {
        if (!font_bytes.empty()) {
            FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(font_bytes.data()), static_cast<FT_Long>(font_bytes.size()), 0, &face);
        }
        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) break;
            }
        }
        if (!face) return;

        auto gid_to_unicode = std::make_shared<std::unordered_map<unsigned int, int>>();

        auto collect_gid_unicode = [&]() {
            if (face->charmap == nullptr) return;
            FT_UInt gid = 0;
            FT_ULong charcode = FT_Get_First_Char(face, &gid);
            while (gid != 0) {
                if (charcode > 0 && charcode <= 0x10FFFFUL && gid_to_unicode->find(gid) == gid_to_unicode->end()) {
                    (*gid_to_unicode)[gid] = static_cast<int>(charcode);
                }
                charcode = FT_Get_Next_Char(face, charcode, &gid);
            }
            if (FT_HAS_GLYPH_NAMES(face)) {
                for (FT_UInt g = 0; g < (FT_UInt)face->num_glyphs; ++g) {
                    if (gid_to_unicode->find(g) == gid_to_unicode->end()) {
                        char gname[64];
                        if (FT_Get_Glyph_Name(face, g, gname, sizeof(gname)) == 0) {
                            int cp = glyph_name_to_unicode_fitz(std::string(gname));
                            if (cp > 0) (*gid_to_unicode)[g] = cp;
                        }
                    }
                }
            }
        };

        FT_CharMap saved_charmap = face->charmap;
        if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) collect_gid_unicode();
        if (face->num_charmaps > 0 && face->charmaps != nullptr) {
            for (int ci = 0; ci < face->num_charmaps; ++ci) {
                FT_CharMap cmap = face->charmaps[ci];
                if (cmap == nullptr || cmap == face->charmap) continue;
                if (FT_Set_Charmap(face, cmap) == 0) collect_gid_unicode();
            }
        }
        if (saved_charmap != nullptr && face->charmap != saved_charmap) {
            FT_Set_Charmap(face, saved_charmap);
        }

        cached_gid_map = gid_to_unicode;
        {
            std::unique_lock<std::shared_mutex> lock(g_global_freetype_cache_mutex);
            g_global_freetype_cache[font_hash] = cached_gid_map;
        }
    }

    auto lookup_gid_for_code = [&](int code) -> FT_UInt {
        if (code < 0) return 0;
        if (is_type0_subtype) {
            if (cid_to_gid.has_map) {
                if (cid_to_gid.identity) return static_cast<FT_UInt>(code);
                if (static_cast<size_t>(code) < cid_to_gid.values.size()) {
                    return static_cast<FT_UInt>(cid_to_gid.values[static_cast<size_t>(code)]);
                }
            }
            return 0;
        }
        FT_UInt gid = 0;
        if (face) {
            auto diff_it = diff_names.find(code);
            if (diff_it != diff_names.end() && FT_HAS_GLYPH_NAMES(face)) {
                gid = FT_Get_Name_Index(face, const_cast<FT_String*>(diff_it->second.c_str()));
            }
            if (gid == 0) {
                char char_name[32];
                snprintf(char_name, sizeof(char_name), "char%02X", code);
                gid = FT_Get_Name_Index(face, char_name);
            }
            
        }
        return gid;
    };

    std::vector<int> codes_to_process;
    if (is_type0_subtype) {
        if (cid_to_gid.has_map) {
            int max_code = cid_to_gid.identity ? 65535 : static_cast<int>(cid_to_gid.values.size());
            for (int c = 0; c < max_code; ++c) codes_to_process.push_back(c);
        }
    } else {
        for (int c = 0; c < 256; ++c) codes_to_process.push_back(c);
    }

    for (int code : codes_to_process) {
        if (unicode_map.find(code) != unicode_map.end() && !unicode_map[code].empty() && unicode_map[code][0] != 0xFFFD) {
            continue; // Already has a valid mapping
        }

        int cp = -1;
        if (is_type0_subtype) {
            FT_UInt gid = lookup_gid_for_code(code);
            if (gid > 0) {
                auto git = cached_gid_map->find(gid);
                if (git != cached_gid_map->end()) cp = git->second;
            }
        } else {
            auto diff_it = diff_names.find(code);
            if (diff_it != diff_names.end()) cp = glyph_name_to_unicode_fitz(diff_it->second);
            if (cp <= 0 && face) {
                FT_UInt gid = lookup_gid_for_code(code);
                if (gid > 0) {
                    auto git = cached_gid_map->find(gid);
                    if (git != cached_gid_map->end()) cp = git->second;
                }
            }
        }
        if (cp > 0 && cp <= 0x10FFFF) unicode_map[code] = {cp};
    }
    if (face) FT_Done_Face(face);
#endif
}

std::map<std::string, int> WinPdfDocument::get_page_font_name_to_id(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    std::map<std::string, int> out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) return out;

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        if (!resources_dict.empty()) break;
        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) return out;

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    if (font_dict.empty()) return out;

    return parse_font_refs_from_dict(font_dict);
}

std::shared_ptr<const std::unordered_map<int, WinUnicodeSequence>> WinPdfDocument::get_font_unicode_map_by_id(int font_obj_id) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    auto it = cached_unicode_maps.find(font_obj_id);
    if (it != cached_unicode_maps.end()) return it->second;
    return nullptr;
}

bool WinPdfDocument::patch_font_unicode_map_lazily(int font_obj_id) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    auto it = cached_unicode_maps.find(font_obj_id);
    if (it == cached_unicode_maps.end()) return false;

    std::unordered_map<int, std::vector<int>> cmap = *(it->second);
    WinPdfObject font_obj = read_obj(font_obj_id);
    
    std::string encoding_dict;
    std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
    if (encoding_name.empty()) {
        int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
        if (encoding_ref > 0) {
            WinPdfObject enc_obj = read_obj(encoding_ref);
            encoding_dict = enc_obj.dict;
            encoding_name = parse_name_value_after_key(enc_obj.dict, "/BaseEncoding");
            if (encoding_name.empty()) encoding_name = parse_name_value_after_key(enc_obj.body, "/BaseEncoding");
        }
    }
    if (encoding_dict.empty()) {
        extract_inline_dict_after_key(font_obj.dict, "/Encoding", encoding_dict);
        if (!encoding_dict.empty() && encoding_name.empty()) {
            encoding_name = parse_name_value_after_key(encoding_dict, "/BaseEncoding");
        }
    }
    std::map<int, std::string> diff_names;
    if (!encoding_dict.empty()) {
        std::map<int, int> dummy;
        apply_differences_to_map(encoding_dict, dummy, &diff_names);
    }

    fill_missing_unicode_from_freetype(font_obj, cmap, diff_names);

    cached_unicode_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>(std::move(cmap));
    return true;
}

} // namespace WinExtract
