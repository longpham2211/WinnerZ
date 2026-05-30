#pragma once

#include <cstdint>
#include <vector>

#include "../extract_text/pdf_engine.hpp"

namespace winnerz {

// Input redact zone in top-down page coordinates (origin at page top-left).
struct WinRedactZone_TopDown {
	WinExtract::Rect rect{0, 0, 0, 0};
};

// Redact text content on a single page and return a filtered decoded content stream.
std::vector<uint8_t> WinnerZ_RedactPage(
	WinExtract::WinPdfDocument& doc,
	int page_idx,
	const std::vector<WinRedactZone_TopDown>& zones_topdown,
	const float page_ctm[6] = nullptr);

}  // namespace winnerz
