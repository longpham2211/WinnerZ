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
#include <shared_mutex>
#include "pdf_font_metrics.hpp"

namespace WinExtract {

using WinUnicodeSequence = std::vector<int>;
using WinFontUnicodeMap = std::unordered_map<std::string, std::shared_ptr<const std::unordered_map<int, WinUnicodeSequence>>>;
using WinFontWidthMap  = std::unordered_map<std::string, std::shared_ptr<const std::unordered_map<int, float>>>;
using WinFontW2Map = std::unordered_map<std::string, std::shared_ptr<const WinW2MetricsMap>>;
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
    int wmode = 0;
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

struct WinImageXObject {
    int obj_id = -1;
    int width = 0;
    int height = 0;
    int bits_per_component = 8;
    std::string color_space;
    std::string filter;
    std::vector<float> decode;
    std::shared_ptr<const std::vector<uint8_t>> stream_ptr;
    std::shared_ptr<const std::vector<uint8_t>> smask_ptr;
    std::vector<uint8_t> indexed_palette;
};

using WinImageXObjectMap = std::unordered_map<std::string, WinImageXObject>;

struct WinFormXObject {
    std::shared_ptr<const std::vector<uint8_t>> stream_ptr;
    WinFontUnicodeMap font_unicode_map;
    WinFontWidthMap font_width_map;
    WinFontCodeBytesMap font_code_bytes_map;
    WinFontCodeSpaceMap font_codespace_map;
    WinFontMatrixMap font_matrix_map;
    WinFontVerticalMetricsMap font_vertical_metrics_map;
    WinFontW2Map font_w2_map;
    WinColorSpaceMap color_space_map;
    std::array<float, 6> matrix = {1, 0, 0, 1, 0, 0};
    bool has_bbox = false;
    std::array<float, 4> bbox = {0, 0, 0, 0};
    int obj_id = -1;
    std::shared_ptr<std::unordered_map<std::string, WinFormXObject>> children;
};

using WinFormXObjectMap = std::unordered_map<std::string, WinFormXObject>;


struct WinPdfObject {
    int id;
    int gen;
    std::string dict;
    std::string body;
    std::vector<uint8_t> stream;
    bool is_stream = false;
};

struct WinPageGeometry {
    Rect mediabox = {0, 0, 595, 842};
    Rect cropbox = {0, 0, 595, 842};
    int rotate = 0;
};

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
    WinFontW2Map get_page_font_w2_map(int page_idx);
    WinColorSpaceMap get_page_color_space_map(int page_idx);
    std::shared_ptr<const WinFormXObjectMap> get_page_form_xobject_map(int page_idx);
    std::shared_ptr<const WinImageXObjectMap> get_page_image_xobject_map(int page_idx);
    WinPageGeometry   get_page_geometry(int page_idx);
    // Save a new PDF by replacing a page's /Contents with a new decoded stream.
    // The output is written as an incremental update to preserve original objects.
    bool save_page_content_incremental(int page_idx,
                                       const std::vector<uint8_t>& decoded_stream,
                                       const std::string& output_path);
                                       
    std::vector<uint8_t> save_multiple_pages_content_incremental_to_bytes(
        const std::map<int, std::vector<uint8_t>>& pages_streams,
        const std::map<int, std::vector<uint8_t>>& updated_xobjects = {});
    bool save_multiple_pages_content_incremental(
        const std::map<int, std::vector<uint8_t>>& pages_streams,
        const std::string& output_path,
        const std::map<int, std::vector<uint8_t>>& updated_xobjects = {});
        
    int get_max_obj_id();
    std::vector<uint8_t> save_incremental_update(
        const std::map<int, std::string>& updated_objects,
        const std::map<int, std::string>& new_objects);
        
    int get_page_id(int page_idx) const { return page_ids[page_idx]; }
    
    void clear_page_cache();

    std::map<std::string, int> get_page_font_name_to_id(int page_idx);
    bool patch_font_unicode_map_lazily(int font_obj_id);
    std::shared_ptr<const std::unordered_map<int, WinUnicodeSequence>> get_font_unicode_map_by_id(int font_obj_id);
    
    std::vector<uint8_t> get_embedded_font_stream(int font_obj_id);
    
    struct CidToGidMapData {
        bool has_map = false;
        bool identity = false;
        std::vector<uint16_t> values;
    };
    bool resolve_type0_descendant_font(const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj);
    bool resolve_font_descriptor_dict(const WinPdfObject& font_obj, std::string& descriptor_dict);
    CidToGidMapData load_cid_to_gid_map(const WinPdfObject& font_obj);
    void fill_missing_unicode_from_freetype(const WinPdfObject& font_obj, std::unordered_map<int, std::vector<int>>& unicode_map, const std::map<int, std::string>& diff_names);

    
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

    MappedFile mapped_file_;
    std::string_view file_view_;  
    std::vector<uint8_t> data_;    

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
    std::unordered_map<int, std::shared_ptr<const WinW2MetricsMap>> cached_w2_maps;
    std::unordered_map<int, std::shared_ptr<const std::vector<uint8_t>>> cached_decoded_streams;
    std::unordered_map<int, std::shared_ptr<const WinFormXObjectMap>> cached_page_xobject_maps;
    std::unordered_map<std::string, std::shared_ptr<const WinFormXObjectMap>> cached_resource_xobject_maps;
    
    std::unordered_map<int, std::shared_ptr<const WinImageXObjectMap>> cached_page_image_xobject_maps;
    std::unordered_map<std::string, std::shared_ptr<const WinImageXObjectMap>> cached_resource_image_xobject_maps;

    mutable std::recursive_mutex cache_mutex;
    std::recursive_mutex objstm_mutex;

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
    static bool is_type0_font_dict(const std::string& dict);
};

class WinPdfInterpreter {
public:
    static void run(const std::vector<uint8_t>& stream, WinTextExtractor& dev,
                   const WinFontUnicodeMap& font_unicode_map = {},
                   const WinFontWidthMap&   font_width_map   = {},
                   const WinFontCodeBytesMap& font_code_bytes_map = {},
                   const WinFontCodeSpaceMap& font_codespace_map = {},
                   const WinFontMatrixMap& font_matrix_map = {},
                   const WinFontVerticalMetricsMap& font_vertical_metrics_map = {},
                   const WinFontW2Map& font_w2_map = {},
                   const WinColorSpaceMap& color_space_map = {},
                   std::shared_ptr<const WinFormXObjectMap> form_xobject_map = nullptr,
                   std::shared_ptr<const WinImageXObjectMap> image_xobject_map = nullptr,
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
    struct State {
        float ctm[6];
        float trm[6];
        float font_size;
    };
};

} // namespace WinExtract

#endif // WINEXTRACT_PDF_ENGINE_HPP