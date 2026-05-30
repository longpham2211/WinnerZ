#include <unordered_map>
#ifndef WINEXTRACT_PDF_ENGINE_HPP
#define WINEXTRACT_PDF_ENGINE_HPP

#pragma once
#include <vector>
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <memory>
#include <array>
#include <cstdint>
#include <functional>
#include "extractor_logic.hpp"
#include "decoder_inflate.hpp"
#include "mapped_file.hpp"
#include <mutex>

namespace WinExtract {

using WinUnicodeSequence = std::vector<int>;
using WinFontUnicodeMap = std::unordered_map<std::string, std::shared_ptr<const std::unordered_map<int, WinUnicodeSequence>>>;
using WinFontWidthMap  = std::unordered_map<std::string, std::shared_ptr<const std::unordered_map<int, float>>>;
using WinFontCodeBytesMap = std::unordered_map<std::string, int>;

struct WinCodeSpaceRange {
    int nbytes = 1;
    uint32_t low = 0;
    uint32_t high = 0;
};

using WinFontCodeSpaceMap = std::unordered_map<std::string, std::shared_ptr<const std::vector<WinCodeSpaceRange>>>;

using WinFontMatrixMap = std::unordered_map<std::string, std::array<float, 6>>;

struct WinFontVerticalMetrics {
    float ascender = 0.8f;
    float descender = -0.2f;
    std::string base_font;
    int flags = 0;
    float font_weight = 400.0f;
    bool is_bold = false;
    bool is_italic = false;
    bool is_serif = false;
    bool is_mono = false;
};

using WinFontVerticalMetricsMap = std::unordered_map<std::string, WinFontVerticalMetrics>;

enum class WinColorSpaceKind {
    Unknown = 0,
    DeviceGray,
    DeviceRGB,
    DeviceCMYK,
    Lab,
    ICCBased,
    Separation,
    DeviceN,
    Indexed,
    Pattern,
};

enum class WinIccCurveType {
    Identity = 0,
    Gamma,
    Table,
};

struct WinIccCurve {
    WinIccCurveType type = WinIccCurveType::Identity;
    float gamma = 1.0f;
    std::vector<float> table;
};

struct WinColorSpaceDef {
    WinColorSpaceKind kind = WinColorSpaceKind::Unknown;
    WinColorSpaceKind alt_kind = WinColorSpaceKind::Unknown;
    int alt_component_count = 0;
    int component_count = 0;
    bool has_tint_transform = false;
    int tint_function_type = -1;
    int tint_input_count = 0;
    int tint_output_count = 0;
    float tint_n = 1.0f;
    std::vector<float> tint_domain;
    std::vector<float> tint_range;
    std::vector<float> tint_c0;
    std::vector<float> tint_c1;
    std::vector<int> tint_size;
    int tint_bits_per_sample = 0;
    std::vector<float> tint_encode;
    std::vector<float> tint_decode;
    std::vector<uint8_t> tint_samples;
    float lab_white_x = 0.9642f;
    float lab_white_y = 1.0000f;
    float lab_white_z = 0.8249f;

    // Optional ICCBased RGB profile data (matrix/TRC) for accurate conversion.
    bool has_icc_rgb_profile = false;
    std::array<float, 9> icc_rgb_to_xyz = {1.0f, 0.0f, 0.0f,
                                           0.0f, 1.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f};
    WinIccCurve icc_trc_r;
    WinIccCurve icc_trc_g;
    WinIccCurve icc_trc_b;
};

using WinColorSpaceMap = std::unordered_map<std::string, WinColorSpaceDef>;

struct WinFormXObject {
    std::vector<uint8_t> stream;
    WinFontUnicodeMap font_unicode_map;
    WinFontWidthMap font_width_map;
    WinFontCodeBytesMap font_code_bytes_map;
    WinFontCodeSpaceMap font_codespace_map;
    WinFontMatrixMap font_matrix_map;
    WinFontVerticalMetricsMap font_vertical_metrics_map;
    WinColorSpaceMap color_space_map;
    std::array<float, 6> matrix = {1, 0, 0, 1, 0, 0};
    bool has_bbox = false;
    std::array<float, 4> bbox = {0, 0, 0, 0};
    std::shared_ptr<std::unordered_map<std::string, WinFormXObject>> children;
};

using WinFormXObjectMap = std::unordered_map<std::string, WinFormXObject>;

// CẤU TRÚC "FITZ PIPELINE" CLONE
// Document -> Page -> Interpreter -> Device (MuLogic)

struct WinPdfObject {
    int id;
    int gen;
    std::string dict;
    std::string body;
    std::vector<uint8_t> stream;
    bool is_stream = false;
};

// 1. Phân tích cú pháp File (Xref/Trailer)
class WinPdfDocument {
public:
    static std::shared_ptr<WinPdfDocument> open(const std::string& path);
    static std::shared_ptr<WinPdfDocument> open_from_memory(const std::vector<uint8_t>& data);
    int count_pages() const;
    std::vector<uint8_t> get_page_content(int page_idx);
    bool is_encrypted() const { return encrypted_; }
    WinFontUnicodeMap get_page_font_unicode_map(int page_idx);
    WinFontWidthMap   get_page_font_width_map(int page_idx);
    WinFontCodeBytesMap get_page_font_code_bytes_map(int page_idx);
    WinFontCodeSpaceMap get_page_font_codespace_map(int page_idx);
    WinFontMatrixMap get_page_font_matrix_map(int page_idx);
    WinFontVerticalMetricsMap get_page_font_vertical_metrics_map(int page_idx);
    WinColorSpaceMap get_page_color_space_map(int page_idx);
    WinFormXObjectMap get_page_form_xobject_map(int page_idx);
    Rect              get_page_mediabox(int page_idx);
    // Save a new PDF by replacing a page's /Contents with a new decoded stream.
    // The output is written as an incremental update to preserve original objects.
    bool save_page_content_incremental(int page_idx,
                                       const std::vector<uint8_t>& decoded_stream,
                                       const std::string& output_path);
                                       
