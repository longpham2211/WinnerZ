#ifndef WINEXTRACT_MICRO_OCR_HPP
#define WINEXTRACT_MICRO_OCR_HPP

#include <string>
#include <vector>

// Freetype is needed for rendering the glyph
#ifdef WINEXTRACT_USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H

namespace WinExtract {

// Runs a very simple template-matching OCR on the glyph drawing to recover its character.
// Returns a vector of Unicode codepoints on success, or empty vector if no match.
std::vector<int> run_micro_ocr_on_glyph(FT_Face face, int glyph_id);

} // namespace WinExtract
#endif

namespace WinExtract {

// Check if a sequence of Unicode points (which might be in PUA or wrong block)
// matches a known VNI or TCVN3 heuristic, and decode them in-place.
// Returns true if heuristics were applied.
bool apply_heuristic_vni_tcvn3(const std::string& font_name, int code, int& out_unicode);

} // namespace WinExtract

#endif // WINEXTRACT_MICRO_OCR_HPP
