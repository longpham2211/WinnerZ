#include "cmap_table.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace WinExtract {
namespace {

struct pdf_storable {
	int refs;
	void (*drop)(void);
};

struct pdf_codespace {
	int n;
	uint32_t low;
	uint32_t high;
};

struct pdf_range {
	uint32_t low;
	uint32_t high;
	uint32_t out;
};

struct pdf_xrange {
	uint32_t low;
	uint32_t high;
	uint32_t out;
};

struct pdf_mrange {
	uint32_t low;
	uint32_t out;
};

static void pdf_drop_cmap_imp(void) {
}

struct pdf_cmap {
	pdf_storable storable;
	const char* cmapname;
	const char* usecmap_name;
	pdf_cmap* usecmap;
	int wmode;
	int codespace_len;
	pdf_codespace codespaces[40];

	int rlen;
	int rcap;
	pdf_range* ranges;

	int xlen;
	int xcap;
	pdf_xrange* xranges;

	int mlen;
	int mcap;
	pdf_mrange* mranges;

	int tlen;
	int tcap;
	int* table;

	int splay_len;
	int splay_cap;
	int splay_root;
	void* splay;
};

#include "cmaps/Adobe-CNS1-UCS2.h"
#include "cmaps/Adobe-GB1-UCS2.h"
#include "cmaps/Adobe-Japan1-UCS2.h"
#include "cmaps/Adobe-Korea1-UCS2.h"
#include "cmaps/Identity-H.h"
#include "cmaps/Identity-V.h"
#include "cmaps/TrueType-UCS2.h"
#include "cmaps/UniCNS-UCS2-H.h"
#include "cmaps/UniCNS-UCS2-V.h"
#include "cmaps/UniCNS-UTF16-H.h"
#include "cmaps/UniCNS-UTF16-V.h"
#include "cmaps/UniGB-UCS2-H.h"
#include "cmaps/UniGB-UCS2-V.h"
#include "cmaps/UniGB-UTF16-H.h"
#include "cmaps/UniGB-UTF16-V.h"
#include "cmaps/UniJIS-UCS2-H.h"
#include "cmaps/UniJIS-UCS2-V.h"
#include "cmaps/UniJIS-UCS2-HW-H.h"
#include "cmaps/UniJIS-UCS2-HW-V.h"
#include "cmaps/UniJIS-UTF16-H.h"
#include "cmaps/UniJIS-UTF16-V.h"
#include "cmaps/UniKS-UCS2-H.h"
#include "cmaps/UniKS-UCS2-V.h"
#include "cmaps/UniKS-UTF16-H.h"
#include "cmaps/UniKS-UTF16-V.h"

struct CMapEntry {
	const char* name;
	const pdf_cmap* cmap;
};

static const CMapEntry kUnicodeCMapTable[] = {
	{"Adobe-CNS1-UCS2", &cmap_Adobe_CNS1_UCS2},
	{"Adobe-GB1-UCS2", &cmap_Adobe_GB1_UCS2},
	{"Adobe-Japan1-UCS2", &cmap_Adobe_Japan1_UCS2},
	{"Adobe-Korea1-UCS2", &cmap_Adobe_Korea1_UCS2},
	{"Identity-H", &cmap_Identity_H},
	{"Identity-V", &cmap_Identity_V},
	{"TrueType-UCS2", &cmap_TrueType_UCS2},
	{"UniCNS-UCS2-H", &cmap_UniCNS_UCS2_H},
	{"UniCNS-UCS2-V", &cmap_UniCNS_UCS2_V},
	{"UniCNS-UTF16-H", &cmap_UniCNS_UTF16_H},
	{"UniCNS-UTF16-V", &cmap_UniCNS_UTF16_V},
	{"UniGB-UCS2-H", &cmap_UniGB_UCS2_H},
	{"UniGB-UCS2-V", &cmap_UniGB_UCS2_V},
	{"UniGB-UTF16-H", &cmap_UniGB_UTF16_H},
	{"UniGB-UTF16-V", &cmap_UniGB_UTF16_V},
	{"UniJIS-UCS2-H", &cmap_UniJIS_UCS2_H},
	{"UniJIS-UCS2-V", &cmap_UniJIS_UCS2_V},
	{"UniJIS-UCS2-HW-H", &cmap_UniJIS_UCS2_HW_H},
	{"UniJIS-UCS2-HW-V", &cmap_UniJIS_UCS2_HW_V},
	{"UniJIS-UTF16-H", &cmap_UniJIS_UTF16_H},
	{"UniJIS-UTF16-V", &cmap_UniJIS_UTF16_V},
	{"UniKS-UCS2-H", &cmap_UniKS_UCS2_H},
	{"UniKS-UCS2-V", &cmap_UniKS_UCS2_V},
	{"UniKS-UTF16-H", &cmap_UniKS_UTF16_H},
	{"UniKS-UTF16-V", &cmap_UniKS_UTF16_V},
};

static const pdf_cmap* find_unicode_cmap_by_name(const std::string& name) {
	for (const auto& entry : kUnicodeCMapTable) {
		if (name == entry.name) {
			return entry.cmap;
		}
	}
	return nullptr;
}

static std::unordered_map<int, std::vector<int>> build_unicode_map_from_cmap(const pdf_cmap& cmap) {
	std::unordered_map<int, std::vector<int>> out;

	for (int i = 0; i < cmap.rlen; ++i) {
		const auto& r = cmap.ranges[i];
		if (r.high < r.low) {
			continue;
		}
		for (uint32_t code = r.low; code <= r.high; ++code) {
			const uint32_t cp = r.out + (code - r.low);
			if (code <= static_cast<uint32_t>(std::numeric_limits<int>::max()) && cp > 0 && cp <= 0x10FFFFu) {
				out[static_cast<int>(code)] = {static_cast<int>(cp)};
			}
			if (code == std::numeric_limits<uint32_t>::max()) {
				break;
			}
		}
	}

	for (int i = 0; i < cmap.xlen; ++i) {
		const auto& r = cmap.xranges[i];
		if (r.high < r.low) {
			continue;
		}
		for (uint32_t code = r.low; code <= r.high; ++code) {
			const uint32_t cp = r.out + (code - r.low);
			if (code <= static_cast<uint32_t>(std::numeric_limits<int>::max()) && cp > 0 && cp <= 0x10FFFFu) {
				out[static_cast<int>(code)] = {static_cast<int>(cp)};
			}
			if (code == std::numeric_limits<uint32_t>::max()) {
				break;
			}
		}
	}

	for (int i = 0; i < cmap.mlen; ++i) {
		const auto& mr = cmap.mranges[i];
		if (mr.low > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
			continue;
		}
		if (mr.out >= static_cast<uint32_t>(cmap.tlen)) {
			continue;
		}

		int* ptr = cmap.table + mr.out;
		const int len = *ptr++;
		if (len <= 0) {
			continue;
		}
		if (mr.out + 1u + static_cast<uint32_t>(len) > static_cast<uint32_t>(cmap.tlen)) {
			continue;
		}

		std::vector<int> seq;
		seq.reserve(static_cast<size_t>(len));
		for (int k = 0; k < len; ++k) {
			const int cp = ptr[k];
			if (cp > 0 && cp <= 0x10FFFF) {
				seq.push_back(cp);
			}
		}

		if (!seq.empty()) {
			out[static_cast<int>(mr.low)] = std::move(seq);
		}
	}

	return out;
}

} // namespace

