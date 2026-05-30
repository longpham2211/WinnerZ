#include "parse.hpp"

#include <cctype>

namespace WinExtract {

long wz_strtol_fitz(const char* nptr, char** endptr, int base) {
    long result = 0;
    int sign = 1;
    const char* p = nptr;

    while (*p && std::isspace(static_cast<unsigned char>(*p))) {
        ++p;
    }
    if (*p == '-') {
        sign = -1;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    if (!std::isdigit(static_cast<unsigned char>(*p))) {
        if (endptr) {
            *endptr = const_cast<char*>(nptr);
        }
        return 0;
    }

    while (*p && std::isdigit(static_cast<unsigned char>(*p))) {
        result = result * base + (*p - '0');
        ++p;
    }

    if (endptr) {
        *endptr = const_cast<char*>(p);
    }
    return result * sign;
}

bool wz_parse_obj_ref_fitz(const char* p, int& id, int& gen) {
    char* e1;
    long v1 = wz_strtol_fitz(p, &e1, 10);
    if (e1 == p || v1 <= 0) {
        return false;
    }

    char* e2;
    long v2 = wz_strtol_fitz(e1, &e2, 10);
    if (e2 == e1 || v2 < 0) {
        return false;
    }

    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) {
        ++e2;
    }
    if (*e2 == 'R') {
        id = static_cast<int>(v1);
        gen = static_cast<int>(v2);
        return true;
    }
    return false;
}

bool wz_parse_obj_header_fitz(const char* p, int& id, int& gen) {
    char* e1;
    long v1 = wz_strtol_fitz(p, &e1, 10);
    if (e1 == p || v1 <= 0) {
        return false;
    }

    char* e2;
    long v2 = wz_strtol_fitz(e1, &e2, 10);
    if (e2 == e1 || v2 < 0) {
        return false;
    }

    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) {
        ++e2;
    }
    if (*e2 == 'o' && *(e2 + 1) == 'b' && *(e2 + 2) == 'j') {
        id = static_cast<int>(v1);
        gen = static_cast<int>(v2);
        return true;
    }
    return false;
}

} // namespace WinExtract
