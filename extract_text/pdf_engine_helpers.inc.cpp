namespace WinExtract {

static std::shared_mutex g_global_freetype_cache_mutex;

struct CachedFreetypeData {
    std::shared_ptr<std::unordered_map<unsigned int, int>> gid_to_unicode;
    std::shared_ptr<std::unordered_map<std::string, unsigned int>> name_to_gid;
    bool is_system_font = false;
};
static std::unordered_map<uint64_t, std::shared_ptr<CachedFreetypeData>> g_global_freetype_cache;


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
            "/usr/share/color/icc/default_rgb.icc",
            "/usr/share/color/icc/sRGB.icc"
        });
        cmyk_profile = open_profile({
            "/usr/share/color/icc/default_cmyk.icc",
            "/usr/share/color/icc/RSWOP.icc"
        });
        lab_profile = open_profile({
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
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c += ('a' - 'A'); 
        }
    }
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
            candidates.push_back(prefix + "resources/fonts/urw/" + std::string(file_name));
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
    return wz_strtod(n.c_str(), nullptr);
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
    // PDF spec: new_tlm = translate(tx, ty) Ä‚â€” old_tlm
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
            // Pos is already at the character that stopped the scan (e.g. '/').
            // The next iteration of the outer loop will process it.
            continue;
        }

        while (pos < font_dict.size() && std::isspace(static_cast<unsigned char>(font_dict[pos]))) {
            ++pos;
        }

        int id = 0;
        int gen = 0;
        if (pos < font_dict.size() && wz_parse_obj_ref(font_dict.c_str() + pos, id, gen) && id > 0) {
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

static std::unordered_map<int, std::vector<int>> parse_tounicode_cmap_internal(const std::vector<uint8_t>& bytes) {
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

static std::vector<WinCodeSpaceRange> parse_cmap_codespace_ranges_internal(const std::vector<uint8_t>& bytes) {
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
    if (wz_parse_obj_ref(t.c_str(), out_ref_id, gen) && out_ref_id > 0) {
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

static bool parse_float_token_strict(const std::string& token, float& out_value) {
    const std::string t = trim_copy(token);
    if (t.empty()) {
        return false;
    }

    char* end_ptr = nullptr;
    const double v = wz_strtod(t.c_str(), &end_ptr);
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

static std::vector<float> parse_float_array_expr(const std::string& expr) {
    std::vector<float> out;
    const std::string t = trim_copy(expr);
    if (t.empty() || t[0] != '[') {
        return out;
    }

    const std::vector<std::string> items = split_pdf_array_items(t);
    out.reserve(items.size());
    for (const std::string& item : items) {
        float v = 0.0f;
        if (parse_float_token_strict(item, v)) {
            out.push_back(v);
        }
    }
    return out;
}

static std::vector<int> parse_int_array_expr(const std::string& expr) {
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

static bool parse_lab_whitepoint_from_expr(const std::string& dict_expr,
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

    const std::vector<float> wp = parse_float_array_expr(it->second);
    if (wp.size() < 3) {
        return false;
    }

    out_x = wp[0];
    out_y = wp[1];
    out_z = wp[2];
    return true;
}

static int parse_int_from_dict_entries(const std::map<std::string, std::string>& entries,
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

static float parse_float_from_dict_entries(const std::map<std::string, std::string>& entries,
                                                const std::string& key,
                                                float fallback) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return fallback;
    }
    float out = 0.0f;
    if (parse_float_token_strict(it->second, out)) {
        return out;
    }
    return fallback;
}

static std::vector<float> parse_float_array_from_dict_entries(const std::map<std::string, std::string>& entries,
                                                                   const std::string& key) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return {};
    }
    return parse_float_array_expr(it->second);
}

static std::vector<int> parse_int_array_from_dict_entries(const std::map<std::string, std::string>& entries,
                                                               const std::string& key) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        return {};
    }
    return parse_int_array_expr(it->second);
}

static int infer_component_count_from_kind(WinColorSpaceKind kind) {
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

static bool parse_tint_function_from_dict(const std::string& function_dict,
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

    const int function_type = parse_int_from_dict_entries(entries, "FunctionType", -1);
    if (function_type == 2) {
        std::vector<float> c0 = parse_float_array_from_dict_entries(entries, "C0");
        std::vector<float> c1 = parse_float_array_from_dict_entries(entries, "C1");

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

        std::vector<float> domain = parse_float_array_from_dict_entries(entries, "Domain");
        if (domain.size() < 2) {
            domain = {0.0f, 1.0f};
        }

        out_space.has_tint_transform = true;
        out_space.tint_function_type = 2;
        out_space.tint_input_count = 1;
        out_space.tint_output_count = out_count;
        out_space.tint_n = parse_float_from_dict_entries(entries, "N", 1.0f);
        out_space.tint_domain = std::move(domain);
        out_space.tint_range = parse_float_array_from_dict_entries(entries, "Range");
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
        std::vector<int> size = parse_int_array_from_dict_entries(entries, "Size");
        if (size.empty()) {
            return false;
        }
        for (int dim : size) {
            if (dim <= 0) {
                return false;
            }
        }

        const int bits_per_sample = parse_int_from_dict_entries(entries, "BitsPerSample", 0);
        if (bits_per_sample <= 0 || bits_per_sample > 32) {
            return false;
        }

        int input_count = static_cast<int>(size.size());
        if (expected_input_count > 0) {
            input_count = expected_input_count;
        }

        std::vector<float> domain = parse_float_array_from_dict_entries(entries, "Domain");
        if (domain.size() != static_cast<size_t>(input_count * 2)) {
            domain.assign(static_cast<size_t>(input_count * 2), 0.0f);
            for (int i = 0; i < input_count; ++i) {
                domain[static_cast<size_t>(2 * i + 1)] = 1.0f;
            }
        }

        std::vector<float> range = parse_float_array_from_dict_entries(entries, "Range");
        int output_count = static_cast<int>(range.size() / 2);
        if (output_count <= 0) {
            output_count = expected_output_count;
        }
        if (output_count <= 0) {
            output_count = 1;
        }

        std::vector<float> encode = parse_float_array_from_dict_entries(entries, "Encode");
        if (encode.size() != static_cast<size_t>(input_count * 2)) {
            encode.clear();
            encode.reserve(static_cast<size_t>(input_count * 2));
            for (int i = 0; i < input_count; ++i) {
                const int dim = (i < static_cast<int>(size.size())) ? size[static_cast<size_t>(i)] : 1;
                encode.push_back(0.0f);
                encode.push_back(static_cast<float>(std::max(dim - 1, 0)));
            }
        }

        std::vector<float> decode = parse_float_array_from_dict_entries(entries, "Decode");
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

static bool parse_tint_transform_expr(const std::string& expr,
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

        return parse_tint_function_from_dict(
            function_dict,
            function_stream,
            expected_input_count,
            expected_output_count,
            out_space);
    }

    if (t.rfind("<<", 0) == 0) {
        return parse_tint_function_from_dict(t, {}, expected_input_count, expected_output_count, out_space);
    }

    return false;
}

static bool evaluate_tint_transform(const WinColorSpaceDef& space,
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

static bool read_be_u16(const std::vector<uint8_t>& data, size_t off, uint16_t& out) {
    if (off + 2 > data.size()) {
        return false;
    }
    out = static_cast<uint16_t>((static_cast<uint16_t>(data[off]) << 8) |
                                static_cast<uint16_t>(data[off + 1]));
    return true;
}

static bool read_be_u32(const std::vector<uint8_t>& data, size_t off, uint32_t& out) {
    if (off + 4 > data.size()) {
        return false;
    }
    out = (static_cast<uint32_t>(data[off]) << 24) |
          (static_cast<uint32_t>(data[off + 1]) << 16) |
          (static_cast<uint32_t>(data[off + 2]) << 8) |
          static_cast<uint32_t>(data[off + 3]);
    return true;
}

static bool read_be_s15fixed16(const std::vector<uint8_t>& data, size_t off, float& out) {
    uint32_t raw = 0;
    if (!read_be_u32(data, off, raw)) {
        return false;
    }
    const int32_t sval = static_cast<int32_t>(raw);
    out = static_cast<float>(static_cast<double>(sval) / 65536.0);
    return true;
}

static bool parse_icc_xyz_tag(const std::vector<uint8_t>& profile,
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

    return read_be_s15fixed16(profile, tag_off + 8, x) &&
           read_be_s15fixed16(profile, tag_off + 12, y) &&
           read_be_s15fixed16(profile, tag_off + 16, z);
}

static bool parse_icc_curve_tag(const std::vector<uint8_t>& profile,
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
        if (!read_be_u32(profile, tag_off + 8, count)) {
            return false;
        }

        if (count == 0) {
            out_curve.type = WinIccCurveType::Identity;
            return true;
        }

        if (count == 1) {
            uint16_t gamma_u8f8 = 0;
            if (!read_be_u16(profile, tag_off + 12, gamma_u8f8)) {
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
            if (!read_be_u16(profile, tag_off + 12 + i * 2, v)) {
                return false;
            }
            out_curve.table[i] = static_cast<float>(v) / 65535.0f;
        }
        return true;
    }

    if (type == "para") {
        uint16_t fn_type = 0;
        if (!read_be_u16(profile, tag_off + 8, fn_type)) {
            return false;
        }
        if (fn_type == 0 && tag_len >= 16 && tag_off + 16 <= profile.size()) {
            float gamma = 1.0f;
            if (!read_be_s15fixed16(profile, tag_off + 12, gamma)) {
                return false;
            }
            out_curve.type = WinIccCurveType::Gamma;
            out_curve.gamma = (gamma > 0.0f) ? gamma : 1.0f;
            return true;
        }
    }

    return false;
}

static bool parse_icc_rgb_profile_stream(const std::vector<uint8_t>& profile,
                                              WinColorSpaceDef& out_space) {
    if (profile.size() < 132) {
        return false;
    }

    const std::string color_space(reinterpret_cast<const char*>(profile.data() + 16), 4);
    if (color_space != "RGB ") {
        return false;
    }

    uint32_t tag_count = 0;
    if (!read_be_u32(profile, 128, tag_count)) {
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
        if (!read_be_u32(profile, entry_off + 4, off_u32) ||
            !read_be_u32(profile, entry_off + 8, len_u32)) {
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
    if (!parse_icc_xyz_tag(profile, it_rxyz->second.off, it_rxyz->second.len, rX, rY, rZ) ||
        !parse_icc_xyz_tag(profile, it_gxyz->second.off, it_gxyz->second.len, gX, gY, gZ) ||
        !parse_icc_xyz_tag(profile, it_bxyz->second.off, it_bxyz->second.len, bX, bY, bZ)) {
        return false;
    }

    WinIccCurve trc_r;
    WinIccCurve trc_g;
    WinIccCurve trc_b;
    if (!parse_icc_curve_tag(profile, it_rtrc->second.off, it_rtrc->second.len, trc_r) ||
        !parse_icc_curve_tag(profile, it_gtrc->second.off, it_gtrc->second.len, trc_g) ||
        !parse_icc_curve_tag(profile, it_btrc->second.off, it_btrc->second.len, trc_b)) {
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
                parse_icc_rgb_profile_stream(icc_profile, out);
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
                if (parse_lab_whitepoint_from_expr(items[1], wx, wy, wz)) {
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
                    expected_outputs = infer_component_count_from_kind(out.alt_kind);
                }
                parse_tint_transform_expr(items[3], resolver, out.component_count, expected_outputs, out, depth + 1);
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
                    expected_outputs = infer_component_count_from_kind(out.alt_kind);
                }
                parse_tint_transform_expr(items[3], resolver, out.component_count, expected_outputs, out, depth + 1);
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

static std::string parse_name_or_string_value_after_key(const std::string& dict, const std::string& key) {
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

static std::string build_cid_collection_name_from_cidsysteminfo(const std::string& cid_system_info_dict) {
    if (cid_system_info_dict.empty()) {
        return {};
    }

    std::string registry = trim_copy(parse_name_or_string_value_after_key(cid_system_info_dict, "/Registry"));
    std::string ordering = trim_copy(parse_name_or_string_value_after_key(cid_system_info_dict, "/Ordering"));
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

        int cp = glyph_name_to_unicode(std::string(gname));
        if (cp > 0 && cp <= 0x10FFFF) {
            m[i] = cp;
        }
    }
    return m;
}

static std::map<int, int> make_mac_roman_encoding_map() {
    return make_glyph_table_encoding_map(wz_glyph_name_from_mac_roman);
}

static std::map<int, int> make_win_ansi_encoding_map() {
    return make_glyph_table_encoding_map(wz_glyph_name_from_win_ansi);
}

static std::map<int, int> make_standard_encoding_map() {
    return make_glyph_table_encoding_map(wz_glyph_name_from_adobe_standard);
}

static std::map<int, int> make_mac_expert_encoding_map() {
    return make_glyph_table_encoding_map(wz_glyph_name_from_mac_expert);
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
                int cp = glyph_name_to_unicode(gname);
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
