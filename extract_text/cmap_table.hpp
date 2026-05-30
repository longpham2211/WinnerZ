#include <unordered_map>
#pragma once

#include <map>
#include <string>
#include <vector>

namespace WinExtract {

// Returns a Unicode mapping for a known built-in CMap name (e.g. Adobe-GB1-UCS2).
// The returned reference is stable for the process lifetime.
const std::unordered_map<int, std::vector<int>>& load_system_unicode_cmap_by_name_fitz(const std::string& cmap_name);

// Collection helper for CID fonts (e.g. Adobe-GB1 -> Adobe-GB1-UCS2).
const std::unordered_map<int, std::vector<int>>& load_collection_unicode_cmap_fitz(const std::string& collection);

} // namespace WinExtract
