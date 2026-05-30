#include "xref.hpp"

#include <cctype>

#include "parse.hpp"

namespace WinExtract {

bool wz_parse_xref_line_fitz(const char* p, long& offset, int& gen, char& type) {
    char* e1;
    long v1 = wz_strtol_fitz(p, &e1, 10);
    if (e1 == p || v1 < 0) {
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
    if (*e2 == 'n' || *e2 == 'f') {
        offset = v1;
        gen = static_cast<int>(v2);
        type = *e2;
        return true;
    }
    return false;
}

bool wz_parse_xref_line_3_fitz(const char* p, int& id, int& gen, char& r, int& consumed) {
    const char* start = p;

    char* e1;
    long v1 = wz_strtol_fitz(p, &e1, 10);
    if (e1 == p) {
        return false;
    }

    char* e2;
    long v2 = wz_strtol_fitz(e1, &e2, 10);
    if (e2 == e1) {
        return false;
    }

    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) {
        ++e2;
    }
    if (!*e2) {
        return false;
    }

    const char tp = *e2;
    ++e2;

    consumed = static_cast<int>(e2 - start);
    id = static_cast<int>(v1);
    gen = static_cast<int>(v2);
    r = tp;
    return true;
}

} // namespace WinExtract
