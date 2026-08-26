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

// Synchronize real number reading format
static double wz_strtod(const char* str, char** endptr = nullptr) {
    double result = 0.0;
    double fraction = 0.0;
    double divisor = 1.0;
    int sign = 1;
    int exp_sign = 1;
    int exp = 0;
    const char* p = str;

    while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;

    if (*p == '-') { sign = -1; ++p; } 
    else if (*p == '+') { ++p; }

    const char* start_digits = p;

    while (*p >= '0' && *p <= '9') {
        result = result * 10.0 + (*p - '0');
        ++p;
    }

    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            fraction = fraction * 10.0 + (*p - '0');
            divisor *= 10.0;
            ++p;
        }
    }

    if (p == start_digits && divisor == 1.0) {
        if (endptr) *endptr = const_cast<char*>(str);
        return 0.0;
    }

    result += fraction / divisor;
    result *= sign;

    if (*p == 'e' || *p == 'E') {
        const char* exp_ptr = p + 1;
        if (*exp_ptr == '-') { exp_sign = -1; ++exp_ptr; } 
        else if (*exp_ptr == '+') { ++exp_ptr; }

        if (*exp_ptr >= '0' && *exp_ptr <= '9') {
            while (*exp_ptr >= '0' && *exp_ptr <= '9') {
                exp = exp * 10 + (*exp_ptr - '0');
                ++exp_ptr;
            }
            
            double scale = 1.0;
            double base = 10.0;
            int temp_exp = exp;
            
            while (temp_exp > 0) {
                if (temp_exp & 1) { 
                    scale *= base;
                }
                base *= base;      
                temp_exp >>= 1;
            }

            if (exp_sign < 0) {
                result /= scale; 
            } else {
                result *= scale;
            }
            p = exp_ptr;
        }
    }

    if (endptr) *endptr = const_cast<char*>(p);
    return result;
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
#include FT_ADVANCES_H
#include FT_OUTLINE_H
#include FT_BBOX_H
#endif