const std::unordered_map<int, std::vector<int>>& load_system_unicode_cmap_by_name(const std::string& cmap_name) {
	static const std::unordered_map<int, std::vector<int>> empty;
	static std::unordered_map<std::string, std::unordered_map<int, std::vector<int>>> cache;

	auto it = cache.find(cmap_name);
	if (it != cache.end()) {
		return it->second;
	}

	const pdf_cmap* cmap = find_unicode_cmap_by_name(cmap_name);
	if (!cmap) {
		return empty;
	}

	auto inserted = cache.emplace(cmap_name, build_unicode_map_from_cmap(*cmap));
	return inserted.first->second;
}

const std::unordered_map<int, std::vector<int>>& load_collection_unicode_cmap(const std::string& collection) {
	if (collection == "Adobe-CNS1") {
		return load_system_unicode_cmap_by_name("Adobe-CNS1-UCS2");
	}
	if (collection == "Adobe-GB1") {
		return load_system_unicode_cmap_by_name("Adobe-GB1-UCS2");
	}
	if (collection == "Adobe-Japan1" || collection == "Adobe-Japan2") {
		return load_system_unicode_cmap_by_name("Adobe-Japan1-UCS2");
	}
	if (collection == "Adobe-Korea1") {
		return load_system_unicode_cmap_by_name("Adobe-Korea1-UCS2");
	}

	static const std::unordered_map<int, std::vector<int>> empty;
	return empty;
}

} // namespace WinExtract
