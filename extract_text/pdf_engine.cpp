// =============================================================================
// pdf_engine.cpp — Orchestrator
//
// This file is intentionally kept short. The implementation is split into
// focused internal-include files (*.inc.cpp) that are compiled as a single
// translation unit. This preserves all global state ordering guarantees and
// avoids any ODR / linker issues while making the codebase easier to navigate.
//
// Section layout:
//   pdf_engine_utils.inc.cpp     — global OCR cache, FNV hash, wz_strtol/strtod,
//                                  wz_parse_* helpers (outside namespace)
//   pdf_engine_helpers.inc.cpp   — namespace WinExtract { namespace { ... } }
//                                  structs (PdfToken, TextState, PdfDecodeParams),
//                                  static helper functions, FreeType/LCMS2 state,
//                                  font helpers, color space helpers.
//                                  Opens: namespace WinExtract {
//                                  Closes: } // namespace  (anonymous namespace only)
//   pdf_engine_interpreter.inc.cpp — WinPdfInterpreter::run() implementation
//                                    (must be inside namespace WinExtract)
//   pdf_engine_parser.inc.cpp    — All WinPdfDocument::* method implementations
//                                  Closes: } // namespace WinExtract
// =============================================================================

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
#include "tex_font_mappings.hpp"
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
#include <mutex>
#include <cstring>
#include <map>
#include <set>
#include <functional>
#include <limits>
#include <cmath>
#include <sstream>

// --- Section 1: Global cache + wz_* parsers (outside any namespace) ---
#include "pdf_engine_utils.inc.cpp"

// --- Section 2: WinExtract anonymous namespace helpers ---
// Opens: namespace WinExtract { namespace { ... } }
// Closes anonymous namespace but leaves WinExtract open
#include "pdf_engine_helpers.inc.cpp"

// --- Section 3: WinPdfInterpreter::run() ---
// Still inside namespace WinExtract (helpers opened it, parser closes it)
#include "pdf_engine_interpreter.inc.cpp"

// --- Section 4: WinPdfDocument::* method implementations ---
// Closes: } // namespace WinExtract
#include "pdf_engine_parser.inc.cpp"



/* 
    TODO
    # TODO List

- `[ ]` Fix `build_images` logic in `e:\thuvien_winnerz\my-lib\extract_text\pdf_engine_parser.inc.cpp`:
  - Restore `recursion_guard.erase(xobj_id)` to allow parsing of valid DAG image structures.
  - Implement a `global_image_cache` (similar to `global_form_cache` for `build_forms`) to prevent exponential memory bloat during a Billion Laughs attack.

*/