    std::vector<uint8_t> save_multiple_pages_content_incremental_to_bytes(const std::map<int, std::vector<uint8_t>>& pages_streams);
    bool save_multiple_pages_content_incremental(
        const std::map<int, std::vector<uint8_t>>& pages_streams,
        const std::string& output_path);
    
    // Giải phóng bộ nhớ cache (page-based flush) để tránh OOM
    void clear_page_cache();
    
    // Tìm object bằng ID
    WinPdfObject read_obj(int id);
    
private:
    struct ObjStreamEntry {
        int container_id = 0;
        int item_index = -1;
    };

    struct ObjStreamInfo {
        int first = 0;
        std::vector<std::pair<int, int>> items; // (object id, relative offset)
        std::vector<uint8_t> decoded;
    };

    // ── Zero-copy file storage ─────────────────────────────────────────────────
    // Đường đi bình thường (mmap): file_view_ trỏ thẳng vào bộ nhớ OS-mapped.
    //   → RAM vật lý chỉ load đúng page đang truy cập. File 1 GB đợc xử lý
    //     mà không bao giờ có 1 GB nào trong heap.
    // Đường dự phòng (mmap thất bại): data_ lưu file, file_view_ trỏ vào data_.
    MappedFile mapped_file_;
    std::string_view file_view_;   // luôn hợp lệ sau open()
    std::vector<uint8_t> data_;    // chỉ khác rỗng khi mmap thất bại (fallback)

    std::map<int, size_t> xref; // ID -> Offset
    int root_id = 0;
    std::vector<int> page_ids;
    std::vector<int> fallback_content_ids;

    bool objstm_index_ready = false;
    std::map<int, ObjStreamInfo> objstm_infos;      // ObjStm object id -> decoded info
    std::map<int, ObjStreamEntry> objstm_lookup;    // object id -> ObjStm location
    std::map<int, WinPdfObject> object_cache;       // resolved object cache
    WinFormXObjectMap form_xobjects_;

    bool encrypted_ = false;

    // Cached font maps per font object ID
    std::unordered_map<int, std::shared_ptr<const std::unordered_map<int, WinUnicodeSequence>>> cached_unicode_maps;
    std::unordered_map<int, std::shared_ptr<const std::unordered_map<int, float>>> cached_width_maps;
    std::unordered_map<int, int> cached_code_bytes_maps;
    std::unordered_map<int, std::shared_ptr<const std::vector<WinCodeSpaceRange>>> cached_codespace_maps;
    std::unordered_map<int, std::array<float, 6>> cached_matrix_maps;
    std::unordered_map<int, WinFontVerticalMetrics> cached_vertical_metrics_maps;
    std::unordered_map<int, std::shared_ptr<std::vector<uint8_t>>> cached_decoded_streams;

    mutable std::recursive_mutex cache_mutex;

    void parse_xref();
    void build_objstm_index();
    void load_objstm(int container_id);
    WinPdfObject read_obj_from_offset(int id, size_t offset);
    WinPdfObject read_obj_from_objstm(int id);
    void find_pages();
    void collect_page_nodes(int node_id);
    void build_fallback_pages_from_streams();
    static bool looks_like_text_content_stream(const std::vector<uint8_t>& bytes);
    static int parse_ref_id_after_key(const std::string& dict, const std::string& key);
    std::vector<int> parse_ref_array_after_key(const std::string& dict, const std::string& key);
    static bool is_page_object(const std::string& dict);
    static bool has_flate_filter(const std::string& dict);
};

// 2. Thông dịch Content Stream (Clone Fitz Interpreter)
class WinPdfInterpreter {
public:
    // Nhận Content Stream và Một Device (MuLogicExtractor)
    static void run(const std::vector<uint8_t>& stream, MuLogicExtractor& dev,
                   const WinFontUnicodeMap& font_unicode_map = {},
                   const WinFontWidthMap&   font_width_map   = {},
                   const WinFontCodeBytesMap& font_code_bytes_map = {},
                   const WinFontCodeSpaceMap& font_codespace_map = {},
                   const WinFontMatrixMap& font_matrix_map = {},
                   const WinFontVerticalMetricsMap& font_vertical_metrics_map = {},
                   const WinColorSpaceMap& color_space_map = {},
                   const WinFormXObjectMap& form_xobject_map = {},
                   const float* initial_ctm = nullptr,
                   int recursion_depth = 0,
                   const Rect* page_mediabox = nullptr,
                   const Rect* inherited_clip_box = nullptr,
                   const uint32_t* inherited_fill_color = nullptr,
                   const uint32_t* inherited_stroke_color = nullptr,
                   const WinColorSpaceDef* inherited_fill_space = nullptr,
                   const WinColorSpaceDef* inherited_stroke_space = nullptr,
                   const int* inherited_text_render_mode = nullptr);

private:
    // State machine cho Matrix (1:1 Fitz)
    struct State {
        float ctm[6];
        float trm[6];
        float font_size;
        // ...
    };
};

} // namespace WinExtract

#endif // WINEXTRACT_PDF_ENGINE_HPP
