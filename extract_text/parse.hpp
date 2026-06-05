#pragma once

namespace WinExtract {

long wz_strtol(const char* nptr, char** endptr, int base);
bool wz_parse_obj_ref(const char* p, int& id, int& gen);
bool wz_parse_obj_header(const char* p, int& id, int& gen);

} // namespace WinExtract
