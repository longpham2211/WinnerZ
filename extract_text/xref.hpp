#pragma once

namespace WinExtract {

bool wz_parse_xref_line_fitz(const char* p, long& offset, int& gen, char& type);
bool wz_parse_xref_line_3_fitz(const char* p, int& id, int& gen, char& r, int& consumed);

} // namespace WinExtract
