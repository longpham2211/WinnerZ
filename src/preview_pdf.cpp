#include "preview_pdf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>

#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
#include "fpdfview.h"
#endif

namespace winnerz {

namespace {

static bool SetError(std::string* error_message, const std::string& message) {
	if (error_message != nullptr) {
		*error_message = message;
	}
	return false;
}

}  // namespace

#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
namespace {

static std::once_flag g_pdfium_init_once;
static bool g_pdfium_initialized = false;

static void InitPdfiumOnce() {
	FPDF_InitLibrary();
	std::atexit([]() { FPDF_DestroyLibrary(); });
	g_pdfium_initialized = true;
}

static std::array<int, 4> ComputeCropRect(float page_w,
										  float page_h,
										  float scale,
										  int full_w,
										  int full_h,
										  const std::array<float, 4>* clip) {
	if (clip == nullptr) {
		return {0, 0, full_w, full_h};
	}

	float x0 = std::clamp((*clip)[0], 0.0f, page_w);
	float y0 = std::clamp((*clip)[1], 0.0f, page_h);
	float x1 = std::clamp((*clip)[2], 0.0f, page_w);
	float y1 = std::clamp((*clip)[3], 0.0f, page_h);

	float rx0 = (std::min)(x0, x1);
	float ry0 = (std::min)(y0, y1);
	float rx1 = (std::max)(x0, x1);
	float ry1 = (std::max)(y0, y1);

	int left = (std::max)(0, static_cast<int>(std::floor(rx0 * scale)));
	int top = (std::max)(0, static_cast<int>(std::floor(ry0 * scale)));
	int right = (std::min)(full_w, static_cast<int>(std::ceil(rx1 * scale)));
	int bottom = (std::min)(full_h, static_cast<int>(std::ceil(ry1 * scale)));

	if (right <= left) {
		right = (std::min)(full_w, left + 1);
	}
	if (bottom <= top) {
		bottom = (std::min)(full_h, top + 1);
	}

	return {left, top, right, bottom};
}

static void ConvertBGRAtoRGBA(const uint8_t* src, uint8_t* dst, int pixel_count, bool has_alpha) {
	for (int x = 0; x < pixel_count; ++x) {
		const uint8_t b = src[x * 4 + 0];
		const uint8_t g = src[x * 4 + 1];
		const uint8_t r = src[x * 4 + 2];
		const uint8_t a = has_alpha ? src[x * 4 + 3] : static_cast<uint8_t>(255);

		dst[x * 4 + 0] = r;
		dst[x * 4 + 1] = g;
		dst[x * 4 + 2] = b;
		dst[x * 4 + 3] = a;
	}
}

}  // namespace
#endif

bool IsPdfiumPreviewEnabled() {
#if defined(WINNERZ_USE_PDFIUM_PREVIEW) && WINNERZ_USE_PDFIUM_PREVIEW
	return true;
#else
	return false;
#endif
}

bool RenderPdfPagePreview(const std::string& pdf_path,
                          const std::vector<uint8_t>& mem_data,
                          int page_index,
                          float scale,
                          const std::array<float, 4>* clip,
                          PreviewImage& out_image,
                          std::string* error_message) {
	out_image = {};

#if !defined(WINNERZ_USE_PDFIUM_PREVIEW) || !WINNERZ_USE_PDFIUM_PREVIEW
	return SetError(error_message, "PDFium preview disabled at build time");
#else
	if (scale <= 0.0f) {
		return SetError(error_message, "scale must be > 0");
	}

	std::call_once(g_pdfium_init_once, InitPdfiumOnce);
	if (!g_pdfium_initialized) {
		return SetError(error_message, "FPDF_InitLibrary failed");
	}

	FPDF_DOCUMENT document = nullptr;
	if (!mem_data.empty()) {
		document = FPDF_LoadMemDocument(mem_data.data(), static_cast<int>(mem_data.size()), nullptr);
	} else {
		document = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
	}
	
	if (!document) {
		const unsigned long code = static_cast<unsigned long>(FPDF_GetLastError());
		std::ostringstream oss;
		oss << "FPDF_LoadDocument failed, error=" << code;
		return SetError(error_message, oss.str());
	}

	const int page_count = FPDF_GetPageCount(document);
	if (page_index < 0 || page_index >= page_count) {
		FPDF_CloseDocument(document);
		return SetError(error_message, "page_index out of range");
	}

	FPDF_PAGE page = FPDF_LoadPage(document, page_index);
	if (!page) {
		const unsigned long code = static_cast<unsigned long>(FPDF_GetLastError());
		FPDF_CloseDocument(document);
		std::ostringstream oss;
		oss << "FPDF_LoadPage failed, error=" << code;
		return SetError(error_message, oss.str());
	}

	const float page_w = FPDF_GetPageWidthF(page);
	const float page_h = FPDF_GetPageHeightF(page);
	const int full_w = (std::max)(1, static_cast<int>(std::lround(page_w * scale)));
	const int full_h = (std::max)(1, static_cast<int>(std::lround(page_h * scale)));

	FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(full_w, full_h, FPDFBitmap_BGRA, nullptr, 0);
	if (!bitmap) {
		FPDF_ClosePage(page);
		FPDF_CloseDocument(document);
		return SetError(error_message, "FPDFBitmap_CreateEx failed");
	}

	FPDFBitmap_FillRect(bitmap, 0, 0, full_w, full_h, 0xFFFFFFFF);

	const int render_flags = FPDF_ANNOT | FPDF_NO_CATCH;
	FPDF_RenderPageBitmap(bitmap, page, 0, 0, full_w, full_h, 0, render_flags);

	const std::array<int, 4> crop = ComputeCropRect(page_w, page_h, scale, full_w, full_h, clip);
	const int crop_w = crop[2] - crop[0];
	const int crop_h = crop[3] - crop[1];

	const int format = FPDFBitmap_GetFormat(bitmap);
	const bool has_alpha = (format == FPDFBitmap_BGRA || format == FPDFBitmap_BGRA_Premul);
	const int src_stride = FPDFBitmap_GetStride(bitmap);
	const auto* src_buf = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
	if (!src_buf || src_stride <= 0) {
		FPDFBitmap_Destroy(bitmap);
		FPDF_ClosePage(page);
		FPDF_CloseDocument(document);
		return SetError(error_message, "FPDFBitmap_GetBuffer failed");
	}

	out_image.width = crop_w;
	out_image.height = crop_h;
	out_image.channels = 4;
	out_image.stride = crop_w * 4;
	out_image.rgba.resize(static_cast<size_t>(crop_h) * static_cast<size_t>(out_image.stride));

	for (int y = 0; y < crop_h; ++y) {
		const uint8_t* src_row = src_buf + static_cast<size_t>(crop[1] + y) * static_cast<size_t>(src_stride) +
								 static_cast<size_t>(crop[0]) * 4u;
		uint8_t* dst_row = out_image.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(out_image.stride);
		ConvertBGRAtoRGBA(src_row, dst_row, crop_w, has_alpha);
	}

	FPDFBitmap_Destroy(bitmap);
	FPDF_ClosePage(page);
	FPDF_CloseDocument(document);
	return true;
#endif
}

}  // namespace winnerz
