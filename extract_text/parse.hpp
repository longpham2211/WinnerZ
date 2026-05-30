#pragma once

namespace WinExtract {

long wz_strtol_fitz(const char* nptr, char** endptr, int base);
bool wz_parse_obj_ref_fitz(const char* p, int& id, int& gen);
bool wz_parse_obj_header_fitz(const char* p, int& id, int& gen);

} // namespace WinExtract
