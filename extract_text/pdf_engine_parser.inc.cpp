std::shared_ptr<WinPdfDocument> WinPdfDocument::open_from_memory(const std::vector<uint8_t>& data) {
    auto doc = std::make_shared<WinPdfDocument>();
    doc->data_ = data;
    doc->file_view_ = std::string_view(
        reinterpret_cast<const char*>(doc->data_.data()), doc->data_.size());

    doc->parse_xref();

    const std::string_view sv = doc->file_view_;
    if (doc->root_id <= 0) {
        const size_t last_trailer = sv.rfind("trailer");
        if (last_trailer != std::string_view::npos) {
            doc->root_id = parse_ref_id_after_key(
                std::string(sv.substr(last_trailer)), "/Root");
        }
    }
    if (doc->root_id <= 0) {
        doc->root_id = parse_ref_id_after_key(std::string(sv), "/Root");
    }
    if (doc->root_id > 0) {
        doc->find_pages();
    }
    if (doc->page_ids.empty()) {
        doc->build_fallback_pages_from_streams();
    }
    return doc;
}

std::shared_ptr<WinPdfDocument> WinPdfDocument::open(const std::string& path) {
    auto doc = std::make_shared<WinPdfDocument>();

    doc->mapped_file_ = MappedFile(path);

    if (!doc->mapped_file_.ok()) {
#if defined(_WIN32) || defined(_WIN64)
        int size_w = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(size_w, 0);
        ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], size_w);
        wpath.resize(size_w > 0 ? size_w - 1 : 0);
        std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
#else
        std::ifstream file(path, std::ios::binary | std::ios::ate);
#endif
        if (!file.is_open()) {
            return nullptr;
        }
        const size_t size = static_cast<size_t>(file.tellg());
        doc->data_.resize(size);
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(doc->data_.data()),
                  static_cast<std::streamsize>(size));
        doc->file_view_ = std::string_view(
            reinterpret_cast<const char*>(doc->data_.data()), size);
    } else {
        doc->file_view_ = doc->mapped_file_.view();
    }

    doc->parse_xref();

    const std::string_view sv = doc->file_view_;
    if (doc->root_id <= 0) {
        const size_t last_trailer = sv.rfind("trailer");
        if (last_trailer != std::string_view::npos) {
            doc->root_id = parse_ref_id_after_key(
                std::string(sv.substr(last_trailer)), "/Root");
        }
    }
    if (doc->root_id <= 0) {
        doc->root_id = parse_ref_id_after_key(std::string(sv), "/Root");
    }

    if (doc->root_id > 0) {
        doc->find_pages();
    }

    if (doc->page_ids.empty()) {
        doc->build_fallback_pages_from_streams();
    }
    return doc;
}

void WinPdfDocument::find_pages() {
    page_ids.clear();
    if (root_id <= 0) {
        return;
    }

    WinPdfObject root_obj = read_obj(root_id);
    int pages_id = parse_ref_id_after_key(root_obj.dict, "/Pages");
    if (pages_id > 0) {
        collect_page_nodes(pages_id);
    }
}

void WinPdfDocument::build_fallback_pages_from_streams() {
    fallback_content_ids.clear();

    for (const auto& kv : xref) {
        int obj_id = kv.first;
        WinPdfObject obj = read_obj(obj_id);
        if (!obj.is_stream || obj.stream.empty()) {
            continue;
        }

        std::vector<uint8_t> bytes = decode_stream_data(obj.stream, obj.dict, this);
        if (bytes.empty()) {
            bytes = obj.stream;
        }

        if (looks_like_text_content_stream(bytes)) {
            fallback_content_ids.push_back(obj_id);
        }
    }
}

void WinPdfDocument::collect_page_nodes(int node_id) {
    if (node_id <= 0) {
        return;
    }

    WinPdfObject node = read_obj(node_id);
    if (node.dict.empty()) {
        return;
    }

    if (is_page_object(node.dict)) {
        page_ids.push_back(node_id);
        return;
    }

    std::vector<int> kids = parse_ref_array_after_key(node.dict, "/Kids");
    for (int kid : kids) {
        collect_page_nodes(kid);
    }
}

int WinPdfDocument::count_pages() const {
    if (!page_ids.empty()) {
        return static_cast<int>(page_ids.size());
    }
    return static_cast<int>(fallback_content_ids.size());
}

std::vector<uint8_t> WinPdfDocument::get_page_content(int page_idx) {
    if (page_idx < 0) {
        return {};
    }

    if (!page_ids.empty()) {
        if (page_idx >= static_cast<int>(page_ids.size())) {
            return {};
        }

        WinPdfObject page_obj = read_obj(page_ids[page_idx]);
        std::vector<int> content_refs = parse_ref_array_after_key(page_obj.dict, "/Contents");
        if (content_refs.empty()) {
            int single_ref = parse_ref_id_after_key(page_obj.dict, "/Contents");
            if (single_ref > 0) {
                content_refs.push_back(single_ref);
            }
        }

        auto parse_array_refs = [&](const std::string& text) {
            std::vector<int> out;
            size_t arr_start = text.find('[');
            size_t arr_end = text.find(']', arr_start == std::string::npos ? 0 : arr_start);
            if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start + 1) {
                return out;
            }

            const std::string refs_text = text.substr(arr_start + 1, arr_end - arr_start - 1);
            const char* p = refs_text.c_str();
            while (*p) {
                int id = 0;
                int gen = 0;
                char r = 0;
                int consumed = 0;
                if (!wz_parse_xref_line_3(p, id, gen, r, consumed) || consumed <= 0) {
                    break;
                }
                if (r == 'R' && id > 0) {
                    out.push_back(id);
                }
                p += consumed;
            }

            return out;
        };

        std::vector<int> resolved_content_refs;
        std::set<int> content_guard;
        std::function<void(int)> resolve_content_ref = [&](int obj_id) {
            if (obj_id <= 0 || content_guard.find(obj_id) != content_guard.end()) {
                return;
            }
            content_guard.insert(obj_id);

            WinPdfObject content = read_obj(obj_id);
            if (content.is_stream) {
                resolved_content_refs.push_back(obj_id);
                return;
            }

            std::vector<int> nested_refs = parse_array_refs(content.body);
            if (nested_refs.empty()) {
                nested_refs = parse_array_refs(content.dict);
            }
            for (int nested_id : nested_refs) {
                resolve_content_ref(nested_id);
            }
        };

        for (int obj_id : content_refs) {
            resolve_content_ref(obj_id);
        }

        std::vector<uint8_t> merged;
        for (int obj_id : resolved_content_refs) {
            WinPdfObject content = read_obj(obj_id);
            if (!content.is_stream || content.stream.empty()) {
                continue;
            }

            std::vector<uint8_t> bytes = decode_stream_data(content.stream, content.dict, this);
            if (bytes.empty()) {
                bytes = content.stream;
            }

            merged.insert(merged.end(), bytes.begin(), bytes.end());
            merged.push_back('\n');
        }

        return merged;
    }

    if (page_idx >= static_cast<int>(fallback_content_ids.size())) {
        return {};
    }

    WinPdfObject content = read_obj(fallback_content_ids[page_idx]);
    if (!content.is_stream || content.stream.empty()) {
        return {};
    }

    std::vector<uint8_t> decoded = decode_stream_data(content.stream, content.dict, this);
    if (!decoded.empty()) {
        return decoded;
    }
    return content.stream;
}


std::vector<uint8_t> WinPdfDocument::save_multiple_pages_content_incremental_to_bytes(
        const std::map<int, std::vector<uint8_t>>& pages_streams,
        const std::map<int, std::vector<uint8_t>>& updated_xobjects) {
    if (pages_streams.empty() || page_ids.empty()) {
        return {};
    }
    if (file_view_.empty()) {
        return {};
    }

    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
            char* end_ptr = nullptr;
            long v = wz_strtol(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) return v;
            if (pos == 0) break;
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    auto replace_contents_entry = [&](const std::string& dict, const std::string& new_ref) {
        auto append_at_end = [&](const std::string& src) {
            std::string out = src;
            size_t dict_end = out.rfind(">>");
            if (dict_end == std::string::npos) return out;
            out.insert(dict_end, " /Contents " + new_ref + " ");
            return out;
        };

        size_t key_pos = std::string::npos;
        size_t search_pos = 0;
        while (search_pos < dict.size()) {
            size_t found = dict.find("/Contents", search_pos);
            if (found == std::string::npos) break;
            const size_t after = found + 9;
            const char c = (after < dict.size()) ? dict[after] : ' ';
            if (after >= dict.size() || std::isspace(static_cast<unsigned char>(c)) ||
                c == '[' || c == '<' || c == '/' || std::isdigit(static_cast<unsigned char>(c))) {
                key_pos = found;
                break;
            }
            search_pos = after;
        }

        if (key_pos == std::string::npos) return append_at_end(dict);

        size_t value_start = key_pos + 9;
        while (value_start < dict.size() && std::isspace(static_cast<unsigned char>(dict[value_start]))) ++value_start;
        size_t value_end = value_start;

        if (value_start < dict.size() && dict[value_start] == '[') {
            int depth = 0;
            for (size_t i = value_start; i < dict.size(); ++i) {
                if (dict[i] == '[') ++depth;
                else if (dict[i] == ']') {
                    --depth;
                    if (depth == 0) { value_end = i + 1; break; }
                }
            }
        }

        if (value_end == value_start) {
            const char* p = dict.c_str() + value_start;
            char* e1 = nullptr;
            long v1 = wz_strtol(p, &e1, 10);
            if (e1 != p && v1 > 0) {
                char* e2 = nullptr;
                long v2 = wz_strtol(e1, &e2, 10);
                if (e2 != e1 && v2 >= 0) {
                    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) ++e2;
                    if (*e2 == 'R') {
                        ++e2;
                        value_end = static_cast<size_t>(e2 - dict.c_str());
                    }
                }
            }
        }

        if (value_end == value_start) {
            while (value_end < dict.size() && !std::isspace(static_cast<unsigned char>(dict[value_end])) &&
                   dict[value_end] != '/' && dict[value_end] != '>') {
                ++value_end;
            }
        }

        if (value_end <= value_start) return append_at_end(dict);

        std::string out = dict;
        out.replace(value_start, value_end - value_start, new_ref);
        return out;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) root_id = parse_ref_id_after_key(raw, "/Root");
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) return {};

    build_objstm_index();

    int max_obj_id = 0;
    for (const auto& kv : xref) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (const auto& kv : objstm_lookup) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (int pid : page_ids) max_obj_id = (std::max)(max_obj_id, pid);
    max_obj_id = (std::max)(max_obj_id, root_id);

    std::string out;
    out.reserve(raw.size() + pages_streams.size() * 500000);
    out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') out.push_back('\n');

    struct XrefEntry { int id; size_t offset; };
    std::vector<XrefEntry> changed;

    int current_new_id = max_obj_id + 1;

    for (const auto& kv : pages_streams) {
        int page_idx = kv.first;
        const auto& decoded_stream = kv.second;

        if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size())) continue;
        WinPdfObject page_obj = read_obj(page_ids[page_idx]);
        if (page_obj.id <= 0 || page_obj.dict.empty()) continue;

        const int new_stream_id = current_new_id++;
        const std::string new_ref = std::to_string(new_stream_id) + " 0 R";
        const std::string updated_page_dict = replace_contents_entry(page_obj.dict, new_ref);

        const size_t stream_offset = out.size();
        out += std::to_string(new_stream_id) + " 0 obj\n<< /Length " + std::to_string(decoded_stream.size()) + " >>\nstream\n";
        if (!decoded_stream.empty()) out.append(reinterpret_cast<const char*>(decoded_stream.data()), decoded_stream.size());
        if (decoded_stream.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endstream\nendobj\n";

        const size_t page_offset = out.size();
        out += std::to_string(page_obj.id) + " " + std::to_string(page_obj.gen) + " obj\n";
        out += updated_page_dict;
        if (updated_page_dict.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endobj\n";

        changed.push_back({page_obj.id, page_offset});
        changed.push_back({new_stream_id, stream_offset});
    }

    for (const auto& kv : updated_xobjects) {
        int xobj_id = kv.first;
        const auto& raw_stream = kv.second;

        WinPdfObject xobj = read_obj(xobj_id);
        if (xobj.id <= 0 || xobj.dict.empty()) continue;

        // Strip old /Length and /Filter
        std::string new_dict = xobj.dict;
        auto remove_key = [&](const std::string& key) {
            size_t pos = new_dict.find(key);
            if (pos != std::string::npos) {
                size_t end_pos = pos + key.size();
                while (end_pos < new_dict.size() && std::isspace(static_cast<unsigned char>(new_dict[end_pos]))) ++end_pos;
                if (end_pos < new_dict.size() && new_dict[end_pos] == '[') {
                    int depth = 0;
                    for (; end_pos < new_dict.size(); ++end_pos) {
                        if (new_dict[end_pos] == '[') depth++;
                        else if (new_dict[end_pos] == ']') {
                            depth--;
                            if (depth == 0) { end_pos++; break; }
                        }
                    }
                } else {
                    if (end_pos < new_dict.size() && new_dict[end_pos] == '/') ++end_pos;
                    while (end_pos < new_dict.size() && !std::isspace(new_dict[end_pos]) && new_dict[end_pos] != '/' && new_dict[end_pos] != '>' && new_dict[end_pos] != '<' && new_dict[end_pos] != '[') ++end_pos;
                }
                new_dict.erase(pos, end_pos - pos);
            }
        };
        remove_key("/Length");
        remove_key("/Filter");

        auto compressed = raw_stream; // Uncompressed fallback
        
        size_t dict_end = new_dict.rfind(">>");
        if (dict_end != std::string::npos) {
            new_dict.insert(dict_end, " /Length " + std::to_string(compressed.size()) + " ");
        }

        const size_t xobj_offset = out.size();
        out += std::to_string(xobj.id) + " " + std::to_string(xobj.gen) + " obj\n";
        out += new_dict;
        if (new_dict.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "stream\n";
        if (!compressed.empty()) out.append(reinterpret_cast<const char*>(compressed.data()), compressed.size());
        if (compressed.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endstream\nendobj\n";

        changed.push_back({xobj.id, xobj_offset});
    }

    if (changed.empty()) return {};

    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) { return a.id < b.id; });

    const size_t xref_offset = out.size();
    out += "xref\n";
    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) ++j;
        out += std::to_string(changed[i].id) + " " + std::to_string(j - i) + "\n";
        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n", static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    const int size_value = current_new_id;
    out += "trailer\n<< /Size " + std::to_string(size_value) + " /Root " + std::to_string(root_id) +
           " 0 R /Prev " + std::to_string(prev_startxref) + " >>\nstartxref\n" +
           std::to_string(xref_offset) + "\n%%EOF\n";

    return std::vector<uint8_t>(out.begin(), out.end());
}

bool WinPdfDocument::save_multiple_pages_content_incremental(
        const std::map<int, std::vector<uint8_t>>& pages_streams,
        const std::string& output_path,
        const std::map<int, std::vector<uint8_t>>& updated_xobjects) {
    std::vector<uint8_t> new_data = save_multiple_pages_content_incremental_to_bytes(pages_streams, updated_xobjects);
    if (pages_streams.empty() || page_ids.empty()) {
        return false;
    }
    if (output_path.empty() || file_view_.empty()) {
        return false;
    }

    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
            char* end_ptr = nullptr;
            long v = wz_strtol(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) return v;
            if (pos == 0) break;
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    auto replace_contents_entry = [&](const std::string& dict, const std::string& new_ref) {
        auto append_at_end = [&](const std::string& src) {
            std::string out = src;
            size_t dict_end = out.rfind(">>");
            if (dict_end == std::string::npos) return out;
            out.insert(dict_end, " /Contents " + new_ref + " ");
            return out;
        };

        size_t key_pos = std::string::npos;
        size_t search_pos = 0;
        while (search_pos < dict.size()) {
            size_t found = dict.find("/Contents", search_pos);
            if (found == std::string::npos) break;
            const size_t after = found + 9;
            const char c = (after < dict.size()) ? dict[after] : ' ';
            if (after >= dict.size() || std::isspace(static_cast<unsigned char>(c)) ||
                c == '[' || c == '<' || c == '/' || std::isdigit(static_cast<unsigned char>(c))) {
                key_pos = found;
                break;
            }
            search_pos = after;
        }

        if (key_pos == std::string::npos) return append_at_end(dict);

        size_t value_start = key_pos + 9;
        while (value_start < dict.size() && std::isspace(static_cast<unsigned char>(dict[value_start]))) ++value_start;
        size_t value_end = value_start;

        if (value_start < dict.size() && dict[value_start] == '[') {
            int depth = 0;
            for (size_t i = value_start; i < dict.size(); ++i) {
                if (dict[i] == '[') ++depth;
                else if (dict[i] == ']') {
                    --depth;
                    if (depth == 0) { value_end = i + 1; break; }
                }
            }
        }

        if (value_end == value_start) {
            const char* p = dict.c_str() + value_start;
            char* e1 = nullptr;
            long v1 = wz_strtol(p, &e1, 10);
            if (e1 != p && v1 > 0) {
                char* e2 = nullptr;
                long v2 = wz_strtol(e1, &e2, 10);
                if (e2 != e1 && v2 >= 0) {
                    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) ++e2;
                    if (*e2 == 'R') {
                        ++e2;
                        value_end = static_cast<size_t>(e2 - dict.c_str());
                    }
                }
            }
        }

        if (value_end == value_start) {
            while (value_end < dict.size() && !std::isspace(static_cast<unsigned char>(dict[value_end])) &&
                   dict[value_end] != '/' && dict[value_end] != '>') {
                ++value_end;
            }
        }

        if (value_end <= value_start) return append_at_end(dict);

        std::string out = dict;
        out.replace(value_start, value_end - value_start, new_ref);
        return out;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) root_id = parse_ref_id_after_key(raw, "/Root");
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) return false;

    build_objstm_index();

    int max_obj_id = 0;
    for (const auto& kv : xref) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (const auto& kv : objstm_lookup) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (int pid : page_ids) max_obj_id = (std::max)(max_obj_id, pid);
    max_obj_id = (std::max)(max_obj_id, root_id);

    std::string out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') out.push_back('\n');

    struct XrefEntry { int id; size_t offset; };
    std::vector<XrefEntry> changed;

    int current_new_id = max_obj_id + 1;

    for (const auto& kv : pages_streams) {
        int page_idx = kv.first;
        const auto& decoded_stream = kv.second;

        if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size())) continue;
        WinPdfObject page_obj = read_obj(page_ids[page_idx]);
        if (page_obj.id <= 0 || page_obj.dict.empty()) continue;

        const int new_stream_id = current_new_id++;
        const std::string new_ref = std::to_string(new_stream_id) + " 0 R";
        const std::string updated_page_dict = replace_contents_entry(page_obj.dict, new_ref);

        const size_t stream_offset = out.size();
        out += std::to_string(new_stream_id) + " 0 obj\n<< /Length " + std::to_string(decoded_stream.size()) + " >>\nstream\n";
        if (!decoded_stream.empty()) out.append(reinterpret_cast<const char*>(decoded_stream.data()), decoded_stream.size());
        if (decoded_stream.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endstream\nendobj\n";

        const size_t page_offset = out.size();
        out += std::to_string(page_obj.id) + " " + std::to_string(page_obj.gen) + " obj\n";
        out += updated_page_dict;
        if (updated_page_dict.empty() || (out.back() != '\n' && out.back() != '\r')) out.push_back('\n');
        out += "endobj\n";

        changed.push_back({page_obj.id, page_offset});
        changed.push_back({new_stream_id, stream_offset});
    }

    if (changed.empty()) return false;

    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) { return a.id < b.id; });

    const size_t xref_offset = out.size();
    out += "xref\n";
    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) ++j;
        out += std::to_string(changed[i].id) + " " + std::to_string(j - i) + "\n";
        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n", static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    const int size_value = current_new_id;
    out += "trailer\n<< /Size " + std::to_string(size_value) + " /Root " + std::to_string(root_id) +
           " 0 R /Prev " + std::to_string(prev_startxref) + " >>\nstartxref\n" +
           std::to_string(xref_offset) + "\n%%EOF\n";

    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file.good();
}

int WinPdfDocument::get_max_obj_id() {
    if (root_id <= 0) {
        std::string raw(file_view_.data(), file_view_.size());
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) root_id = parse_ref_id_after_key(raw, "/Root");
    }
    build_objstm_index();
    int max_obj_id = 0;
    for (const auto& kv : xref) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (const auto& kv : objstm_lookup) max_obj_id = (std::max)(max_obj_id, kv.first);
    for (int pid : page_ids) max_obj_id = (std::max)(max_obj_id, pid);
    max_obj_id = (std::max)(max_obj_id, root_id);
    return max_obj_id;
}

std::vector<uint8_t> WinPdfDocument::save_incremental_update(
        const std::map<int, std::string>& updated_objects,
        const std::map<int, std::string>& new_objects) {
    
    if (file_view_.empty()) return {};
    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
            char* end_ptr = nullptr;
            long v = wz_strtol(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) return v;
            if (pos == 0) break;
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) root_id = parse_ref_id_after_key(raw, "/Root");
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) return {};

    std::string out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') out.push_back('\n');

    struct XrefEntry { int id; size_t offset; };
    std::vector<XrefEntry> changed;

    // Append updated objects
    for (const auto& kv : updated_objects) {
        int id = kv.first;
        const std::string& content = kv.second;
        changed.push_back({id, out.size()});
        out += content;
        if (!content.empty() && content.back() != '\n') out.push_back('\n');
    }

    // Append new objects
    int current_max_id = get_max_obj_id();
    for (const auto& kv : new_objects) {
        int id = kv.first;
        const std::string& content = kv.second;
        changed.push_back({id, out.size()});
        out += content;
        if (!content.empty() && content.back() != '\n') out.push_back('\n');
        current_max_id = (std::max)(current_max_id, id);
    }

    if (changed.empty()) return {};

    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) { return a.id < b.id; });

    const size_t xref_offset = out.size();
    out += "xref\n";
    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) ++j;
        out += std::to_string(changed[i].id) + " " + std::to_string(j - i) + "\n";
        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n", static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    auto extract_trailer_entries = [&](const std::string& src, long startxref_offset) -> std::string {
        std::string entries;
        if (startxref_offset < 0 || startxref_offset >= src.size()) return entries;
        size_t pos = startxref_offset;
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) pos++;
        bool is_stream = false;
        size_t dict_start = std::string::npos;
        if (pos + 4 <= src.size() && src.substr(pos, 4) == "xref") {
            size_t trailer_pos = src.find("trailer", pos);
            if (trailer_pos != std::string::npos) {
                dict_start = src.find("<<", trailer_pos);
            }
        } else {
            is_stream = true;
            dict_start = src.find("<<", pos);
        }
        
        if (dict_start != std::string::npos) {
            int depth = 0;
            size_t dict_end = std::string::npos;
            for (size_t k = dict_start; k < src.size(); k++) {
                if (src[k] == '<' && k + 1 < src.size() && src[k+1] == '<') { depth++; k++; }
                else if (src[k] == '>' && k + 1 < src.size() && src[k+1] == '>') {
                    depth--; k++;
                    if (depth == 0) { dict_end = k - 1; break; }
                }
            }
            if (dict_end != std::string::npos) {
                std::string dict_content = src.substr(dict_start + 2, dict_end - dict_start - 2);
                size_t id_pos = dict_content.find("/ID");
                if (id_pos != std::string::npos) {
                    size_t arr_start = dict_content.find("[", id_pos);
                    if (arr_start != std::string::npos) {
                        size_t arr_end = dict_content.find("]", arr_start);
                        if (arr_end != std::string::npos) {
                            entries += " /ID " + dict_content.substr(arr_start, arr_end - arr_start + 1);
                        }
                    }
                }
                size_t info_pos = dict_content.find("/Info");
                if (info_pos != std::string::npos) {
                    size_t v_start = info_pos + 5;
                    while (v_start < dict_content.size() && std::isspace(static_cast<unsigned char>(dict_content[v_start]))) v_start++;
                    size_t r_pos = dict_content.find(" R", v_start);
                    if (r_pos != std::string::npos) {
                        entries += " /Info " + dict_content.substr(v_start, r_pos - v_start + 2);
                    }
                }
            }
        }
        if (is_stream) {
            entries += " /XRefStm " + std::to_string(startxref_offset);
        }
        return entries;
    };

    std::string extra_trailer = extract_trailer_entries(raw, prev_startxref);
    const int size_value = current_max_id + 1;
    out += "trailer\n<< /Size " + std::to_string(size_value) + " /Root " + std::to_string(root_id) +
           " 0 R /Prev " + std::to_string(prev_startxref) + extra_trailer + " >>\nstartxref\n" +
           std::to_string(xref_offset) + "\n%%EOF\n";

    return std::vector<uint8_t>(out.begin(), out.end());
}

bool WinPdfDocument::save_page_content_incremental(
        int page_idx,
        const std::vector<uint8_t>& decoded_stream,
        const std::string& output_path) {
    if (page_idx < 0 || page_ids.empty() || page_idx >= static_cast<int>(page_ids.size())) {
        return false;
    }
    if (output_path.empty() || file_view_.empty()) {
        return false;
    }

    WinPdfObject page_obj = read_obj(page_ids[page_idx]);
    if (page_obj.id <= 0 || page_obj.dict.empty()) {
        return false;
    }

    std::string raw(file_view_.data(), file_view_.size());

    auto find_prev_startxref = [&](const std::string& src) -> long {
        size_t pos = src.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) {
                ++p;
            }

            char* end_ptr = nullptr;
            long v = wz_strtol(src.c_str() + p, &end_ptr, 10);
            if (end_ptr != src.c_str() + p && v >= 0) {
                return v;
            }

            if (pos == 0) {
                break;
            }
            pos = src.rfind("startxref", pos - 1);
        }
        return -1;
    };

    auto replace_contents_entry = [&](const std::string& dict, const std::string& new_ref) {
        auto append_at_end = [&](const std::string& src) {
            std::string out = src;
            size_t dict_end = out.rfind(">>");
            if (dict_end == std::string::npos) {
                return out;
            }
            out.insert(dict_end, " /Contents " + new_ref + " ");
            return out;
        };

        size_t key_pos = std::string::npos;
        size_t search_pos = 0;
        while (search_pos < dict.size()) {
            size_t found = dict.find("/Contents", search_pos);
            if (found == std::string::npos) {
                break;
            }

            const size_t after = found + 9;
            const char c = (after < dict.size()) ? dict[after] : ' ';
            if (after >= dict.size() || std::isspace(static_cast<unsigned char>(c)) ||
                c == '[' || c == '<' || c == '/' || std::isdigit(static_cast<unsigned char>(c))) {
                key_pos = found;
                break;
            }
            search_pos = after;
        }

        if (key_pos == std::string::npos) {
            return append_at_end(dict);
        }

        size_t value_start = key_pos + 9;
        while (value_start < dict.size() && std::isspace(static_cast<unsigned char>(dict[value_start]))) {
            ++value_start;
        }

        size_t value_end = value_start;

        if (value_start < dict.size() && dict[value_start] == '[') {
            int depth = 0;
            for (size_t i = value_start; i < dict.size(); ++i) {
                if (dict[i] == '[') {
                    ++depth;
                } else if (dict[i] == ']') {
                    --depth;
                    if (depth == 0) {
                        value_end = i + 1;
                        break;
                    }
                }
            }
        }

        if (value_end == value_start) {
            const char* p = dict.c_str() + value_start;
            char* e1 = nullptr;
            long v1 = wz_strtol(p, &e1, 10);
            if (e1 != p && v1 > 0) {
                char* e2 = nullptr;
                long v2 = wz_strtol(e1, &e2, 10);
                if (e2 != e1 && v2 >= 0) {
                    while (*e2 && std::isspace(static_cast<unsigned char>(*e2))) {
                        ++e2;
                    }
                    if (*e2 == 'R') {
                        ++e2;
                        value_end = static_cast<size_t>(e2 - dict.c_str());
                    }
                }
            }
        }

        if (value_end == value_start) {
            while (value_end < dict.size() &&
                   !std::isspace(static_cast<unsigned char>(dict[value_end])) &&
                   dict[value_end] != '/' && dict[value_end] != '>') {
                ++value_end;
            }
        }

        if (value_end <= value_start) {
            return append_at_end(dict);
        }

        std::string out = dict;
        out.replace(value_start, value_end - value_start, new_ref);
        return out;
    };

    if (root_id <= 0) {
        size_t last_trailer = raw.rfind("trailer");
        if (last_trailer != std::string::npos) {
            root_id = parse_ref_id_after_key(raw.substr(last_trailer), "/Root");
        }
        if (root_id <= 0) {
            root_id = parse_ref_id_after_key(raw, "/Root");
        }
    }

    const long prev_startxref = find_prev_startxref(raw);
    if (prev_startxref < 0 || root_id <= 0) {
        return false;
    }

    build_objstm_index();

    int max_obj_id = 0;
    for (const auto& kv : xref) {
        max_obj_id = (std::max)(max_obj_id, kv.first);
    }
    for (const auto& kv : objstm_lookup) {
        max_obj_id = (std::max)(max_obj_id, kv.first);
    }
    for (int pid : page_ids) {
        max_obj_id = (std::max)(max_obj_id, pid);
    }
    max_obj_id = (std::max)(max_obj_id, root_id);

    const int new_stream_id = max_obj_id + 1;
    const std::string new_ref = std::to_string(new_stream_id) + " 0 R";
    const std::string updated_page_dict = replace_contents_entry(page_obj.dict, new_ref);

    std::string out = raw;
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') {
        out.push_back('\n');
    }

    const size_t stream_offset = out.size();
    out += std::to_string(new_stream_id);
    out += " 0 obj\n";
    out += "<< /Length ";
    out += std::to_string(decoded_stream.size());
    out += " >>\nstream\n";
    if (!decoded_stream.empty()) {
        out.append(reinterpret_cast<const char*>(decoded_stream.data()), decoded_stream.size());
    }
    if (decoded_stream.empty() || (out.back() != '\n' && out.back() != '\r')) {
        out.push_back('\n');
    }
    out += "endstream\nendobj\n";

    const size_t page_offset = out.size();
    out += std::to_string(page_obj.id);
    out += " ";
    out += std::to_string(page_obj.gen);
    out += " obj\n";
    out += updated_page_dict;
    if (updated_page_dict.empty() || (out.back() != '\n' && out.back() != '\r')) {
        out.push_back('\n');
    }
    out += "endobj\n";

    struct XrefEntry {
        int id;
        size_t offset;
    };

    std::vector<XrefEntry> changed = {
        {page_obj.id, page_offset},
        {new_stream_id, stream_offset},
    };
    std::sort(changed.begin(), changed.end(), [](const XrefEntry& a, const XrefEntry& b) {
        return a.id < b.id;
    });

    const size_t xref_offset = out.size();
    out += "xref\n";

    size_t i = 0;
    while (i < changed.size()) {
        size_t j = i + 1;
        while (j < changed.size() && changed[j].id == changed[j - 1].id + 1) {
            ++j;
        }

        out += std::to_string(changed[i].id);
        out += " ";
        out += std::to_string(j - i);
        out += "\n";

        for (size_t k = i; k < j; ++k) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                          static_cast<unsigned long long>(changed[k].offset));
            out += entry;
        }
        i = j;
    }

    const int size_value = (std::max)(new_stream_id, max_obj_id) + 1;
    out += "trailer\n<< /Size ";
    out += std::to_string(size_value);
    out += " /Root ";
    out += std::to_string(root_id);
    out += " 0 R /Prev ";
    out += std::to_string(prev_startxref);
    out += " >>\n";
    out += "startxref\n";
    out += std::to_string(xref_offset);
    out += "\n%%EOF\n";

    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file.good();
}

std::vector<uint8_t> WinPdfDocument::get_embedded_font_stream(int font_obj_id) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    
    WinPdfObject font_obj = read_obj(font_obj_id);
    if (font_obj.dict.empty()) return {};

    WinPdfObject target_font = font_obj;
    if (font_obj.dict.find("/Type0") != std::string::npos) {
        WinPdfObject descendant;
        if (resolve_type0_descendant_font(font_obj, descendant)) {
            target_font = descendant;
        }
    }

    std::string descriptor_dict;
    if (!resolve_font_descriptor_dict(target_font, descriptor_dict)) {
        return {}; 
    }

    int font_file_id = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
    if (font_file_id <= 0) {
        font_file_id = parse_ref_id_after_key(descriptor_dict, "/FontFile3");
    }
    if (font_file_id <= 0) {
        font_file_id = parse_ref_id_after_key(descriptor_dict, "/FontFile"); // Type1
    }

    if (font_file_id > 0) {
        WinPdfObject stream_obj = read_obj(font_file_id);
        if (stream_obj.is_stream && !stream_obj.stream.empty()) {
            return stream_obj.stream; 
        }
    }

    return {};
}

WinFontUnicodeMap WinPdfDocument::get_page_font_unicode_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontUnicodeMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

#ifdef WINEXTRACT_USE_FREETYPE
    struct CidToGidMapData {
        bool has_map = false;
        bool identity = false;
        std::vector<uint16_t> values;
    };

    auto resolve_type0_descendant_font = [&](const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj) -> bool {
        descendant_font_obj = {};
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") {
            return false;
        }

        std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
        if (descendant_ids.empty()) {
            int single_descendant = parse_ref_id_after_key(font_obj.dict, "/DescendantFonts");
            if (single_descendant > 0) {
                descendant_ids.push_back(single_descendant);
            }
        }
        if (descendant_ids.empty() || descendant_ids.front() <= 0) {
            return false;
        }

        descendant_font_obj = read_obj(descendant_ids.front());
        return descendant_font_obj.id > 0 || !descendant_font_obj.dict.empty() || !descendant_font_obj.body.empty();
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        WinPdfObject descendant_font_obj;
        if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return read_descriptor_from_dict(descendant_font_obj.dict);
        }

        return false;
    };

    auto load_cid_to_gid_map = [&](const WinPdfObject& font_obj) -> CidToGidMapData {
        CidToGidMapData cid_to_gid;

        WinPdfObject descendant_font_obj;
        if (!resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return cid_to_gid;
        }

        const std::string direct_map_name = parse_name_value_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (direct_map_name == "Identity") {
            cid_to_gid.has_map = true;
            cid_to_gid.identity = true;
            return cid_to_gid;
        }

        const int map_ref = parse_ref_id_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (map_ref <= 0) {
            if (direct_map_name.empty()) {
                cid_to_gid.has_map = true;
                cid_to_gid.identity = true;
            }
            return cid_to_gid;
        }

        WinPdfObject map_obj = read_obj(map_ref);
        if (!map_obj.is_stream || map_obj.stream.empty()) {
            return cid_to_gid;
        }

        std::vector<uint8_t> decoded = decode_stream_data(map_obj.stream, map_obj.dict, this);
        if (decoded.empty()) {
            decoded = map_obj.stream;
        }
        if (decoded.size() < 2) {
            return cid_to_gid;
        }

        cid_to_gid.values.reserve(decoded.size() / 2);
        for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
            const uint16_t gid = static_cast<uint16_t>((static_cast<uint16_t>(decoded[i]) << 8) | decoded[i + 1]);
            cid_to_gid.values.push_back(gid);
        }

        cid_to_gid.has_map = !cid_to_gid.values.empty();
        return cid_to_gid;
    };

    auto fill_missing_unicode_from_freetype = [&](const WinPdfObject& font_obj,
                                                  std::unordered_map<int, std::vector<int>>& unicode_map,
                                                  const std::map<int, std::string>& diff_names) {
        FT_Library library = get_freetype_library();
        if (!library) {
            return;
        }

        const bool is_type0_subtype = parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0";
        const CidToGidMapData cid_to_gid = load_cid_to_gid_map(font_obj);

        std::string descriptor_dict;
        resolve_font_descriptor_dict(font_obj, descriptor_dict);

        std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        if (base_font_name.empty() && is_type0_subtype) {
            WinPdfObject descendant_font_obj;
            if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
                base_font_name = parse_name_value_after_key(descendant_font_obj.dict, "/BaseFont");
            }
        }
        base_font_name = normalize_pdf_font_name(base_font_name);

        FT_Face face = nullptr;
        std::vector<uint8_t> font_bytes;

        if (!descriptor_dict.empty()) {
            int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

            if (font_file_ref > 0) {
                WinPdfObject font_file_obj = read_obj(font_file_ref);
                if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                    font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                    if (font_bytes.empty()) {
                        font_bytes = font_file_obj.stream;
                    }
                    if (!font_bytes.empty()) {
                        if (FT_New_Memory_Face(library,
                                               reinterpret_cast<const FT_Byte*>(font_bytes.data()),
                                               static_cast<FT_Long>(font_bytes.size()),
                                               0,
                                               &face) != 0) {
                            face = nullptr;
                        }
                    }
                }
            }
        }

        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    break;
                }
            }
        }

        if (!face) {
            return;
        }

        std::unordered_map<unsigned int, int> gid_to_unicode;

        auto collect_gid_unicode = [&]() {
            if (face->charmap == nullptr) {
                return;
            }

            FT_UInt gid = 0;
            FT_ULong charcode = FT_Get_First_Char(face, &gid);
            while (gid != 0) {
                if (charcode > 0 && charcode <= 0x10FFFFUL && gid_to_unicode.find(gid) == gid_to_unicode.end()) {
                    gid_to_unicode[gid] = static_cast<int>(charcode);
                }
                charcode = FT_Get_Next_Char(face, charcode, &gid);
            }

            if (FT_HAS_GLYPH_NAMES(face)) {
                for (FT_UInt g = 0; g < (FT_UInt)face->num_glyphs; ++g) {
                    if (gid_to_unicode.find(g) == gid_to_unicode.end()) {
                        char gname[64];
                        if (FT_Get_Glyph_Name(face, g, gname, sizeof(gname)) == 0) {
                            int cp = glyph_name_to_unicode(std::string(gname));
                            if (cp > 0) {
                                gid_to_unicode[g] = cp;
                            }
                        }
                    }
                }
            }
        };

        FT_CharMap saved_charmap = face->charmap;
        if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
            collect_gid_unicode();
        }
        if (face->num_charmaps > 0 && face->charmaps != nullptr) {
            for (int ci = 0; ci < face->num_charmaps; ++ci) {
                FT_CharMap cmap = face->charmaps[ci];
                if (cmap == nullptr || cmap == face->charmap) {
                    continue;
                }
                if (FT_Set_Charmap(face, cmap) != 0) {
                    continue;
                }
                collect_gid_unicode();
            }
        }
        if (saved_charmap != nullptr && face->charmap != saved_charmap) {
            FT_Set_Charmap(face, saved_charmap);
        }

        auto lookup_gid_for_code = [&](int code) -> FT_UInt {
            if (code < 0) {
                return 0;
            }

            if (is_type0_subtype) {
                if (cid_to_gid.has_map) {
                    if (cid_to_gid.identity) {
                        const FT_UInt gid = static_cast<FT_UInt>(code);
                        if (face->num_glyphs <= 0 || gid < static_cast<FT_UInt>(face->num_glyphs)) {
                            return gid;
                        }
                        return 0;
                    }
                    if (static_cast<size_t>(code) < cid_to_gid.values.size()) {
                        const FT_UInt gid = static_cast<FT_UInt>(cid_to_gid.values[static_cast<size_t>(code)]);
                        return gid;
                    }
                }
                return 0;
            }

            FT_UInt gid = 0;
            auto diff_it = diff_names.find(code);
            if (diff_it != diff_names.end() && FT_HAS_GLYPH_NAMES(face)) {
                gid = FT_Get_Name_Index(face, const_cast<FT_String*>(diff_it->second.c_str()));
            }

            if (gid == 0) {
                gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
            }
            if (gid != 0) {
                return gid;
            }

            FT_CharMap saved_charmap = face->charmap;
            if (face->num_charmaps > 0 && face->charmaps != nullptr) {
                for (int ci = 0; ci < face->num_charmaps; ++ci) {
                    FT_CharMap cmap = face->charmaps[ci];
                    if (cmap == nullptr || cmap == face->charmap) {
                        continue;
                    }
                    if (FT_Set_Charmap(face, cmap) != 0) {
                        continue;
                    }
                    gid = 0;
                    if (diff_it != diff_names.end() && FT_HAS_GLYPH_NAMES(face)) {
                        gid = FT_Get_Name_Index(face, const_cast<FT_String*>(diff_it->second.c_str()));
                    }
                    if (gid == 0) {
                        gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
                    }
                    if (gid != 0) {
                        break;
                    }
                }
            }

            if (saved_charmap != nullptr && face->charmap != saved_charmap) {
                FT_Set_Charmap(face, saved_charmap);
            }

            return gid;
        };

        int max_code = 255;
        if (is_type0_subtype) {
            max_code = 65535;
            if (cid_to_gid.has_map && !cid_to_gid.identity && !cid_to_gid.values.empty()) {
                const size_t capped_size = (std::min)(cid_to_gid.values.size(), static_cast<size_t>(65536));
                if (capped_size > 0) {
                    max_code = static_cast<int>(capped_size - 1);
                }
            }
        }


        int consecutive_ocr_failures = 0;

        for (int code = 0; code <= max_code; ++code) {
            FT_UInt gid = lookup_gid_for_code(code);
            if (gid == 0) {
                continue;
            }

            auto it_check = unicode_map.find(code);
            bool is_missing_in_pdf_cmap = (it_check == unicode_map.end() || it_check->second.empty() || 
                                          (it_check->second.size() == 1 && (it_check->second[0] == 0xFFFD || it_check->second[0] == 0)));
            if (!is_missing_in_pdf_cmap) {
                // PDF's ToUnicode table already provides a valid mapping. Do nothing.
                continue;
            }

            int cp = 0;
            auto unicode_it = gid_to_unicode.find(gid);
            if (unicode_it != gid_to_unicode.end()) {
                cp = unicode_it->second;
            }

            auto is_invalid_cp = [](int c) {
                return c <= 0 || c > 0x10FFFF || c == 0xFFFD || (c < 32 && c != 9 && c != 10 && c != 13);
            };

            if (is_invalid_cp(cp) && FT_HAS_GLYPH_NAMES(face)) {
                char glyph_name[256] = {0};
                if (FT_Get_Glyph_Name(face, gid, glyph_name, static_cast<FT_UInt>(sizeof(glyph_name))) == 0 && glyph_name[0] != '\0') {
                    cp = glyph_name_to_unicode(std::string(glyph_name));
                }
            }

            if (is_invalid_cp(cp)) {
                if (apply_heuristic_vni_tcvn3(base_font_name, code, cp)) {
                    // handled
                } else if (consecutive_ocr_failures < 20) {
                    std::vector<int> ocr_cps = run_micro_ocr_on_glyph(face, gid);
                    if (!ocr_cps.empty()) {
                        unicode_map[code] = ocr_cps;
                        cp = ocr_cps.front(); // just for flow below, though map is already updated
                        consecutive_ocr_failures = 0; // Reset counter on success!
                    } else {
                        consecutive_ocr_failures++; // Increment on failure
                    }
                }
            }

            if (cp > 0 && cp <= 0x10FFFF) {
                auto it = unicode_map.find(code);
                bool should_update = false;
                if (it == unicode_map.end() || it->second.empty()) {
                    should_update = true;
                } else if (it->second.size() == 1 && (it->second[0] == 0xFFFD || it->second[0] == 0)) {
                    should_update = true;
                }
                if (should_update) {
                    unicode_map[code] = {cp};
                }
            }
        }

        if (face) {
            FT_Done_Face(face);
        }
    };
#endif

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_unicode_maps.count(font_obj_id)) {
            out[it.first] = cached_unicode_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        const std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
        const bool is_type0_subtype = (subtype == "Type0");
        const bool is_type3_subtype = is_type3_font_subtype(subtype);
        std::unordered_map<int, std::vector<int>> cmap;
        std::string cid_collection;
        int cmap_ref = parse_ref_id_after_key(font_obj.dict, "/ToUnicode");
        if (cmap_ref > 0) {
            WinPdfObject cmap_obj = read_obj(cmap_ref);
            if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                if (cmap_stream.empty()) {
                    cmap_stream = cmap_obj.stream;
                }

                cmap = parse_tounicode_cmap(cmap_stream);
            }
        }

        if (cmap.empty()) {
            std::string to_unicode_name = parse_name_value_after_key(font_obj.dict, "/ToUnicode");
            if (!to_unicode_name.empty()) {
                const auto& named_cmap = load_system_unicode_cmap_by_name(to_unicode_name);
                if (!named_cmap.empty()) {
                    cmap = named_cmap;
                }
            }
        }

std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        if (base_font_name.empty() && is_type0_subtype) {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                base_font_name = parse_name_value_after_key(cid_font_obj.dict, "/BaseFont");
            }
        }
        std::string lower_font = to_lower_ascii(normalize_pdf_font_name(base_font_name));
        

        for (auto it_cmap = cmap.begin(); it_cmap != cmap.end(); ) {
            bool has_invalid = false;
            for (int& ucs : it_cmap->second) {
                if (ucs >= 128 && ucs <= 159) {
                    static const unsigned short win_ansi_to_unicode[32] = {
                        0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
                        0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
                    };
                    unsigned short mapped = win_ansi_to_unicode[ucs - 128];
                    if (mapped != 0) ucs = mapped;
                }

                if (ucs >= 0 && ucs < 32 && ucs != 9 && ucs != 10 && ucs != 13) {
                    has_invalid = true;
                    break;
                }
            }
            if (has_invalid) {
                it_cmap = cmap.erase(it_cmap);
            } else {
                ++it_cmap;
            }
        }

        if (is_type0_subtype && cmap.empty()) {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());

                std::string cid_system_info_dict;
                int cid_system_info_ref = parse_ref_id_after_key(cid_font_obj.dict, "/CIDSystemInfo");
                if (cid_system_info_ref > 0) {
                    WinPdfObject cid_system_info_obj = read_obj(cid_system_info_ref);
                    cid_system_info_dict = cid_system_info_obj.dict;
                }
                if (cid_system_info_dict.empty()) {
                    extract_inline_dict_after_key(cid_font_obj.dict, "/CIDSystemInfo", cid_system_info_dict);
                }

                cid_collection = build_cid_collection_name_from_cidsysteminfo(cid_system_info_dict);
                std::string temp_enc = parse_name_value_after_key(font_obj.dict, "/Encoding");
                bool is_utf16_encoding = (temp_enc.find("UTF16") != std::string::npos || temp_enc.find("UCS2") != std::string::npos);
                if (!cid_collection.empty() && !is_utf16_encoding) {
                    const auto& cid_fallback = load_collection_unicode_cmap(cid_collection);
                    if (!cid_fallback.empty()) {
                        cmap = cid_fallback;
                    }
                }
            }
        }

        std::string encoding_dict;
        std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
        if (encoding_name.empty()) {
            int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
            if (encoding_ref > 0) {
                WinPdfObject enc_obj = read_obj(encoding_ref);
                encoding_dict = enc_obj.dict;
                encoding_name = parse_name_value_after_key(enc_obj.dict, "/BaseEncoding");
                if (encoding_name.empty()) {
                    encoding_name = parse_name_value_after_key(enc_obj.body, "/BaseEncoding");
                }
            }
        }
        if (encoding_dict.empty()) {
            extract_inline_dict_after_key(font_obj.dict, "/Encoding", encoding_dict);
            if (!encoding_dict.empty() && encoding_name.empty()) {
                encoding_name = parse_name_value_after_key(encoding_dict, "/BaseEncoding");
            }
        }

        std::map<int, std::string> diff_names;
        if (!encoding_dict.empty()) {
            std::map<int, int> dummy;
            apply_differences_to_map(encoding_dict, dummy, &diff_names);
        }

        if (!is_type0_subtype) {
            std::map<int, int> fallback = build_encoding_map(encoding_name);
            if (fallback.empty()) {
                fallback = make_identity_encoding_map();
            }

            if (!encoding_dict.empty()) {
                apply_differences_to_map(encoding_dict, fallback);
            }

            if (!fallback.empty()) {
                for (const auto& kv : fallback) {
                    if (cmap.find(kv.first) == cmap.end()) {
                        cmap[kv.first] = {kv.second};
                    }
                }
            }
        }

#ifdef WINEXTRACT_USE_FREETYPE
        // Run FreeType fallback to fill any gaps, even if the font has a partial ToUnicode cmap
        // or a maliciously broken one.
        // Disabled to prevent Mojibake on Windows caused by obfuscated TrueType subset cmaps
        // fill_missing_unicode_from_freetype(font_obj, cmap, diff_names);
#endif

        if (is_type3_subtype) {
            apply_type3_ascii_fallback(cmap);
        }

        const std::unordered_map<int, int>* final_fallback = nullptr;
        if (lower_font.find("cmmi") != std::string::npos) {
            final_fallback = &get_oml_mapping();
        } else if (lower_font.find("cmsy") != std::string::npos) {
            final_fallback = &get_oms_mapping();
        } else if (lower_font.find("symbol") != std::string::npos) {
            final_fallback = &get_symbol_mapping();
        } else if (lower_font.find("zapfdingbats") != std::string::npos || lower_font.find("dingbats") != std::string::npos) {
            final_fallback = &get_zapf_dingbats_mapping();
        }

            if (final_fallback) {
            for (int i = 0; i <= 255; ++i) {
                auto it_fallback = final_fallback->find(i);
                if (it_fallback != final_fallback->end()) {
                    cmap[i] = {it_fallback->second};
                }
            }
        }


        if (!cmap.empty()) {
            cached_unicode_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>(std::move(cmap));
            out[it.first] = cached_unicode_maps[font_obj_id];
        } else {
            cached_unicode_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>();
            out[it.first] = cached_unicode_maps[font_obj_id];
        }
    }

    return out;
}

WinFontCodeBytesMap WinPdfDocument::get_page_font_code_bytes_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontCodeBytesMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    const WinFontCodeSpaceMap code_space_map = get_page_font_codespace_map(page_idx);

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_code_bytes_maps.count(font_obj_id)) {
            out[it.first] = cached_code_bytes_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        int code_bytes = 1;
        bool is_type0 = (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") ||
                        (font_obj.dict.find("/Subtype /Type0") != std::string::npos) ||
                        (font_obj.dict.find("/Subtype/Type0") != std::string::npos) ||
                        (font_obj.dict.find("/Encoding /Identity-H") != std::string::npos) ||
                        (font_obj.dict.find("/Encoding/Identity-H") != std::string::npos);
        if (is_type0) {
            code_bytes = 2;
            auto cs_it = code_space_map.find(it.first);
            if (cs_it != code_space_map.end()) {
                for (const auto& r : *cs_it->second) {
                    if (r.nbytes > code_bytes) {
                        code_bytes = r.nbytes;
                    }
                }
            }
        }

        int result = std::max(1, code_bytes);
        cached_code_bytes_maps[font_obj_id] = result;
        out[it.first] = result;
    }

    return out;
}

WinFontCodeSpaceMap WinPdfDocument::get_page_font_codespace_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontCodeSpaceMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_codespace_maps.count(font_obj_id)) {
            out[it.first] = cached_codespace_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") {
            continue;
        }

        std::vector<WinCodeSpaceRange> ranges;

        int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
        if (encoding_ref > 0) {
            WinPdfObject enc_obj = read_obj(encoding_ref);
            if (enc_obj.is_stream && !enc_obj.stream.empty()) {
                std::vector<uint8_t> cmap_stream = decode_stream_data(enc_obj.stream, enc_obj.dict, this);
                if (cmap_stream.empty()) {
                    cmap_stream = enc_obj.stream;
                }
                if (!cmap_stream.empty()) {
                    ranges = parse_cmap_codespace_ranges(cmap_stream);
                }
            }
        }

        if (ranges.empty()) {
            std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
            if (ranges.empty() && (encoding_name == "Identity-H" || encoding_name == "Identity-V")) {
                WinCodeSpaceRange r;
                r.nbytes = 2;
                r.low = 0x0000u;
                r.high = 0xFFFFu;
                ranges.push_back(r);
            }
        }

        if (ranges.empty()) {
            int cmap_ref = parse_ref_id_after_key(font_obj.dict, "/ToUnicode");
            if (cmap_ref > 0) {
                WinPdfObject cmap_obj = read_obj(cmap_ref);
                if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                    std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                    if (cmap_stream.empty()) {
                        cmap_stream = cmap_obj.stream;
                    }
                    if (!cmap_stream.empty()) {
                        ranges = parse_cmap_codespace_ranges(cmap_stream);
                    }
                }
            }
        }

        if (!ranges.empty()) {
            cached_codespace_maps[font_obj_id] = std::make_shared<const std::vector<WinCodeSpaceRange>>(std::move(ranges));
            out[it.first] = cached_codespace_maps[font_obj_id];
        } else {
            cached_codespace_maps[font_obj_id] = std::make_shared<const std::vector<WinCodeSpaceRange>>();
            out[it.first] = cached_codespace_maps[font_obj_id];
        }
    }

    return out;
}

WinFontMatrixMap WinPdfDocument::get_page_font_matrix_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontMatrixMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }
        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_matrix_array = [](const std::string& array_text, std::array<float, 6>& out_matrix) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[6] = {0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 6; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = wz_strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_matrix = {
            static_cast<float>(v[0]), static_cast<float>(v[1]),
            static_cast<float>(v[2]), static_cast<float>(v[3]),
            static_cast<float>(v[4]), static_cast<float>(v[5])
        };
        return true;
    };

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& it : font_refs) {
        int font_obj_id = it.second;
        if (cached_matrix_maps.count(font_obj_id)) {
            out[it.first] = cached_matrix_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        std::array<float, 6> matrix = {1, 0, 0, 1, 0, 0};

        std::string matrix_array;
        bool has_matrix = false;
        if (extract_array_after_key(font_obj.dict, "/FontMatrix", matrix_array)) {
            has_matrix = parse_matrix_array(matrix_array, matrix);
        }

        if (!has_matrix && parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                if (extract_array_after_key(cid_font_obj.dict, "/FontMatrix", matrix_array)) {
                    has_matrix = parse_matrix_array(matrix_array, matrix);
                }
            }
        }

        cached_matrix_maps[font_obj_id] = matrix;
        out[it.first] = matrix;
    }

    return out;
}

WinFontVerticalMetricsMap WinPdfDocument::get_page_font_vertical_metrics_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontVerticalMetricsMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }

        if (!resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) {
        return out;
    }

    const WinFontMatrixMap font_matrix_map = get_page_font_matrix_map(page_idx);

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }

        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_bbox_array = [](const std::string& array_text, std::array<float, 4>& out_bbox) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = wz_strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_bbox = {
            static_cast<float>(v[0]),
            static_cast<float>(v[1]),
            static_cast<float>(v[2]),
            static_cast<float>(v[3])
        };
        return true;
    };

    auto parse_float_after = [&](const std::string& dict, const std::string& key, double fallback) -> double {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return fallback;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return fallback;
        }

        char* end_ptr = nullptr;
        double v = wz_strtod(dict.c_str() + pos, &end_ptr);
        if (end_ptr == dict.c_str() + pos) {
            return fallback;
        }
        return v;
    };

    auto parse_metrics_from_descriptor = [&](const std::string& descriptor_dict,
                                             WinFontVerticalMetrics& metrics) -> bool {
        if (descriptor_dict.empty()) {
            return false;
        }

        bool has_value = false;
        const double ascent = parse_float_after(descriptor_dict, "/Ascent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(ascent) && std::fabs(ascent) > 0.001) {
            metrics.ascender = static_cast<float>(std::fabs(ascent / 1000.0));
            has_value = true;
        }

        const double descent = parse_float_after(descriptor_dict, "/Descent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(descent) && std::fabs(descent) > 0.001) {
            metrics.descender = static_cast<float>(descent / 1000.0);
            if (metrics.descender > 0.0f) {
                metrics.descender = -metrics.descender;
            }
            has_value = true;
        }

        metrics.flags = static_cast<int>(parse_float_after(descriptor_dict, "/Flags", 0.0));
        metrics.font_weight = static_cast<float>(parse_float_after(descriptor_dict, "/FontWeight", 400.0));

        return has_value;
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        if (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                if (read_descriptor_from_dict(cid_font_obj.dict)) {
                    return true;
                }
            }
        }

        return false;
    };

#ifdef WINEXTRACT_USE_FREETYPE
    auto fill_metrics_from_freetype = [&](const WinPdfObject& font_obj, WinFontVerticalMetrics& metrics) -> bool {
        FT_Library library = get_freetype_library();
        if (!library) {
            return false;
        }

        std::string descriptor_dict;
        resolve_font_descriptor_dict(font_obj, descriptor_dict);

        std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        base_font_name = normalize_pdf_font_name(base_font_name);

        FT_Face face = nullptr;
        std::vector<uint8_t> font_bytes;

        if (!descriptor_dict.empty()) {
            int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

            if (font_file_ref > 0) {
                WinPdfObject font_file_obj = read_obj(font_file_ref);
                if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                    font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                    if (font_bytes.empty()) {
                        font_bytes = font_file_obj.stream;
                    }
                    if (!font_bytes.empty()) {
                        if (FT_New_Memory_Face(library,
                                               reinterpret_cast<const FT_Byte*>(font_bytes.data()),
                                               static_cast<FT_Long>(font_bytes.size()),
                                               0,
                                               &face) != 0) {
                            face = nullptr;
                        }
                    }
                }
            }
        }

        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    break;
                }
            }
        }

        if (!face) {
            return false;
        }

        const FT_Long units_per_em = (face->units_per_EM > 0) ? face->units_per_EM : 1000;
        const int LOAD_FLAGS = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;
        const FT_Long num_glyphs = face->num_glyphs;

        float asc_max = 0.0f;
        float desc_min = 0.0f;
        bool got_bounds = false;
        int success_count = 0;

        for (FT_Long gid = 0; gid < num_glyphs; ++gid) {
            if (FT_Load_Glyph(face, static_cast<FT_UInt>(gid), LOAD_FLAGS) != 0) continue;
            success_count++;

            FT_BBox cbox;
            if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE && face->glyph->outline.n_contours > 0) {
                FT_Outline_Get_CBox(&face->glyph->outline, &cbox);
            } else {
                const FT_Glyph_Metrics& gm = face->glyph->metrics;
                cbox.xMin = gm.horiBearingX;
                cbox.yMin = gm.horiBearingY - gm.height;
                cbox.xMax = gm.horiBearingX + gm.width;
                cbox.yMax = gm.horiBearingY;
            }

            const float y_max = static_cast<float>(cbox.yMax) / static_cast<float>(units_per_em);
            const float y_min = static_cast<float>(cbox.yMin) / static_cast<float>(units_per_em);

            if (!got_bounds) {
                asc_max = y_max;
                desc_min = y_min;
                got_bounds = true;
            } else {
                if (y_max > asc_max) asc_max = y_max;
                if (y_min < desc_min) desc_min = y_min;
            }
        }
        // Initialize with global font metrics
        if (face->ascender != 0) {
            metrics.ascender = static_cast<float>(face->ascender) / static_cast<float>(units_per_em);
            if (metrics.ascender < 0.0f) metrics.ascender = -metrics.ascender;
        } else {
            metrics.ascender = 0.8f;
        }
        
        if (face->descender != 0) {
            metrics.descender = static_cast<float>(face->descender) / static_cast<float>(units_per_em);
            if (metrics.descender > 0.0f) metrics.descender = -metrics.descender;
        } else {
            metrics.descender = -0.2f;
        }

        if (got_bounds) {
            if (asc_max > metrics.ascender) metrics.ascender = asc_max;
            if (desc_min < metrics.descender) metrics.descender = desc_min;
        }

        FT_Done_Face(face);
        return got_bounds || (face != nullptr);
    };
#endif

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& entry : font_refs) {
        int font_obj_id = entry.second;
        if (cached_vertical_metrics_maps.count(font_obj_id)) {
            out[entry.first] = cached_vertical_metrics_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        const std::string base_font_name = normalize_pdf_font_name(parse_name_value_after_key(font_obj.dict, "/BaseFont"));
        const bool is_type3_subtype = is_type3_font_subtype(parse_name_value_after_key(font_obj.dict, "/Subtype"));

        WinFontVerticalMetrics metrics = get_base14_vertical_metrics(base_font_name);
        metrics.base_font = base_font_name;

        std::string descriptor_dict;
        const bool has_descriptor = resolve_font_descriptor_dict(font_obj, descriptor_dict);
        bool has_metrics = false;
        if (has_descriptor) {
            has_metrics = parse_metrics_from_descriptor(descriptor_dict, metrics);
        }

        if (!has_metrics && is_type3_subtype) {
            std::string bbox_array;
            std::array<float, 4> bbox = {0, 0, 0, 0};
            if (extract_array_after_key(font_obj.dict, "/FontBBox", bbox_array) && parse_bbox_array(bbox_array, bbox)) {
                std::array<float, 6> matrix = {0.001f, 0, 0, 0.001f, 0, 0};
                auto mit = font_matrix_map.find(entry.first);
                if (mit != font_matrix_map.end()) {
                    matrix = mit->second;
                }

                auto transform_point = [&](float x, float y) -> Vec2 {
                    return {
                        x * matrix[0] + y * matrix[2] + matrix[4],
                        x * matrix[1] + y * matrix[3] + matrix[5]
                    };
                };

                Vec2 p0 = transform_point(bbox[0], bbox[1]);
                Vec2 p1 = transform_point(bbox[0], bbox[3]);
                Vec2 p2 = transform_point(bbox[2], bbox[1]);
                Vec2 p3 = transform_point(bbox[2], bbox[3]);

                metrics.ascender = std::max({p0.y, p1.y, p2.y, p3.y});
                metrics.descender = std::min({p0.y, p1.y, p2.y, p3.y});
                has_metrics = true;
            }
        }

#ifdef WINEXTRACT_USE_FREETYPE
        if (!has_metrics) {
            fill_metrics_from_freetype(font_obj, metrics);
        }
#endif

        if (!std::isfinite(metrics.ascender) || metrics.ascender <= 0.01f) {
            metrics.ascender = 0.8f;
        }
        if (!std::isfinite(metrics.descender) || metrics.descender >= -0.01f) {
            metrics.descender = -0.2f;
        }

        metrics.ascender = std::min(metrics.ascender, 3.0f);
        metrics.descender = std::max(metrics.descender, -3.0f);

        populate_font_flags_and_properties(metrics);
        cached_vertical_metrics_maps[font_obj_id] = metrics;
        out[entry.first] = metrics;
    }

    return out;
}

WinColorSpaceMap WinPdfDocument::get_page_color_space_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinColorSpaceMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);
        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }
        if (!resources_dict.empty()) {
            break;
        }
        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) {
        return out;
    }

    std::string colorspace_dict;
    int colorspace_ref = parse_ref_id_after_key(resources_dict, "/ColorSpace");
    if (colorspace_ref > 0) {
        WinPdfObject colorspace_obj = read_obj(colorspace_ref);
        colorspace_dict = colorspace_obj.dict;
        if (colorspace_dict.empty()) {
            colorspace_dict = extract_first_dict_fragment(colorspace_obj.body);
        }
    }
    if (colorspace_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/ColorSpace", colorspace_dict);
    }

    if (!colorspace_dict.empty()) {
        parse_colorspace_dict_into_map(this, colorspace_dict, out);
    }

    return out;
}

std::shared_ptr<const WinFormXObjectMap> WinPdfDocument::get_page_form_xobject_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    if (cached_page_xobject_maps.count(page_idx)) {
        return cached_page_xobject_maps[page_idx];
    }
    
    auto out_ptr = std::make_shared<WinFormXObjectMap>();
    WinFormXObjectMap& out = *out_ptr;
    
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        cached_page_xobject_maps[page_idx] = out_ptr;
        return out_ptr;
    }

    std::string page_resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && page_resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            page_resources_dict = resources_obj.dict;
        }

        if (page_resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", page_resources_dict);
        }

        if (!page_resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (page_resources_dict.empty()) {
        cached_page_xobject_maps[page_idx] = out_ptr;
        return out_ptr;
    }

    if (cached_resource_xobject_maps.count(page_resources_dict)) {
        auto ptr = cached_resource_xobject_maps[page_resources_dict];
        cached_page_xobject_maps[page_idx] = ptr;
        return ptr;
    }

    const WinFontUnicodeMap page_unicode_map = get_page_font_unicode_map(page_idx);
    const WinFontWidthMap page_width_map = get_page_font_width_map(page_idx);
    const WinFontCodeBytesMap page_code_bytes_map = get_page_font_code_bytes_map(page_idx);
    const WinFontCodeSpaceMap page_codespace_map = get_page_font_codespace_map(page_idx);
    const WinFontMatrixMap page_matrix_map = get_page_font_matrix_map(page_idx);
    const WinFontVerticalMetricsMap page_vertical_metrics_map = get_page_font_vertical_metrics_map(page_idx);
    const WinColorSpaceMap page_color_space_map = get_page_color_space_map(page_idx);

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }

        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_matrix_array = [](const std::string& array_text, std::array<float, 6>& out_matrix) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[6] = {0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 6; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = wz_strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_matrix = {
            static_cast<float>(v[0]), static_cast<float>(v[1]),
            static_cast<float>(v[2]), static_cast<float>(v[3]),
            static_cast<float>(v[4]), static_cast<float>(v[5])
        };
        return true;
    };

    auto parse_bbox_array = [](const std::string& array_text, std::array<float, 4>& out_bbox) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        double v[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            v[i] = wz_strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
        }

        out_bbox = {
            static_cast<float>(v[0]),
            static_cast<float>(v[1]),
            static_cast<float>(v[2]),
            static_cast<float>(v[3])
        };
        return true;
    };

    auto parse_float_after = [](const std::string& dict, const std::string& key, double fallback) -> double {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return fallback;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return fallback;
        }

        char* end_ptr = nullptr;
        double v = wz_strtod(dict.c_str() + pos, &end_ptr);
        if (end_ptr == dict.c_str() + pos) {
            return fallback;
        }
        return v;
    };

    auto parse_vertical_metrics_from_descriptor = [&](const std::string& descriptor_dict,
                                                      WinFontVerticalMetrics& metrics) -> bool {
        if (descriptor_dict.empty()) {
            return false;
        }

        bool has_value = false;
        const double ascent = parse_float_after(descriptor_dict, "/Ascent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(ascent) && std::fabs(ascent) > 0.001) {
            metrics.ascender = static_cast<float>(std::fabs(ascent / 1000.0));
            has_value = true;
        }

        const double descent = parse_float_after(descriptor_dict, "/Descent", std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(descent) && std::fabs(descent) > 0.001) {
            metrics.descender = static_cast<float>(descent / 1000.0);
            if (metrics.descender > 0.0f) {
                metrics.descender = -metrics.descender;
            }
            has_value = true;
        }

        return has_value;
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        if (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                if (read_descriptor_from_dict(cid_font_obj.dict)) {
                    return true;
                }
            }
        }

        return false;
    };

    auto resolve_resources_dict = [&](const std::string& object_dict, const std::string& inherited_resources) -> std::string {
        std::string resources_dict;

        int resources_ref = parse_ref_id_after_key(object_dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }

        if (resources_dict.empty()) {
            extract_inline_dict_after_key(object_dict, "/Resources", resources_dict);
        }

        if (resources_dict.empty()) {
            resources_dict = inherited_resources;
        }

        return resources_dict;
    };

    auto merge_fonts_from_resources = [&](const std::string& resources_dict,
                                          WinFontUnicodeMap& unicode_map,
                                          WinFontWidthMap& width_map,
                                          WinFontCodeBytesMap& code_bytes_map,
                                          WinFontCodeSpaceMap& codespace_map,
                                          WinFontMatrixMap& matrix_map,
                                          WinFontVerticalMetricsMap& vertical_metrics_map) {
        if (resources_dict.empty()) {
            return;
        }

        std::string font_dict;
        int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
        if (font_ref > 0) {
            WinPdfObject font_obj = read_obj(font_ref);
            font_dict = font_obj.dict;
        }
        if (font_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
        }
        if (font_dict.empty()) {
            return;
        }

        std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
        for (const auto& it : font_refs) {
            int font_obj_id = it.second;
            
            if (cached_unicode_maps.count(font_obj_id)) {
                unicode_map[it.first] = cached_unicode_maps[font_obj_id];
                if (cached_width_maps.count(font_obj_id)) width_map[it.first] = cached_width_maps[font_obj_id];
                if (cached_code_bytes_maps.count(font_obj_id)) code_bytes_map[it.first] = cached_code_bytes_maps[font_obj_id];
                if (cached_codespace_maps.count(font_obj_id)) codespace_map[it.first] = cached_codespace_maps[font_obj_id];
                if (cached_matrix_maps.count(font_obj_id)) matrix_map[it.first] = cached_matrix_maps[font_obj_id];
                if (cached_vertical_metrics_maps.count(font_obj_id)) vertical_metrics_map[it.first] = cached_vertical_metrics_maps[font_obj_id];
                continue;
            }

            WinPdfObject font_obj = read_obj(font_obj_id);
            const std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
            const bool is_type0_subtype = (subtype == "Type0");
            const bool is_type3_subtype = is_type3_font_subtype(subtype);
            std::string cid_collection;

            int cmap_ref = parse_ref_id_after_key(font_obj.dict, "/ToUnicode");
            std::unordered_map<int, std::vector<int>> cmap;
            if (cmap_ref > 0) {
                WinPdfObject cmap_obj = read_obj(cmap_ref);
                if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                    std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                    if (cmap_stream.empty()) {
                        cmap_stream = cmap_obj.stream;
                    }
                    cmap = parse_tounicode_cmap(cmap_stream);
                }
            }

            if (cmap.empty()) {
                std::string to_unicode_name = parse_name_value_after_key(font_obj.dict, "/ToUnicode");
                if (!to_unicode_name.empty()) {
                    const auto& named_cmap = load_system_unicode_cmap_by_name(to_unicode_name);
                    if (!named_cmap.empty()) {
                        cmap = named_cmap;
                    }
                }
            }

std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        if (base_font_name.empty() && is_type0_subtype) {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                base_font_name = parse_name_value_after_key(cid_font_obj.dict, "/BaseFont");
            }
        }
        std::string lower_font = to_lower_ascii(normalize_pdf_font_name(base_font_name));


            for (auto it_cmap = cmap.begin(); it_cmap != cmap.end(); ) {
            bool has_invalid = false;
            for (int& ucs : it_cmap->second) {
                if (ucs >= 128 && ucs <= 159) {
                    static const unsigned short win_ansi_to_unicode[32] = {
                        0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
                        0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
                    };
                    unsigned short mapped = win_ansi_to_unicode[ucs - 128];
                    if (mapped != 0) ucs = mapped;
                }



                if (ucs >= 0 && ucs < 32 && ucs != 9 && ucs != 10 && ucs != 13) {
                    has_invalid = true;
                    break;
                }
            }
            if (has_invalid) {
                it_cmap = cmap.erase(it_cmap);
            } else {
                ++it_cmap;
            }
        }

            if (is_type0_subtype && cmap.empty()) {
                std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
                if (!descendant_ids.empty()) {
                    WinPdfObject cid_font_obj = read_obj(descendant_ids.front());

                    std::string cid_system_info_dict;
                    int cid_system_info_ref = parse_ref_id_after_key(cid_font_obj.dict, "/CIDSystemInfo");
                    if (cid_system_info_ref > 0) {
                        WinPdfObject cid_system_info_obj = read_obj(cid_system_info_ref);
                        cid_system_info_dict = cid_system_info_obj.dict;
                    }
                    if (cid_system_info_dict.empty()) {
                        extract_inline_dict_after_key(cid_font_obj.dict, "/CIDSystemInfo", cid_system_info_dict);
                    }

                    cid_collection = build_cid_collection_name_from_cidsysteminfo(cid_system_info_dict);
                    if (!cid_collection.empty()) {
                        const auto& cid_fallback = load_collection_unicode_cmap(cid_collection);
                        if (!cid_fallback.empty()) {
                            cmap = cid_fallback;
                        }
                    }
                }
            }

            std::string encoding_dict;
            std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
            if (encoding_name.empty()) {
                int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
                if (encoding_ref > 0) {
                    WinPdfObject enc_obj = read_obj(encoding_ref);
                    encoding_dict = enc_obj.dict;
                    encoding_name = parse_name_value_after_key(enc_obj.dict, "/BaseEncoding");
                    if (encoding_name.empty()) {
                        encoding_name = parse_name_value_after_key(enc_obj.body, "/BaseEncoding");
                    }
                }
            }
            if (encoding_dict.empty()) {
                extract_inline_dict_after_key(font_obj.dict, "/Encoding", encoding_dict);
                if (!encoding_dict.empty() && encoding_name.empty()) {
                    encoding_name = parse_name_value_after_key(encoding_dict, "/BaseEncoding");
                }
            }

            if (!is_type0_subtype) {
                std::map<int, int> fallback = build_encoding_map(encoding_name);
                if (fallback.empty()) {
                    fallback = make_identity_encoding_map();
                }
                if (!encoding_dict.empty()) {
                    apply_differences_to_map(encoding_dict, fallback);
                }

                if (!fallback.empty()) {
                    for (const auto& kv : fallback) {
                        if (cmap.find(kv.first) == cmap.end()) {
                            cmap[kv.first] = {kv.second};
                        }
                    }
                }

            }

            const std::unordered_map<int, int>* final_fallback = nullptr;
            if (lower_font.find("cmmi") != std::string::npos) {
                final_fallback = &get_oml_mapping();
            } else if (lower_font.find("cmsy") != std::string::npos) {
                final_fallback = &get_oms_mapping();
            } else if (lower_font.find("symbol") != std::string::npos) {
                final_fallback = &get_symbol_mapping();
            } else if (lower_font.find("zapfdingbats") != std::string::npos || lower_font.find("dingbats") != std::string::npos) {
                final_fallback = &get_zapf_dingbats_mapping();
            }

            if (final_fallback) {
            for (int i = 0; i <= 255; ++i) {
                auto it_fallback = final_fallback->find(i);
                if (it_fallback != final_fallback->end()) {
                    cmap[i] = {it_fallback->second};
                }
            }
        }

            if (is_type3_subtype) {
                apply_type3_ascii_fallback(cmap);
            }

            if (!cmap.empty()) {
                auto ptr = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>(std::move(cmap));
                unicode_map[it.first] = ptr;
                cached_unicode_maps[font_obj_id] = ptr;
            } else {
                auto ptr = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>();
                unicode_map[it.first] = ptr;
                cached_unicode_maps[font_obj_id] = ptr;
            }

            int code_bytes = 1;
            std::vector<WinCodeSpaceRange> ranges;
            if (is_type0_subtype) {
                code_bytes = 2;

                int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
                if (encoding_ref > 0) {
                    WinPdfObject enc_obj = read_obj(encoding_ref);
                    if (enc_obj.is_stream && !enc_obj.stream.empty()) {
                        std::vector<uint8_t> cmap_stream = decode_stream_data(enc_obj.stream, enc_obj.dict, this);
                        if (cmap_stream.empty()) {
                            cmap_stream = enc_obj.stream;
                        }
                        if (!cmap_stream.empty()) {
                            ranges = parse_cmap_codespace_ranges(cmap_stream);
                        }
                    }
                }

                if (ranges.empty() && (encoding_name == "Identity-H" || encoding_name == "Identity-V")) {
                    WinCodeSpaceRange r;
                    r.nbytes = 2;
                    r.low = 0x0000u;
                    r.high = 0xFFFFu;
                    ranges.push_back(r);
                }

                if (ranges.empty() && cmap_ref > 0) {
                    WinPdfObject cmap_obj = read_obj(cmap_ref);
                    if (cmap_obj.is_stream && !cmap_obj.stream.empty()) {
                        std::vector<uint8_t> cmap_stream = decode_stream_data(cmap_obj.stream, cmap_obj.dict, this);
                        if (cmap_stream.empty()) {
                            cmap_stream = cmap_obj.stream;
                        }
                        if (!cmap_stream.empty()) {
                            ranges = parse_cmap_codespace_ranges(cmap_stream);
                        }
                    }
                }

                for (const auto& r : ranges) {
                    if (r.nbytes > code_bytes) {
                        code_bytes = r.nbytes;
                    }
                }
            }

            int result_code_bytes = std::max(1, code_bytes);
            code_bytes_map[it.first] = result_code_bytes;
            cached_code_bytes_maps[font_obj_id] = result_code_bytes;

            if (!ranges.empty()) {
                auto ptr = std::make_shared<const std::vector<WinCodeSpaceRange>>(std::move(ranges));
                codespace_map[it.first] = ptr;
                cached_codespace_maps[font_obj_id] = ptr;
            } else {
                auto ptr = std::make_shared<const std::vector<WinCodeSpaceRange>>();
                codespace_map[it.first] = ptr;
                cached_codespace_maps[font_obj_id] = ptr;
            }

            std::array<float, 6> matrix = {1, 0, 0, 1, 0, 0};
            std::string matrix_array;
            bool has_matrix = false;
            if (extract_array_after_key(font_obj.dict, "/FontMatrix", matrix_array)) {
                has_matrix = parse_matrix_array(matrix_array, matrix);
            }
            if (!has_matrix && parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
                std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
                if (!descendant_ids.empty()) {
                    WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                    if (extract_array_after_key(cid_font_obj.dict, "/FontMatrix", matrix_array)) {
                        has_matrix = parse_matrix_array(matrix_array, matrix);
                    }
                }
            }
            matrix_map[it.first] = matrix;
            cached_matrix_maps[font_obj_id] = matrix;
            {
                // Local Form resources can reuse font aliases from the parent scope
                // (for example /T1_0). Recompute metrics for the local binding.
                WinFontVerticalMetrics metrics = get_base14_vertical_metrics(parse_name_value_after_key(font_obj.dict, "/BaseFont"));
                
                if (encoding_name == "Identity-V" || (encoding_name.length() >= 2 && encoding_name.substr(encoding_name.length() - 2) == "-V")) {
                    metrics.wmode = 1;
                }
                
                std::string descriptor_dict;
                bool has_metrics = false;
                if (resolve_font_descriptor_dict(font_obj, descriptor_dict)) {
                    has_metrics = parse_vertical_metrics_from_descriptor(descriptor_dict, metrics);
                }

                const bool is_type3_subtype = is_type3_font_subtype(parse_name_value_after_key(font_obj.dict, "/Subtype"));
                if (!has_metrics && is_type3_subtype) {
                    std::string bbox_array;
                    std::array<float, 4> bbox = {0, 0, 0, 0};
                    if (extract_array_after_key(font_obj.dict, "/FontBBox", bbox_array) && parse_bbox_array(bbox_array, bbox)) {
                        std::array<float, 6> t3m = matrix;
                        if (!has_matrix) {
                            t3m = {0.001f, 0, 0, 0.001f, 0, 0};
                        }

                        auto transform_point = [&](float x, float y) -> Vec2 {
                            return {
                                x * t3m[0] + y * t3m[2] + t3m[4],
                                x * t3m[1] + y * t3m[3] + t3m[5]
                            };
                        };

                        Vec2 p0 = transform_point(bbox[0], bbox[1]);
                        Vec2 p1 = transform_point(bbox[0], bbox[3]);
                        Vec2 p2 = transform_point(bbox[2], bbox[1]);
                        Vec2 p3 = transform_point(bbox[2], bbox[3]);

                        metrics.ascender = std::max({p0.y, p1.y, p2.y, p3.y});
                        metrics.descender = std::min({p0.y, p1.y, p2.y, p3.y});
                        has_metrics = true;
                    }
                }
                if (!std::isfinite(metrics.ascender) || metrics.ascender <= 0.01f) {
                    metrics.ascender = 0.8f;
                }
                if (!std::isfinite(metrics.descender) || metrics.descender >= -0.01f) {
                    metrics.descender = -0.2f;
                }
                metrics.ascender = std::min(metrics.ascender, 3.0f);
                metrics.descender = std::max(metrics.descender, -3.0f);
                vertical_metrics_map[it.first] = metrics;
                cached_vertical_metrics_maps[font_obj_id] = metrics;
            }

            {
                // Same alias shadowing rule as above: local Form font widths must
                // replace inherited widths for the same resource name.
                std::unordered_map<int, float> widths;
                const std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
                const bool is_type3_subtype = is_type3_font_subtype(subtype);
                std::string descriptor_dict;
                int descriptor_ref = parse_ref_id_after_key(font_obj.dict, "/FontDescriptor");
                if (descriptor_ref > 0) {
                    WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                    descriptor_dict = descriptor_obj.dict;
                }
                if (descriptor_dict.empty()) {
                    extract_inline_dict_after_key(font_obj.dict, "/FontDescriptor", descriptor_dict);
                }

                float default_width = -1.0f;
                if (!descriptor_dict.empty()) {
                    double missing = parse_float_after(descriptor_dict, "/MissingWidth", -1.0);
                    if (missing > 0.0) {
                        default_width = static_cast<float>(missing / 1000.0);
                    }
                }

                const std::string base_font = normalize_pdf_font_name(parse_name_value_after_key(font_obj.dict, "/BaseFont"));

                auto parse_cid_width_array_local = [](const std::string& array_text, std::unordered_map<int, float>& out_widths) {
                    std::vector<std::string> tokens;
                    for (size_t i = 0; i < array_text.size();) {
                        if (std::isspace(static_cast<unsigned char>(array_text[i]))) {
                            ++i;
                            continue;
                        }
                        if (array_text[i] == '[' || array_text[i] == ']') {
                            tokens.push_back(array_text.substr(i, 1));
                            ++i;
                            continue;
                        }

                        size_t start = i;
                        if (array_text[i] == '+' || array_text[i] == '-') {
                            ++i;
                        }
                        while (i < array_text.size() && (std::isdigit(static_cast<unsigned char>(array_text[i])) || array_text[i] == '.')) {
                            ++i;
                        }
                        if (i > start) {
                            tokens.push_back(array_text.substr(start, i - start));
                        } else {
                            ++i;
                        }
                    }

                    size_t i = 0;
                    while (i < tokens.size()) {
                        if (tokens[i] == "[" || tokens[i] == "]") {
                            ++i;
                            continue;
                        }

                        int c0 = std::atoi(tokens[i].c_str());
                        ++i;
                        if (i >= tokens.size()) {
                            break;
                        }

                        if (tokens[i] == "[") {
                            ++i;
                            int cid = c0;
                            while (i < tokens.size() && tokens[i] != "]") {
                                if (tokens[i] != "[") {
                                    out_widths[cid] = static_cast<float>(wz_strtod(tokens[i].c_str()) / 1000.0);
                                    ++cid;
                                }
                                ++i;
                            }
                            if (i < tokens.size() && tokens[i] == "]") {
                                ++i;
                            }
                        } else {
                            if (i + 1 >= tokens.size()) {
                                break;
                            }
                            int c1 = std::atoi(tokens[i].c_str());
                            ++i;
                            float w = static_cast<float>(wz_strtod(tokens[i].c_str()) / 1000.0);
                            ++i;

                            if (c1 < c0) {
                                std::swap(c0, c1);
                            }
                            for (int cid = c0; cid <= c1; ++cid) {
                                out_widths[cid] = w;
                            }
                        }
                    }
                };

                if (subtype == "Type0") {
                    std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
                    if (!descendant_ids.empty()) {
                        WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                        double dw = parse_float_after(cid_font_obj.dict, "/DW", 1000.0);
                        widths[-1] = static_cast<float>(dw / 1000.0);

                        std::string w_array;
                        if (extract_array_after_key(cid_font_obj.dict, "/W", w_array)) {
                            parse_cid_width_array_local(w_array, widths);
                        }
                    }

                    if (widths.find(-1) == widths.end()) {
                        widths[-1] = default_width > 0.0f ? default_width : get_base14_width(base_font, 'n');
                    }
                } else if (is_type3_subtype) {
                    int first_char = static_cast<int>(parse_float_after(font_obj.dict, "/FirstChar", -1.0));
                    int last_char = static_cast<int>(parse_float_after(font_obj.dict, "/LastChar", -1.0));
                    if (first_char < 0 || last_char > 255 || first_char > last_char) {
                        first_char = 0;
                        last_char = 0;
                    }

                    std::string widths_array;
                    const float type3_width_scale = matrix[0];
                    if (extract_array_after_key(font_obj.dict, "/Widths", widths_array)) {
                        size_t arr_start = widths_array.find('[');
                        size_t arr_end = widths_array.rfind(']');
                        if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
                            const char* p = widths_array.c_str() + arr_start + 1;
                            const char* end = widths_array.c_str() + arr_end;
                            int code = first_char;
                            while (code <= last_char) {
                                while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                                    ++p;
                                }
                                if (p >= end) {
                                    break;
                                }

                                char* end_ptr = nullptr;
                                const double w = wz_strtod(p, &end_ptr);
                                if (end_ptr == p || end_ptr > end) {
                                    break;
                                }
                                p = end_ptr;
                                widths[code] = scale_type3_width(static_cast<float>(w), type3_width_scale);
                                ++code;
                            }
                        }
                    }

                    widths[-1] = 0.0f;
                } else {
                    int first_char = static_cast<int>(parse_float_after(font_obj.dict, "/FirstChar", 0.0));
                    int last_char = static_cast<int>(parse_float_after(font_obj.dict, "/LastChar", 255.0));
                    if (first_char < 0) first_char = 0;
                    if (last_char < first_char || last_char > 255) last_char = 255;

                    std::string widths_array;
                    if (extract_array_after_key(font_obj.dict, "/Widths", widths_array)) {
                        size_t arr_start = widths_array.find('[');
                        size_t arr_end = widths_array.rfind(']');
                        if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
                            const char* p = widths_array.c_str() + arr_start + 1;
                            const char* end = widths_array.c_str() + arr_end;
                            int code = first_char;
                            while (code <= last_char) {
                                while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                                    ++p;
                                }
                                if (p >= end) {
                                    break;
                                }

                                char* end_ptr = nullptr;
                                const double w = wz_strtod(p, &end_ptr);
                                if (end_ptr == p || end_ptr > end) {
                                    break;
                                }
                                p = end_ptr;
                                widths[code] = static_cast<float>(w / 1000.0);
                                ++code;
                            }
                        }
                    }

                    if (default_width <= 0.0f) {
                        default_width = get_base14_width(base_font, 'n');
                    }

                    widths[-1] = default_width;
                    for (int c = first_char; c <= last_char; ++c) {
                        if (widths.find(c) == widths.end()) {
                            widths[c] = get_base14_width(base_font, c);
                        }
                    }
                }
                auto ptr = std::make_shared<const std::unordered_map<int, float>>(std::move(widths));
                width_map[it.first] = ptr;
                cached_width_maps[font_obj_id] = ptr;
            }
        }
    };

    auto merge_color_spaces_from_resources = [&](const std::string& resources_dict,
                                                 WinColorSpaceMap& color_space_map_out) {
        if (resources_dict.empty()) {
            return;
        }

        std::string colorspace_dict;
        int colorspace_ref = parse_ref_id_after_key(resources_dict, "/ColorSpace");
        if (colorspace_ref > 0) {
            WinPdfObject colorspace_obj = read_obj(colorspace_ref);
            colorspace_dict = colorspace_obj.dict;
            if (colorspace_dict.empty()) {
                colorspace_dict = extract_first_dict_fragment(colorspace_obj.body);
            }
        }

        if (colorspace_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/ColorSpace", colorspace_dict);
        }

        if (!colorspace_dict.empty()) {
            parse_colorspace_dict_into_map(this, colorspace_dict, color_space_map_out);
        }
    };

    std::function<WinFormXObjectMap(const std::string&,
                                    const WinFontUnicodeMap&,
                                    const WinFontWidthMap&,
                                    const WinFontCodeBytesMap&,
                                    const WinFontCodeSpaceMap&,
                                    const WinFontMatrixMap&,
                                    const WinFontVerticalMetricsMap&,
                                    const WinColorSpaceMap&,
                                    std::set<int>&)> build_forms;

    int form_count = 0;

    build_forms = [&](const std::string& resources_dict,
                      const WinFontUnicodeMap& inherited_unicode,
                      const WinFontWidthMap& inherited_width,
                      const WinFontCodeBytesMap& inherited_code_bytes,
                      const WinFontCodeSpaceMap& inherited_codespace,
                      const WinFontMatrixMap& inherited_matrix,
                      const WinFontVerticalMetricsMap& inherited_vertical_metrics,
                      const WinColorSpaceMap& inherited_color_space,
                      std::set<int>& recursion_guard) -> WinFormXObjectMap {
        WinFormXObjectMap forms;
        if (++form_count > 1000) {
            return forms;
        }

        if (resources_dict.empty()) {
            return forms;
        }

        std::string xobject_dict;
        int xobject_ref = parse_ref_id_after_key(resources_dict, "/XObject");
        if (xobject_ref > 0) {
            WinPdfObject xobject_obj = read_obj(xobject_ref);
            xobject_dict = xobject_obj.dict;
        }
        if (xobject_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/XObject", xobject_dict);
        }
        if (xobject_dict.empty()) {
            return forms;
        }

        std::map<std::string, int> xobject_refs = parse_font_refs_from_dict(xobject_dict);
        for (const auto& it : xobject_refs) {
            const int xobj_id = it.second;
            if (xobj_id <= 0 || recursion_guard.find(xobj_id) != recursion_guard.end()) {
                continue;
            }

            WinPdfObject xobj = read_obj(xobj_id);
            std::string xobj_dict = xobj.dict;
            if (xobj_dict.empty() || xobj_dict.find("/Subtype") == std::string::npos || xobj_dict.find("/Resources") == std::string::npos) {
                std::string dict_from_body = extract_first_dict_fragment(xobj.body);
                if (!dict_from_body.empty()) {
                    xobj_dict = std::move(dict_from_body);
                }
            }

            if (!xobj.is_stream || xobj.stream.empty()) {
                continue;
            }
            if (xobj_dict.empty() || parse_name_value_after_key(xobj_dict, "/Subtype") != "Form") {
                continue;
            }

            std::shared_ptr<const std::vector<uint8_t>> decoded_ptr;
            if (cached_decoded_streams.count(xobj_id)) {
                decoded_ptr = cached_decoded_streams[xobj_id];
            } else {
                std::vector<uint8_t> decoded = decode_stream_data(xobj.stream, xobj_dict, this);
                if (decoded.empty()) {
                    decoded = xobj.stream;
                }
                decoded_ptr = std::make_shared<const std::vector<uint8_t>>(std::move(decoded));
                cached_decoded_streams[xobj_id] = decoded_ptr;
            }

            if (!decoded_ptr || decoded_ptr->empty()) {
                continue;
            }

            WinFormXObject form;
            form.obj_id = xobj_id;
            form.stream_ptr = decoded_ptr;
            form.font_unicode_map = inherited_unicode;
            form.font_width_map = inherited_width;
            form.font_code_bytes_map = inherited_code_bytes;
            form.font_codespace_map = inherited_codespace;
            form.font_matrix_map = inherited_matrix;
            form.font_vertical_metrics_map = inherited_vertical_metrics;
            form.color_space_map = inherited_color_space;

            std::string child_resources = resolve_resources_dict(xobj_dict, resources_dict);
            if (!child_resources.empty() && child_resources != resources_dict) {
                merge_fonts_from_resources(child_resources,
                                           form.font_unicode_map,
                                           form.font_width_map,
                                           form.font_code_bytes_map,
                                           form.font_codespace_map,
                                           form.font_matrix_map,
                                           form.font_vertical_metrics_map);
                merge_color_spaces_from_resources(child_resources, form.color_space_map);
            }

            std::string matrix_array;
            if (extract_array_after_key(xobj_dict, "/Matrix", matrix_array)) {
                std::array<float, 6> m = {1, 0, 0, 1, 0, 0};
                if (parse_matrix_array(matrix_array, m)) {
                    form.matrix = m;
                }
            }

            std::string bbox_array;
            if (extract_array_after_key(xobj_dict, "/BBox", bbox_array)) {
                std::array<float, 6> b_tmp = {0, 0, 0, 0, 0, 0};
                if (parse_matrix_array(bbox_array, b_tmp)) {
                    form.bbox = {b_tmp[0], b_tmp[1], b_tmp[2], b_tmp[3]};
                    form.has_bbox = true;
                }
            }

            if (!child_resources.empty() && child_resources != resources_dict) {
                recursion_guard.insert(xobj_id);
                form.children = std::make_shared<WinFormXObjectMap>(build_forms(child_resources,
                                                                                form.font_unicode_map,
                                                                                form.font_width_map,
                                                                                form.font_code_bytes_map,
                                                                                form.font_codespace_map,
                                                                                form.font_matrix_map,
                                                                                form.font_vertical_metrics_map,
                                                                                form.color_space_map,
                                                                                recursion_guard));
                recursion_guard.erase(xobj_id);
            }

            forms[it.first] = std::move(form);
        }

        return forms;
    };

    std::set<int> recursion_guard;
    out = build_forms(page_resources_dict,
                      page_unicode_map,
                      page_width_map,
                      page_code_bytes_map,
                      page_codespace_map,
                      page_matrix_map,
                      page_vertical_metrics_map,
                      page_color_space_map,
                      recursion_guard);
    cached_resource_xobject_maps[page_resources_dict] = out_ptr;
    cached_page_xobject_maps[page_idx] = out_ptr;
    return out_ptr;
}

std::shared_ptr<const WinImageXObjectMap> WinPdfDocument::get_page_image_xobject_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    if (cached_page_image_xobject_maps.count(page_idx)) {
        return cached_page_image_xobject_maps[page_idx];
    }
    
    auto out_ptr = std::make_shared<WinImageXObjectMap>();
    WinImageXObjectMap& out = *out_ptr;
    
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        cached_page_image_xobject_maps[page_idx] = out_ptr;
        return out_ptr;
    }

    std::string page_resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && page_resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            page_resources_dict = resources_obj.dict;
        }

        if (page_resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", page_resources_dict);
        }

        if (!page_resources_dict.empty()) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (page_resources_dict.empty()) {
        cached_page_image_xobject_maps[page_idx] = out_ptr;
        return out_ptr;
    }

    if (cached_resource_image_xobject_maps.count(page_resources_dict)) {
        auto ptr = cached_resource_image_xobject_maps[page_resources_dict];
        cached_page_image_xobject_maps[page_idx] = ptr;
        return ptr;
    }

    auto resolve_resources_dict = [&](const std::string& object_dict, const std::string& inherited_resources) -> std::string {
        std::string resources_dict;
        int resources_ref = parse_ref_id_after_key(object_dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) {
            extract_inline_dict_after_key(object_dict, "/Resources", resources_dict);
        }
        if (resources_dict.empty()) {
            resources_dict = inherited_resources;
        }
        return resources_dict;
    };

    auto parse_int_after = [](const std::string& dict, const std::string& key, int fallback) -> int {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) return fallback;
        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) ++pos;
        if (pos >= dict.size()) return fallback;
        return std::atoi(dict.c_str() + pos);
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) return false;
        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) ++pos;
        if (pos >= dict.size() || dict[pos] != '[') return false;
        size_t arr_start = pos;
        int depth = 0;
        for (size_t i = arr_start; i < dict.size(); ++i) {
            if (dict[i] == '[') depth++;
            else if (dict[i] == ']') {
                depth--;
                if (depth == 0) {
                    out_array = dict.substr(arr_start + 1, i - arr_start - 1);
                    return true;
                }
            }
        }
        return false;
    };

    std::function<WinImageXObjectMap(const std::string&, std::set<int>&)> build_images;
    build_images = [&](const std::string& resources_dict, std::set<int>& recursion_guard) -> WinImageXObjectMap {
        WinImageXObjectMap images;
        if (resources_dict.empty()) return images;

        std::string xobject_dict;
        int xobject_ref = parse_ref_id_after_key(resources_dict, "/XObject");
        if (xobject_ref > 0) {
            WinPdfObject xobject_obj = read_obj(xobject_ref);
            xobject_dict = xobject_obj.dict;
        }
        if (xobject_dict.empty()) {
            extract_inline_dict_after_key(resources_dict, "/XObject", xobject_dict);
        }
        if (xobject_dict.empty()) return images;

        std::map<std::string, int> xobject_refs = parse_font_refs_from_dict(xobject_dict);
        for (const auto& it : xobject_refs) {
            const int xobj_id = it.second;
            if (xobj_id <= 0 || recursion_guard.find(xobj_id) != recursion_guard.end()) {
                continue;
            }

            WinPdfObject xobj = read_obj(xobj_id);
            std::string xobj_dict = xobj.dict;
            if (xobj_dict.empty() || xobj_dict.find("/Subtype") == std::string::npos || xobj_dict.find("/Resources") == std::string::npos) {
                std::string dict_from_body = extract_first_dict_fragment(xobj.body);
                if (!dict_from_body.empty()) {
                    xobj_dict = std::move(dict_from_body);
                }
            }

            if (!xobj.is_stream || xobj.stream.empty()) {
                continue;
            }
            
            std::string subtype = parse_name_value_after_key(xobj_dict, "/Subtype");

            if (subtype == "Image") {
                WinImageXObject img;
                img.obj_id = xobj_id;
                img.width = parse_int_after(xobj_dict, "/Width", 0);
                img.height = parse_int_after(xobj_dict, "/Height", 0);
                img.bits_per_component = parse_int_after(xobj_dict, "/BitsPerComponent", 8);
                img.color_space = parse_name_value_after_key(xobj_dict, "/ColorSpace");
                std::string decode_str;
                if (extract_array_after_key(xobj_dict, "/Decode", decode_str)) {
                    std::stringstream ss(decode_str);
                    float val;
                    while (ss >> val) {
                        img.decode.push_back(val);
                    }
                }
                if (img.color_space.empty()) {
                    // Try to parse array ColorSpace like [/Indexed /DeviceRGB 255 <...>]
                    size_t cs_pos = xobj_dict.find("/ColorSpace");
                    if (cs_pos != std::string::npos) {
                        size_t start_val = cs_pos + 11;
                        while(start_val < xobj_dict.size() && std::isspace(static_cast<unsigned char>(xobj_dict[start_val]))) start_val++;
                        if (start_val < xobj_dict.size()) {
                            if (xobj_dict[start_val] == '[') {
                                size_t end_cs = xobj_dict.find(']', start_val);
                                if (end_cs != std::string::npos) {
                                    img.color_space = xobj_dict.substr(start_val, end_cs - start_val + 1);
                                }
                            } else {
                                // Maybe it's an indirect reference like "368 0 R"
                                int cs_ref = parse_ref_id_after_key(xobj_dict, "/ColorSpace");
                                if (cs_ref > 0) {
                                    WinPdfObject cs_obj = read_obj(cs_ref);
                                    std::string cs_content = cs_obj.body.empty() ? cs_obj.dict : cs_obj.body;
                                    if (!cs_content.empty()) {
                                        img.color_space = cs_content;
                                        // Overwrite the original /ColorSpace 368 0 R with /ColorSpace [ ... ] so the palette parser below can find it
                                        xobj_dict += "\n/ResolvedColorSpace " + cs_content;
                                    }
                                }
                            }
                        }
                    }
                }

                if (img.color_space.find("/Indexed") != std::string::npos || img.color_space.find("/I ") != std::string::npos || img.color_space.find("/I]") != std::string::npos) {
                    size_t cs_pos = xobj_dict.find("/ColorSpace");
                    if (cs_pos == std::string::npos || xobj_dict.find('[', cs_pos) == std::string::npos) {
                        cs_pos = xobj_dict.find("/ResolvedColorSpace");
                    }
                    if (cs_pos != std::string::npos) {
                        size_t bracket_pos = xobj_dict.find('[', cs_pos);
                        if (bracket_pos != std::string::npos) {
                            size_t end_bracket = xobj_dict.find(']', bracket_pos);
                            if (end_bracket != std::string::npos) {
                                std::string arr_content = xobj_dict.substr(bracket_pos + 1, end_bracket - bracket_pos - 1);
                                size_t hex_start = arr_content.find('<');
                                if (hex_start != std::string::npos) {
                                    size_t hex_end = arr_content.find('>', hex_start);
                                    if (hex_end != std::string::npos) {
                                        std::string hex_data = arr_content.substr(hex_start + 1, hex_end - hex_start - 1);
                                        std::vector<uint8_t> pal;
                                        auto hex_val = [](char c) -> int {
                                            if (c >= '0' && c <= '9') return c - '0';
                                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                                            return 0;
                                        };
                                        for (size_t i = 0; i < hex_data.size(); i++) {
                                            if (std::isspace(static_cast<unsigned char>(hex_data[i]))) continue;
                                            if (i + 1 < hex_data.size()) {
                                                char c1 = hex_data[i];
                                                // find next non-space
                                                size_t j = i + 1;
                                                while(j < hex_data.size() && std::isspace(static_cast<unsigned char>(hex_data[j]))) j++;
                                                if (j < hex_data.size()) {
                                                    char c2 = hex_data[j];
                                                    pal.push_back((uint8_t)((hex_val(c1) << 4) | hex_val(c2)));
                                                    i = j;
                                                }
                                            }
                                        }
                                        img.indexed_palette = pal;
                                    }
                                } else {
                                    size_t r_pos = arr_content.rfind(" R");
                                    if (r_pos != std::string::npos) {
                                        std::stringstream ss(arr_content);
                                        std::string token;
                                        std::vector<std::string> tokens;
                                        while (ss >> token) tokens.push_back(token);
                                        if (tokens.size() >= 3 && tokens.back() == "R") {
                                            try {
                                                int ref_id = std::stoi(tokens[tokens.size() - 3]);
                                                if (ref_id > 0) {
                                                    WinPdfObject pal_obj = read_obj(ref_id);
                                                    if (pal_obj.is_stream && !pal_obj.stream.empty()) {
                                                        std::vector<uint8_t> dec = decode_stream_data(pal_obj.stream, pal_obj.dict, this);
                                                        if (!dec.empty()) {
                                                            printf("DEBUG: Decoded palette stream size: %zu\n", dec.size());
                                                            img.indexed_palette = dec;
                                                        } else {
                                                            printf("DEBUG: Using raw palette stream size: %zu\n", pal_obj.stream.size());
                                                            img.indexed_palette = pal_obj.stream;
                                                        }
                                                    }
                                                }
                                            } catch(...) {}
                                        }
                                    } else {
                                        size_t str_start = arr_content.find('(');
                                        if (str_start != std::string::npos) {
                                            size_t str_end = arr_content.find(')', str_start);
                                            if (str_end != std::string::npos) {
                                                std::string str_data = arr_content.substr(str_start + 1, str_end - str_start - 1);
                                                img.indexed_palette.assign(str_data.begin(), str_data.end());
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                int smask_ref = parse_ref_id_after_key(xobj_dict, "/SMask");
                if (smask_ref > 0) {
                    WinPdfObject smask_obj = read_obj(smask_ref);
                    if (smask_obj.is_stream && !smask_obj.stream.empty()) {
                        std::vector<uint8_t> decoded_smask = decode_stream_data(smask_obj.stream, smask_obj.dict, this);
                        if (decoded_smask.empty()) {
                            decoded_smask = smask_obj.stream;
                        }
                        img.smask_ptr = std::make_shared<const std::vector<uint8_t>>(std::move(decoded_smask));
                    }
                }
                
                img.filter = parse_name_value_after_key(xobj_dict, "/Filter");
                if (img.filter.empty()) {
                    size_t f_pos = xobj_dict.find("/Filter");
                    if (f_pos != std::string::npos) {
                        f_pos += 7;
                        while(f_pos < xobj_dict.size() && std::isspace(static_cast<unsigned char>(xobj_dict[f_pos]))) f_pos++;
                        if (f_pos < xobj_dict.size() && xobj_dict[f_pos] == '[') {
                            size_t end_f = xobj_dict.find(']', f_pos);
                            if (end_f != std::string::npos) {
                                img.filter = xobj_dict.substr(f_pos, end_f - f_pos + 1);
                            }
                        }
                    }
                }
                
                std::shared_ptr<const std::vector<uint8_t>> decoded_ptr;
                if (cached_decoded_streams.count(xobj_id)) {
                    decoded_ptr = cached_decoded_streams[xobj_id];
                } else {
                    std::vector<uint8_t> decoded = decode_stream_data(xobj.stream, xobj_dict, this);
                    if (decoded.empty()) {
                        decoded = xobj.stream;
                    }
                    decoded_ptr = std::make_shared<const std::vector<uint8_t>>(std::move(decoded));
                    cached_decoded_streams[xobj_id] = decoded_ptr;
                }
                img.stream_ptr = decoded_ptr;
                images[it.first] = std::move(img);
            } else if (subtype == "Form") {
                std::string child_resources = resolve_resources_dict(xobj_dict, resources_dict);
                if (child_resources.empty() || child_resources == resources_dict) {
                    continue;
                }
                recursion_guard.insert(xobj_id);
                WinImageXObjectMap child_images = build_images(child_resources, recursion_guard);
                recursion_guard.erase(xobj_id);
                for (auto& child : child_images) {
                    images[it.first + "_" + child.first] = std::move(child.second);
                }
            }
        }
        return images;
    };

    std::set<int> recursion_guard;
    out = build_images(page_resources_dict, recursion_guard);
    cached_resource_image_xobject_maps[page_resources_dict] = out_ptr;
    cached_page_image_xobject_maps[page_idx] = out_ptr;
    return out_ptr;
}


WinFontWidthMap WinPdfDocument::get_page_font_width_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontWidthMap out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    WinFontUnicodeMap unicode_map = get_page_font_unicode_map(page_idx);

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);
        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) {
            extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        }
        if (!resources_dict.empty()) break;
        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }
    if (resources_dict.empty()) return out;

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) {
        extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    }
    if (font_dict.empty()) return out;

    auto parse_int_after = [&](const std::string& dict, const std::string& key) -> int {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) return -1;
        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) ++pos;
        if (pos >= dict.size()) return -1;
        return std::atoi(dict.c_str() + pos);
    };

    auto parse_float_after = [&](const std::string& dict, const std::string& key, double fallback) -> double {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) return fallback;
        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) ++pos;
        if (pos >= dict.size()) return fallback;
        char* end_ptr = nullptr;
        double v = wz_strtod(dict.c_str() + pos, &end_ptr);
        if (end_ptr == dict.c_str() + pos) {
            return fallback;
        }
        return v;
    };

    auto extract_balanced_array = [](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }
        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_type3_font_matrix_scale = [&](const WinPdfObject& font_obj) -> float {
        std::string matrix_array;
        if (!extract_array_after_key(font_obj.dict, "/FontMatrix", matrix_array)) {
            return 1.0f;
        }

        size_t arr_start = matrix_array.find('[');
        size_t arr_end = matrix_array.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return 1.0f;
        }

        const char* p = matrix_array.c_str() + arr_start + 1;
        const char* end = matrix_array.c_str() + arr_end;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (p >= end) {
            return 1.0f;
        }

        char* end_ptr = nullptr;
        double a = wz_strtod(p, &end_ptr);
        if (end_ptr == p || end_ptr > end || !std::isfinite(a)) {
            return 1.0f;
        }

        return static_cast<float>(a);
    };

    auto parse_cid_width_array = [](const std::string& array_text, std::unordered_map<int, float>& widths) {
        std::vector<std::string> tokens;
        for (size_t i = 0; i < array_text.size();) {
            if (std::isspace(static_cast<unsigned char>(array_text[i]))) {
                ++i;
                continue;
            }
            if (array_text[i] == '[' || array_text[i] == ']') {
                tokens.push_back(array_text.substr(i, 1));
                ++i;
                continue;
            }

            size_t start = i;
            if (array_text[i] == '+' || array_text[i] == '-') {
                ++i;
            }
            while (i < array_text.size() && (std::isdigit(static_cast<unsigned char>(array_text[i])) || array_text[i] == '.')) {
                ++i;
            }
            if (i > start) {
                tokens.push_back(array_text.substr(start, i - start));
            } else {
                ++i;
            }
        }

        size_t i = 0;
        while (i < tokens.size()) {
            if (tokens[i] == "[" || tokens[i] == "]") {
                ++i;
                continue;
            }

            int c0 = std::atoi(tokens[i].c_str());
            ++i;
            if (i >= tokens.size()) {
                break;
            }

            if (tokens[i] == "[") {
                ++i;
                int cid = c0;
                while (i < tokens.size() && tokens[i] != "]") {
                    if (tokens[i] != "[") {
                        widths[cid] = static_cast<float>(wz_strtod(tokens[i].c_str()) / 1000.0);
                        ++cid;
                    }
                    ++i;
                }
                if (i < tokens.size() && tokens[i] == "]") {
                    ++i;
                }
            } else {
                if (i + 1 >= tokens.size()) {
                    break;
                }
                int c1 = std::atoi(tokens[i].c_str());
                ++i;
                float w = static_cast<float>(wz_strtod(tokens[i].c_str()) / 1000.0);
                ++i;

                if (c1 < c0) {
                    std::swap(c0, c1);
                }
                for (int cid = c0; cid <= c1; ++cid) {
                    widths[cid] = w;
                }
            }
        }
    };

#ifdef WINEXTRACT_USE_FREETYPE
    struct CidToGidMapData {
        bool has_map = false;
        bool identity = false;
        std::vector<uint16_t> values;
    };

    auto resolve_type0_descendant_font = [&](const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj) -> bool {
        descendant_font_obj = {};
        if (parse_name_value_after_key(font_obj.dict, "/Subtype") != "Type0") {
            return false;
        }

        std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
        if (descendant_ids.empty()) {
            int single_descendant = parse_ref_id_after_key(font_obj.dict, "/DescendantFonts");
            if (single_descendant > 0) {
                descendant_ids.push_back(single_descendant);
            }
        }
        if (descendant_ids.empty() || descendant_ids.front() <= 0) {
            return false;
        }

        descendant_font_obj = read_obj(descendant_ids.front());
        return descendant_font_obj.id > 0 || !descendant_font_obj.dict.empty() || !descendant_font_obj.body.empty();
    };

    auto resolve_font_descriptor_dict = [&](const WinPdfObject& font_obj, std::string& descriptor_dict) -> bool {
        descriptor_dict.clear();

        auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
            return !descriptor_dict.empty();
        };

        if (read_descriptor_from_dict(font_obj.dict)) {
            return true;
        }

        WinPdfObject descendant_font_obj;
        if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return read_descriptor_from_dict(descendant_font_obj.dict);
        }

        return false;
    };

    auto load_cid_to_gid_map = [&](const WinPdfObject& font_obj) -> CidToGidMapData {
        CidToGidMapData cid_to_gid;

        WinPdfObject descendant_font_obj;
        if (!resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            return cid_to_gid;
        }

        const std::string direct_map_name = parse_name_value_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (direct_map_name == "Identity") {
            cid_to_gid.has_map = true;
            cid_to_gid.identity = true;
            return cid_to_gid;
        }

        const int map_ref = parse_ref_id_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
        if (map_ref <= 0) {
            if (direct_map_name.empty()) {
                cid_to_gid.has_map = true;
                cid_to_gid.identity = true;
            }
            return cid_to_gid;
        }

        WinPdfObject map_obj = read_obj(map_ref);
        if (!map_obj.is_stream || map_obj.stream.empty()) {
            return cid_to_gid;
        }

        std::vector<uint8_t> decoded = decode_stream_data(map_obj.stream, map_obj.dict, this);
        if (decoded.empty()) {
            decoded = map_obj.stream;
        }
        if (decoded.size() < 2) {
            return cid_to_gid;
        }

        cid_to_gid.values.reserve(decoded.size() / 2);
        for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
            const uint16_t gid = static_cast<uint16_t>((static_cast<uint16_t>(decoded[i]) << 8) | decoded[i + 1]);
            cid_to_gid.values.push_back(gid);
        }

        cid_to_gid.has_map = !cid_to_gid.values.empty();
        return cid_to_gid;
    };

    auto fill_missing_widths_from_freetype = [&](const WinPdfObject& font_obj,
                                                 const std::unordered_map<int, std::vector<int>>* code_to_unicode,
                                                 std::unordered_map<int, float>& widths) {
        FT_Library library = get_freetype_library();
        if (!library) return;

        const bool is_type0_subtype = parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0";
        const CidToGidMapData cid_to_gid = load_cid_to_gid_map(font_obj);

        std::string descriptor_dict;
        resolve_font_descriptor_dict(font_obj, descriptor_dict);
        std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
        if (base_font_name.empty() && is_type0_subtype) {
            WinPdfObject descendant_font_obj;
            if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
                base_font_name = parse_name_value_after_key(descendant_font_obj.dict, "/BaseFont");
            }
        }
        base_font_name = normalize_pdf_font_name(base_font_name);

        FT_Face face = nullptr;
        std::vector<uint8_t> font_bytes;
        
        bool is_system_font = false; 

        if (!descriptor_dict.empty()) {
            int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
            if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

            if (font_file_ref > 0) {
                WinPdfObject font_file_obj = read_obj(font_file_ref);
                if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                    font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                    if (font_bytes.empty()) font_bytes = font_file_obj.stream;
                    if (!font_bytes.empty()) {
                        if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(font_bytes.data()), static_cast<FT_Long>(font_bytes.size()), 0, &face) != 0) {
                            face = nullptr;
                        }
                    }
                }
            }
        }

        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    is_system_font = true;
                    break;
                }
            }
        }

        if (!face) return;

        if (is_system_font) {
            FT_Done_Face(face);
            return;
        }

        FT_Select_Charmap(face, FT_ENCODING_UNICODE);
        const float units_per_em = (face->units_per_EM > 0) ? static_cast<float>(face->units_per_EM) : 1000.0f;

        auto lookup_gid_for_code = [&](int code) -> FT_UInt {
            if (code < 0) return 0;
            if (is_type0_subtype) {
                if (cid_to_gid.has_map) {
                    if (cid_to_gid.identity) {
                        const FT_UInt gid = static_cast<FT_UInt>(code);
                        if (face->num_glyphs <= 0 || gid < static_cast<FT_UInt>(face->num_glyphs)) return gid;
                        return 0;
                    }
                    if (static_cast<size_t>(code) < cid_to_gid.values.size()) {
                        return static_cast<FT_UInt>(cid_to_gid.values[static_cast<size_t>(code)]);
                    }
                }
                return 0;
            }

            FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
            if (gid != 0) return gid;

            FT_CharMap saved_charmap = face->charmap;
            if (face->num_charmaps > 0 && face->charmaps != nullptr) {
                for (int ci = 0; ci < face->num_charmaps; ++ci) {
                    FT_CharMap cmap = face->charmaps[ci];
                    if (cmap == nullptr || cmap == face->charmap) continue;
                    if (FT_Set_Charmap(face, cmap) != 0) continue;
                    gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
                    if (gid != 0) break;
                }
            }
            if (saved_charmap != nullptr && face->charmap != saved_charmap) {
                FT_Set_Charmap(face, saved_charmap);
            }
            return gid;
        };

        if (code_to_unicode) {
            for (const auto& cu : *code_to_unicode) {
                const int code = cu.first;
                if (code < 0 || widths.find(code) != widths.end()) continue;
                if (cu.second.empty()) continue;

                FT_UInt glyph_index = 0;
                if (is_type0_subtype) glyph_index = lookup_gid_for_code(code);
                if (glyph_index == 0) glyph_index = FT_Get_Char_Index(face, static_cast<FT_ULong>(cu.second.front()));
                if (glyph_index == 0) continue;
                if (face->num_glyphs > 0 && glyph_index >= static_cast<FT_UInt>(face->num_glyphs)) continue;
                if (FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) continue;

                widths[code] = static_cast<float>(face->glyph->metrics.horiAdvance) / units_per_em;
            }
        }

        int max_code = 255;
        if (is_type0_subtype) {
            max_code = 65535;
            if (cid_to_gid.has_map && !cid_to_gid.identity && !cid_to_gid.values.empty()) {
                const size_t capped_size = (std::min)(cid_to_gid.values.size(), static_cast<size_t>(65536));
                if (capped_size > 0) max_code = static_cast<int>(capped_size - 1);
            }
        }

        for (int code = 0; code <= max_code; ++code) {
            if (widths.find(code) != widths.end()) continue;

            FT_UInt glyph_index = lookup_gid_for_code(code);
            if (code_to_unicode) {
                auto u = code_to_unicode->find(code);
                if (u != code_to_unicode->end() && !u->second.empty()) {
                    FT_UInt from_unicode = FT_Get_Char_Index(face, static_cast<FT_ULong>(u->second.front()));
                    if (glyph_index == 0 && from_unicode != 0) glyph_index = from_unicode;
                }
            }
            if (glyph_index == 0) continue;
            if (face->num_glyphs > 0 && glyph_index >= static_cast<FT_UInt>(face->num_glyphs)) continue;
            if (FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) continue;

            widths[code] = static_cast<float>(face->glyph->metrics.horiAdvance) / units_per_em;
        }

        FT_Done_Face(face);
    };
#endif

    auto parse_missing_width_from_descriptor = [&](const WinPdfObject& font_obj) -> float {
        std::string descriptor_dict;
        auto read_descriptor_from_dict = [&](const std::string& dict) {
            int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
            if (descriptor_ref > 0) {
                WinPdfObject descriptor_obj = read_obj(descriptor_ref);
                descriptor_dict = descriptor_obj.dict;
            }
            if (descriptor_dict.empty()) {
                extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
            }
        };

        read_descriptor_from_dict(font_obj.dict);

        if (descriptor_dict.empty() && parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                read_descriptor_from_dict(cid_font_obj.dict);
            }
        }
        if (descriptor_dict.empty()) {
            return -1.0f;
        }

        const double missing_width = parse_float_after(descriptor_dict, "/MissingWidth", -1.0);
        if (missing_width <= 0.0) {
            return -1.0f;
        }
        return static_cast<float>(missing_width / 1000.0);
    };

    std::map<std::string, int> font_refs = parse_font_refs_from_dict(font_dict);
    for (const auto& entry : font_refs) {
        int font_obj_id = entry.second;
        if (cached_width_maps.count(font_obj_id)) {
            out[entry.first] = cached_width_maps[font_obj_id];
            continue;
        }

        WinPdfObject font_obj = read_obj(font_obj_id);
        std::string subtype = parse_name_value_after_key(font_obj.dict, "/Subtype");
        const bool is_type3_subtype = is_type3_font_subtype(subtype);
        const float type3_width_scale = is_type3_subtype ? parse_type3_font_matrix_scale(font_obj) : 1.0f;
        std::string base_font = normalize_pdf_font_name(parse_name_value_after_key(font_obj.dict, "/BaseFont"));

        std::unordered_map<int, float> widths;

        const float missing_width = parse_missing_width_from_descriptor(font_obj);

        bool is_type0 = (subtype == "Type0") ||
                        (font_obj.dict.find("/Subtype /Type0") != std::string::npos) ||
                        (font_obj.dict.find("/Subtype/Type0") != std::string::npos) ||
                        (font_obj.dict.find("/Encoding /Identity-H") != std::string::npos) ||
                        (font_obj.dict.find("/Encoding/Identity-H") != std::string::npos);

        if (is_type0) {
            std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
            if (!descendant_ids.empty()) {
                WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
                double dw = parse_float_after(cid_font_obj.dict, "/DW", 1000.0);
                widths[-1] = static_cast<float>(dw / 1000.0);

                std::string w_array;
                if (extract_array_after_key(cid_font_obj.dict, "/W", w_array)) {
                    parse_cid_width_array(w_array, widths);
                }

                WinW2MetricsMap w2_metrics;
                std::string dw2_array;
                if (extract_array_after_key(cid_font_obj.dict, "/DW2", dw2_array)) {
                    // DW2 format: [ v_y w2_1y ]. We just ignore it for now and use default if not present in W2.
                }
                std::string w2_array;
                if (extract_array_after_key(cid_font_obj.dict, "/W2", w2_array)) {
                    parse_cid_w2_array(w2_array, w2_metrics);
                }
                if (!w2_metrics.empty()) {
                    cached_w2_maps[font_obj_id] = std::make_shared<const WinW2MetricsMap>(std::move(w2_metrics));
                }
            }

            if (widths.find(-1) == widths.end() && missing_width > 0.0f) {
                widths[-1] = missing_width;
            }

#ifdef WINEXTRACT_USE_FREETYPE
            const std::unordered_map<int, std::vector<int>>* code_to_unicode = nullptr;
            auto uit = unicode_map.find(entry.first);
            if (uit != unicode_map.end()) {
                code_to_unicode = uit->second.get();
            }
            fill_missing_widths_from_freetype(font_obj, code_to_unicode, widths);
#endif
        } else {
            int first_char = parse_int_after(font_obj.dict, "/FirstChar");
            int last_char  = parse_int_after(font_obj.dict, "/LastChar");

            if (is_type3_subtype && (first_char < 0 || last_char > 255 || first_char > last_char)) {
                first_char = 0;
                last_char = 0;
            }

            if (first_char >= 0 && last_char >= first_char) {
                std::string widths_array;
                if (extract_array_after_key(font_obj.dict, "/Widths", widths_array)) {
                    size_t arr_start = widths_array.find('[');
                    size_t arr_end   = widths_array.rfind(']');
                    if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
                        const char* p = widths_array.c_str() + arr_start + 1;
                        const char* end = widths_array.c_str() + arr_end;
                        int code = first_char;
                        while (code <= last_char) {
                            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                                ++p;
                            }
                            if (p >= end) {
                                break;
                            }

                            char* end_ptr = nullptr;
                            const double w = wz_strtod(p, &end_ptr);
                            if (end_ptr == p || end_ptr > end) {
                                break;
                            }
                            p = end_ptr;
                            if (is_type3_subtype) {
                                widths[code] = scale_type3_width(static_cast<float>(w), type3_width_scale);
                            } else {
                                const float parsed_width = static_cast<float>(w / 1000.0);
                                if (parsed_width > 0.0f) {
                                    widths[code] = parsed_width;
                                }
                            }
                            ++code;
                        }
                    }
                }
            }

            if (is_type3_subtype) {
                if (widths.find(-1) == widths.end()) {
                    widths[-1] = 0.0f;
                }
            } else if (widths.find(-1) == widths.end() && missing_width > 0.0f) {
                widths[-1] = missing_width;
            }

#ifdef WINEXTRACT_USE_FREETYPE
            if (!is_type3_subtype) {
                const std::unordered_map<int, std::vector<int>>* code_to_unicode = nullptr;
                auto uit = unicode_map.find(entry.first);
                if (uit != unicode_map.end()) {
                    code_to_unicode = uit->second.get();
                }
                fill_missing_widths_from_freetype(font_obj, code_to_unicode, widths);
            }
#endif

            if (!is_type3_subtype && !base_font.empty()) {
                const std::string lower_base = to_lower_ascii(base_font);
                if (lower_base.find("italic") != std::string::npos || lower_base.find("oblique") != std::string::npos) {
                    widths[-2] = 1.0f;
                }
            }

            if (!is_type3_subtype) {
                int fallback_first_char = std::max(0, parse_int_after(font_obj.dict, "/FirstChar"));
                int fallback_last_char  = parse_int_after(font_obj.dict, "/LastChar");
                if (fallback_last_char < fallback_first_char || fallback_last_char > 255) {
                    fallback_last_char = 255;
                }

                for (int c = fallback_first_char; c <= fallback_last_char; ++c) {
                    if (widths.find(c) == widths.end()) {
                        widths[c] = get_base14_width(base_font, c);
                    }
                }
            }
        }

        if (!widths.empty()) {
            auto def_it = widths.find(-1);
            if (def_it != widths.end() && def_it->second <= 0.0f && !is_type3_subtype) {
                widths.erase(def_it);
            }
        }

        if (!widths.empty()) {
            cached_width_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, float>>(std::move(widths));
            out[entry.first] = cached_width_maps[font_obj_id];
        } else {
            cached_width_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, float>>();
            out[entry.first] = cached_width_maps[font_obj_id];
        }
    }

    return out;
}

WinFontW2Map WinPdfDocument::get_page_font_w2_map(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    WinFontW2Map out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return out;
    }

    // Ensure caches are populated
    get_page_font_width_map(page_idx);

    std::map<std::string, int> font_refs = get_page_font_name_to_id(page_idx);
    for (const auto& entry : font_refs) {
        if (cached_w2_maps.count(entry.second) && cached_w2_maps[entry.second]) {
            out[entry.first] = cached_w2_maps[entry.second];
        }
    }
    return out;
}


WinPageGeometry WinPdfDocument::get_page_geometry(int page_idx) {
    WinPageGeometry geo;
    geo.mediabox = {0, 0, 595, 842};
    geo.cropbox = {0, 0, 595, 842};
    geo.rotate = 0;
    
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) {
        return geo;
    }

    auto extract_balanced_array = [&](const std::string& src, size_t arr_start, std::string& out_array) -> bool {
        if (arr_start == std::string::npos || arr_start >= src.size() || src[arr_start] != '[') {
            return false;
        }

        int depth = 0;
        for (size_t i = arr_start; i < src.size(); ++i) {
            if (src[i] == '[') {
                ++depth;
            } else if (src[i] == ']') {
                --depth;
                if (depth == 0) {
                    out_array = src.substr(arr_start, i - arr_start + 1);
                    return true;
                }
            }
        }
        return false;
    };

    auto extract_array_after_key = [&](const std::string& dict, const std::string& key, std::string& out_array) -> bool {
        size_t pos = dict.find(key);
        if (pos == std::string::npos) {
            return false;
        }

        pos += key.size();
        while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
            ++pos;
        }
        if (pos >= dict.size()) {
            return false;
        }

        if (dict[pos] == '[') {
            return extract_balanced_array(dict, pos, out_array);
        }

        int ref_id = 0;
        int ref_gen = 0;
        if (wz_parse_obj_ref(dict.c_str() + pos, ref_id, ref_gen) && ref_id > 0) {
            WinPdfObject arr_obj = read_obj(ref_id);
            std::string src = !arr_obj.body.empty() ? arr_obj.body : arr_obj.dict;
            size_t arr_start = src.find('[');
            if (arr_start != std::string::npos) {
                return extract_balanced_array(src, arr_start, out_array);
            }
        }

        return false;
    };

    auto parse_rect_from_array = [&](const std::string& array_text, Rect& out_rect) -> bool {
        size_t arr_start = array_text.find('[');
        size_t arr_end = array_text.rfind(']');
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
            return false;
        }

        const char* p = array_text.c_str() + arr_start + 1;
        const char* end = array_text.c_str() + arr_end;
        float v[4] = {0, 0, 595, 842};
        for (int k = 0; k < 4; ++k) {
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end) {
                return false;
            }

            char* end_ptr = nullptr;
            const double d = wz_strtod(p, &end_ptr);
            if (end_ptr == p || end_ptr > end) {
                return false;
            }
            p = end_ptr;
            v[k] = static_cast<float>(d);
        }

        out_rect = {v[0], v[1], v[2], v[3]};
        return true;
    };

    Rect crop_box = geo.cropbox;
    Rect media_box = geo.mediabox;
    int rotate = geo.rotate;
    
    bool has_crop = false;
    bool has_media = false;
    bool has_rotate = false;

    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        if (!has_crop) {
            std::string crop_array;
            if (extract_array_after_key(node.dict, "/CropBox", crop_array) ||
                extract_array_after_key(node.dict, "/cropbox", crop_array)) {
                has_crop = parse_rect_from_array(crop_array, crop_box);
            }
        }

        if (!has_media) {
            std::string media_array;
            if (extract_array_after_key(node.dict, "/MediaBox", media_array) ||
                extract_array_after_key(node.dict, "/mediabox", media_array)) {
                has_media = parse_rect_from_array(media_array, media_box);
            }
        }

        if (!has_rotate) {
            int rot = parse_int_after_key(node.dict, "/Rotate", -999);
            if (rot != -999) {
                rotate = rot;
                has_rotate = true;
            }
        }

        if (has_crop && has_media && has_rotate) {
            break;
        }

        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (has_media) {
        geo.mediabox = media_box;
    }
    
    if (has_crop) {
        geo.cropbox = crop_box;
    } else {
        geo.cropbox = geo.mediabox;
    }
    
    geo.rotate = rotate;
    return geo;
}

void WinPdfDocument::clear_page_cache() {
    // DO NOT CLEAR ANY CATCHES! 
    // object_cache.clear();
    // Do NOT clear objstm_infos, objstm_lookup, or objstm_index_ready.
    // They are structural document-level caches. Clearing them forces
    // O(N^2) zlib decompressions for all Object Streams on every page.
}

WinPdfObject WinPdfDocument::read_obj(int id) {
    {
        std::lock_guard<std::recursive_mutex> lock(cache_mutex);
        auto cached = object_cache.find(id);
        if (cached != object_cache.end()) {
            return cached->second;
        }
    }

    WinPdfObject obj;
    obj.id = id;

    auto it = xref.find(id);
    if (it != xref.end()) {
        obj = read_obj_from_offset(id, it->second);
        if (object_has_payload(obj)) {
            std::lock_guard<std::recursive_mutex> lock(cache_mutex);
            object_cache[id] = obj;
            return obj;
        }
    }

    obj = read_obj_from_objstm(id);
    if (object_has_payload(obj)) {
        std::lock_guard<std::recursive_mutex> lock(cache_mutex);
        object_cache[id] = obj;
    }
    return obj;
}

WinPdfObject WinPdfDocument::read_obj_from_offset(int id, size_t offset) {
    WinPdfObject obj;
    obj.id = id;

    const std::string_view fv = file_view_;
    const size_t fv_size = fv.size();
    const char* fv_data  = fv.data();

    if (offset >= fv_size) {
        return obj;
    }

    const std::string_view raw = fv;

    auto find_token = [&](const char* token, size_t start_pos, size_t end_pos) -> size_t {
        if (start_pos > end_pos || start_pos > fv_size) {
            return std::string_view::npos;
        }
        end_pos = std::min(end_pos, fv_size);
        size_t p = raw.find(token, start_pos);
        if (p == std::string_view::npos || p > end_pos) {
            return std::string_view::npos;
        }
        return p;
    };

    size_t header_scan_end = std::min(fv_size, offset + static_cast<size_t>(128));
    size_t header_obj_pos = find_token("obj", offset, header_scan_end);
    if (header_obj_pos == std::string_view::npos) {
        return obj;
    }

    std::string header(fv_data + offset, header_obj_pos + 3 - offset);
    wz_parse_obj_header(header.c_str(), obj.id, obj.gen);

    size_t body_start = header_obj_pos + 3;
    while (body_start < fv_size &&
           std::isspace(static_cast<unsigned char>(fv_data[body_start]))) {
        ++body_start;
    }

    size_t initial_endobj = fv_size;
    size_t endobj_pos = raw.find("endobj", body_start);
    if (endobj_pos != std::string_view::npos) {
        initial_endobj = endobj_pos;
    }

    size_t obj_end = initial_endobj;
    size_t stream_pos = find_token("stream", body_start, initial_endobj);
    if (stream_pos != std::string_view::npos) {
        const std::string_view pre_stream_sv = raw.substr(body_start, stream_pos - body_start);
        obj.dict = extract_first_dict_fragment(std::string(pre_stream_sv));

        size_t stream_abs_start = stream_pos + 6;
        if (stream_abs_start < fv_size && fv_data[stream_abs_start] == '\r') {
            ++stream_abs_start;
        }
        if (stream_abs_start < fv_size && fv_data[stream_abs_start] == '\n') {
            ++stream_abs_start;
        }

        size_t stream_abs_end = fv_size;
        int stream_len = -1;
        size_t len_pos = obj.dict.find("/Length");
        if (len_pos != std::string::npos) {
            const char* ptr = obj.dict.c_str() + len_pos + 7;
            while (*ptr && std::isspace(static_cast<unsigned char>(*ptr))) {
                ++ptr;
            }

            int ref_id = 0;
            int ref_gen = 0;
            if (wz_parse_obj_ref(ptr, ref_id, ref_gen) && ref_id > 0 && ref_id != id) {
                WinPdfObject len_obj = read_obj(ref_id);
                stream_len = parse_first_int(len_obj.body, -1);
            } else {
                char* end_ptr = nullptr;
                long v = wz_strtol(ptr, &end_ptr, 10);
                if (end_ptr != ptr) {
                    stream_len = static_cast<int>(v);
                }
            }
        }

        if (stream_len >= 0) {
            size_t candidate_end = stream_abs_start + static_cast<size_t>(stream_len);
            if (candidate_end <= fv_size) {
                stream_abs_end = candidate_end;
            }
        }

        if (stream_abs_end <= stream_abs_start || stream_abs_end > fv_size) {
            size_t endstream_pos = raw.find("endstream", stream_abs_start);
            if (endstream_pos != std::string_view::npos) {
                stream_abs_end = endstream_pos;
            }
        }

        if (stream_abs_start <= fv_size &&
            stream_abs_end > stream_abs_start &&
            stream_abs_end <= fv_size) {
            obj.is_stream = true;
            const size_t stream_size = stream_abs_end - stream_abs_start;
            obj.stream.resize(stream_size);
            if (stream_size > 0) {
                std::memcpy(obj.stream.data(), fv_data + stream_abs_start, stream_size);
            }

            size_t endobj_after_stream = raw.find("endobj", stream_abs_end);
            if (endobj_after_stream != std::string_view::npos) {
                obj_end = endobj_after_stream;
            }
        }
    }

    if (obj_end <= body_start || obj_end > fv_size) {
        obj_end = fv_size;
    }

    obj.body.assign(fv_data + body_start, obj_end - body_start);
    if (obj.dict.empty()) {
        obj.dict = extract_first_dict_fragment(obj.body);
    }

    return obj;
}

void WinPdfDocument::build_objstm_index() {
    objstm_index_ready = true;
}

void WinPdfDocument::load_objstm(int container_id) {
    std::lock_guard<std::recursive_mutex> lock(objstm_mutex);

    if (objstm_infos.find(container_id) != objstm_infos.end()) {
        return;
    }

    auto it = xref.find(container_id);
    if (it == xref.end()) {
        return;
    }

    WinPdfObject container = read_obj_from_offset(container_id, it->second);
    if (!container.is_stream || container.dict.empty()) {
        return;
    }

    std::vector<uint8_t> decoded = decode_stream_data(container.stream, container.dict, this);
    if (decoded.empty()) {
        decoded = container.stream;
    }
    if (decoded.empty()) {
        return;
    }

    const int n = parse_int_after_key(container.dict, "/N", -1);
    const int first = parse_int_after_key(container.dict, "/First", -1);
    if (n <= 0 || first < 0 || static_cast<size_t>(first) > decoded.size()) {
        return;
    }

    ObjStreamInfo info;
    info.first = first;
    info.decoded = std::move(decoded);

    std::string table(reinterpret_cast<const char*>(info.decoded.data()), static_cast<size_t>(first));
    const char* p = table.c_str();
    for (int i = 0; i < n; ++i) {
        char* end_ptr = nullptr;
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (!*p) {
            break;
        }

        const long obj_id_long = wz_strtol(p, &end_ptr, 10);
        if (end_ptr == p) {
            break;
        }
        p = end_ptr;

        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (!*p) {
            break;
        }

        const long rel_off_long = wz_strtol(p, &end_ptr, 10);
        if (end_ptr == p) {
            break;
        }
        p = end_ptr;

        const int obj_id = static_cast<int>(obj_id_long);
        const int rel_off = static_cast<int>(rel_off_long);
        if (obj_id > 0 && rel_off >= 0) {
            info.items.push_back({obj_id, rel_off});
            objstm_lookup[obj_id] = {container_id, static_cast<int>(info.items.size() - 1)};
        }
    }

    if (info.items.empty()) {
        return;
    }

    objstm_infos[container_id] = std::move(info);
}

WinPdfObject WinPdfDocument::read_obj_from_objstm(int id) {
    WinPdfObject obj;
    obj.id = id;

    auto hit = objstm_lookup.find(id);
    if (hit == objstm_lookup.end()) {
        return obj;
    }

    const int container_id = hit->second.container_id;
    if (objstm_infos.find(container_id) == objstm_infos.end()) {
        load_objstm(container_id);
    }

    auto info_it = objstm_infos.find(container_id);
    if (info_it == objstm_infos.end()) {
        return obj;
    }

    const ObjStreamInfo& info = info_it->second;
    int idx = hit->second.item_index;
    if (idx < 0 || idx >= static_cast<int>(info.items.size()) || info.items[idx].first != id) {
        idx = -1;
        for (int i = 0; i < static_cast<int>(info.items.size()); ++i) {
            if (info.items[i].first == id) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0 || idx >= static_cast<int>(info.items.size())) {
        return obj;
    }

    const int rel_off = info.items[idx].second;
    if (rel_off < 0) {
        return obj;
    }

    size_t start = static_cast<size_t>(info.first + rel_off);
    size_t end = info.decoded.size();

    // Find the closest following object start in byte order.
    for (const auto& item : info.items) {
        if (item.second <= rel_off) {
            continue;
        }
        const size_t candidate = static_cast<size_t>(info.first + item.second);
        if (candidate < end) {
            end = candidate;
        }
    }

    if (start >= end || end > info.decoded.size()) {
        return obj;
    }

    obj.body = trim_copy(std::string(reinterpret_cast<const char*>(info.decoded.data() + start), end - start));
    obj.dict = extract_first_dict_fragment(obj.body);
    return obj;
}

void WinPdfDocument::parse_xref() {
    xref.clear();
    object_cache.clear();
    objstm_infos.clear();
    objstm_lookup.clear();
    objstm_index_ready = false;

    root_id = 0;
    if (file_view_.empty()) {
        return;
    }

    const std::string_view raw = file_view_;

    auto skip_ws = [&](size_t& pos) {
        while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) {
            ++pos;
        }
    };

    auto read_int = [&](size_t& pos, long& out) -> bool {
        skip_ws(pos);
        if (pos >= raw.size()) {
            return false;
        }
        char* end_ptr = nullptr;
        long v = wz_strtol(raw.data() + pos, &end_ptr, 10);
        if (end_ptr == raw.data() + pos) {
            return false;
        }
        out = v;
        pos = static_cast<size_t>(end_ptr - raw.data());
        return true;
    };

    auto find_startxref = [&]() -> long {
        size_t pos = raw.rfind("startxref");
        while (pos != std::string::npos) {
            size_t p = pos + 9;
            skip_ws(p);

            char* end_ptr = nullptr;
            long v = wz_strtol(raw.data() + p, &end_ptr, 10);
            if (end_ptr != raw.data() + p && v >= 0) {
                return v;
            }

            if (pos == 0) {
                break;
            }
            pos = raw.rfind("startxref", pos - 1);
        }
        return -1;
    };

    std::set<size_t> visited;
    std::function<void(size_t)> parse_chain;
    parse_chain = [&](size_t offset) {
        if (offset >= raw.size() || visited.find(offset) != visited.end()) {
            return;
        }
        visited.insert(offset);

        size_t pos = offset;
        skip_ws(pos);

        const bool looks_like_xref_table =
            (pos + 4 <= raw.size() && raw.compare(pos, 4, "xref") == 0 &&
             (pos + 4 == raw.size() || std::isspace(static_cast<unsigned char>(raw[pos + 4]))));

        if (looks_like_xref_table) {
            pos += 4;

            while (pos < raw.size()) {
                skip_ws(pos);
                if (pos >= raw.size()) {
                    break;
                }
                if (pos + 7 <= raw.size() && raw.compare(pos, 7, "trailer") == 0) {
                    pos += 7;
                    break;
                }

                size_t hdr_pos = pos;
                long first_obj = 0;
                long obj_count = 0;
                if (!read_int(hdr_pos, first_obj) || !read_int(hdr_pos, obj_count) || obj_count < 0) {
                    break;
                }
                pos = hdr_pos;

                for (long i = 0; i < obj_count && pos < raw.size(); ++i) {
                    skip_ws(pos);
                    size_t line_end = pos;
                    while (line_end < raw.size() && raw[line_end] != '\n' && raw[line_end] != '\r') {
                        ++line_end;
                    }
                    if (line_end <= pos) {
                        while (pos < raw.size() && (raw[pos] == '\r' || raw[pos] == '\n')) {
                            ++pos;
                        }
                        continue;
                    }

                    const std::string line(raw.substr(pos, line_end - pos));
                    long obj_off = -1;
                    int obj_gen = 0;
                    char in_use = 'f';
                    if (wz_parse_xref_line(line.c_str(), obj_off, obj_gen, in_use)) {
                        const int obj_id = static_cast<int>(first_obj + i);
                        if (in_use == 'n' && obj_id > 0 && obj_off >= 0) {
                            if (xref.find(obj_id) == xref.end()) {
                                xref[obj_id] = static_cast<size_t>(obj_off);
                            }
                        }
                    }

                    pos = line_end;
                    while (pos < raw.size() && (raw[pos] == '\r' || raw[pos] == '\n')) {
                        ++pos;
                    }
                }
            }

            std::string trailer_dict = extract_first_dict_fragment(std::string(raw.substr(pos)));
            if (!trailer_dict.empty()) {
                if (trailer_dict.find("/Encrypt") != std::string::npos) {
                    encrypted_ = true;
                }
                if (root_id <= 0) {
                    int rid = parse_ref_id_after_key(trailer_dict, "/Root");
                    if (rid > 0) {
                        root_id = rid;
                    }
                }

                int xref_stm = parse_int_after_key(trailer_dict, "/XRefStm", -1);
                if (xref_stm >= 0) {
                    parse_chain(static_cast<size_t>(xref_stm));
                }

                int prev = parse_int_after_key(trailer_dict, "/Prev", -1);
                if (prev >= 0) {
                    parse_chain(static_cast<size_t>(prev));
                }
            }

            return;
        }

        WinPdfObject xref_obj = read_obj_from_offset(0, offset);
        if (!xref_obj.is_stream || xref_obj.dict.empty()) {
            return;
        }

        const std::string type_name = parse_name_value_after_key(xref_obj.dict, "/Type");
        if (xref_obj.dict.find("/Encrypt") != std::string::npos) {
            encrypted_ = true;
        }
        const bool is_xref_stream =
            (type_name == "XRef") ||
            (xref_obj.dict.find("/Type/XRef") != std::string::npos) ||
            (xref_obj.dict.find("/Type /XRef") != std::string::npos);
        if (!is_xref_stream) {
            return;
        }

        std::vector<uint8_t> decoded = decode_stream_data(xref_obj.stream, xref_obj.dict, nullptr);
        if (decoded.empty()) {
            decoded = xref_obj.stream;
        }

        std::vector<int> w = parse_int_array_after_key(xref_obj.dict, "/W");
        if (w.size() >= 3 && !decoded.empty()) {
            std::vector<int> index = parse_int_array_after_key(xref_obj.dict, "/Index");
            if (index.empty()) {
                int size_val = parse_int_after_key(xref_obj.dict, "/Size", -1);
                if (size_val > 0) {
                    index.push_back(0);
                    index.push_back(size_val);
                }
            }

            const int w0 = std::max(0, w[0]);
            const int w1 = std::max(0, w[1]);
            const int w2 = std::max(0, w[2]);

            auto read_field = [&](size_t& p, int len) -> uint64_t {
                uint64_t v = 0;
                for (int i = 0; i < len && p < decoded.size(); ++i) {
                    v = (v << 8) | static_cast<uint64_t>(decoded[p++]);
                }
                return v;
            };

            size_t p = 0;
            for (size_t seg = 0; seg + 1 < index.size(); seg += 2) {
                const int start_obj = index[seg];
                const int count = index[seg + 1];
                if (count <= 0) {
                    continue;
                }

                for (int i = 0; i < count; ++i) {
                    if (p >= decoded.size()) {
                        break;
                    }

                    uint64_t f0 = read_field(p, w0);
                    uint64_t f1 = read_field(p, w1);
                    uint64_t f2 = read_field(p, w2);

                    const int type = (w0 == 0) ? 1 : static_cast<int>(f0);
                    const int obj_id = start_obj + i;

                    if (type == 1 && obj_id > 0) {
                        if (xref.find(obj_id) == xref.end()) {
                            xref[obj_id] = static_cast<size_t>(f1);
                        }
                    } else if (type == 2 && obj_id > 0 &&
                               f1 <= static_cast<uint64_t>(std::numeric_limits<int>::max()) &&
                               f2 <= static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                        if (objstm_lookup.find(obj_id) == objstm_lookup.end()) {
                            objstm_lookup[obj_id] = {static_cast<int>(f1), static_cast<int>(f2)};
                        }
                    }
                }
            }
        }

        if (root_id <= 0) {
            int rid = parse_ref_id_after_key(xref_obj.dict, "/Root");
            if (rid > 0) {
                root_id = rid;
            }
        }

        int xref_stm = parse_int_after_key(xref_obj.dict, "/XRefStm", -1);
        if (xref_stm >= 0) {
            parse_chain(static_cast<size_t>(xref_stm));
        }

        int prev = parse_int_after_key(xref_obj.dict, "/Prev", -1);
        if (prev >= 0) {
            parse_chain(static_cast<size_t>(prev));
        }
    };

    const long sx = find_startxref();
    if (sx >= 0) {
        parse_chain(static_cast<size_t>(sx));
    }

    // Fallback for damaged PDFs: scan for object headers and fill only missing entries.
    if (xref.empty()) {
        const char* fv_data = file_view_.data();
        const size_t fv_size = file_view_.size();
        for (size_t i = 1; i + 3 < fv_size; ++i) {
            if (fv_data[i] != 'o' || fv_data[i + 1] != 'b' || fv_data[i + 2] != 'j') {
                continue;
            }
            if (!is_white(static_cast<uint8_t>(fv_data[i - 1]))) {
                continue;
            }

            size_t line_start = i;
            while (line_start > 0 &&
                   fv_data[line_start - 1] != '\n' &&
                   fv_data[line_start - 1] != '\r') {
                --line_start;
            }

            const size_t line_len = (i + 3) - line_start;
            std::string header(fv_data + line_start, line_len);
            int obj_id = 0;
            int obj_gen = 0;
            if (wz_parse_obj_header(header.c_str(), obj_id, obj_gen) && obj_id > 0) {
                if (xref.find(obj_id) == xref.end()) {
                    xref[obj_id] = line_start;
                }
            }
        }
    }
}

int WinPdfDocument::parse_ref_id_after_key(const std::string& dict, const std::string& key) {
    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return -1;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }

    int id = 0;
    int gen = 0;
    if (wz_parse_obj_ref(dict.c_str() + pos, id, gen) && id > 0) {
        return id;
    }
    return -1;
}

std::vector<int> WinPdfDocument::parse_ref_array_after_key(const std::string& dict, const std::string& key) {
    std::vector<int> ids;
    auto parse_refs_from_array_text = [&](const std::string& array_src, std::vector<int>& out_ids) {
        size_t arr_start = array_src.find('[');
        size_t arr_end = array_src.find(']', arr_start == std::string::npos ? 0 : arr_start);
        if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start + 1) {
            return;
        }

        std::string refs_text = array_src.substr(arr_start + 1, arr_end - arr_start - 1);
        const char* p = refs_text.c_str();
        while (*p) {
            int id = 0;
            int gen = 0;
            char r = 0;
            int consumed = 0;
            if (!wz_parse_xref_line_3(p, id, gen, r, consumed) || consumed <= 0) {
                break;
            }
            if (r == 'R' && id > 0) {
                out_ids.push_back(id);
            }
            p += consumed;
        }
    };

    size_t pos = dict.find(key);
    if (pos == std::string::npos) {
        return ids;
    }

    pos += key.size();
    while (pos < dict.size() && std::isspace(static_cast<unsigned char>(dict[pos]))) {
        ++pos;
    }

    if (pos < dict.size() && dict[pos] == '[') {
        parse_refs_from_array_text(dict.substr(pos), ids);
        return ids;
    }

    int single = parse_ref_id_after_key(dict, key);
    if (single > 0) {
        WinPdfObject arr_obj = read_obj(single);
        std::vector<int> nested_ids;
        if (!arr_obj.body.empty()) {
            parse_refs_from_array_text(arr_obj.body, nested_ids);
        }
        if (nested_ids.empty() && !arr_obj.dict.empty()) {
            parse_refs_from_array_text(arr_obj.dict, nested_ids);
        }

        if (!nested_ids.empty()) {
            ids.insert(ids.end(), nested_ids.begin(), nested_ids.end());
        } else {
            ids.push_back(single);
        }
    }
    return ids;
}

bool WinPdfDocument::looks_like_text_content_stream(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }

    const size_t sample_len = std::min<size_t>(bytes.size(), 4096);
    std::string s(reinterpret_cast<const char*>(bytes.data()), sample_len);

    const bool has_bt = (s.find("BT") != std::string::npos);
    const bool has_et = (s.find("ET") != std::string::npos);
    const bool has_text_op =
        (s.find("Tj") != std::string::npos) ||
        (s.find("TJ") != std::string::npos) ||
        (s.find("Tf") != std::string::npos) ||
        (s.find("Td") != std::string::npos) ||
        (s.find("Tm") != std::string::npos);

    return has_bt && has_et && has_text_op;
}

bool WinPdfDocument::is_page_object(const std::string& dict) {
    size_t type_pos = dict.find("/Type");
    if (type_pos == std::string::npos) {
        return false;
    }

    size_t page_pos = dict.find("/Page", type_pos);
    if (page_pos == std::string::npos) {
        return false;
    }
    if (page_pos + 5 < dict.size() && dict[page_pos + 5] == 's') {
        return false;
    }
    return true;
}

bool WinPdfDocument::has_flate_filter(const std::string& dict) {
    if (dict.find("/FlateDecode") != std::string::npos) {
        return true;
    }
    if (dict.find("/Filter /Fl") != std::string::npos) {
        return true;
    }
    if (dict.find("/Filter[/Fl") != std::string::npos) {
        return true;
    }
    return false;
}

bool WinPdfDocument::is_type0_font_dict(const std::string& dict) {
    return (parse_name_value_after_key(dict, "/Subtype") == "Type0") ||
           (dict.find("/Subtype /Type0") != std::string::npos) ||
           (dict.find("/Subtype/Type0") != std::string::npos) ||
           (dict.find("/Encoding /Identity-H") != std::string::npos) ||
           (dict.find("/Encoding/Identity-H") != std::string::npos);
}

bool WinPdfDocument::resolve_type0_descendant_font(const WinPdfObject& font_obj, WinPdfObject& descendant_font_obj) {
    descendant_font_obj = {};
    if (!is_type0_font_dict(font_obj.dict)) return false;

    std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
    if (descendant_ids.empty()) {
        int single_descendant = parse_ref_id_after_key(font_obj.dict, "/DescendantFonts");
        if (single_descendant > 0) descendant_ids.push_back(single_descendant);
    }
    if (descendant_ids.empty() || descendant_ids.front() <= 0) return false;

    descendant_font_obj = read_obj(descendant_ids.front());
    return descendant_font_obj.id > 0 || !descendant_font_obj.dict.empty() || !descendant_font_obj.body.empty();
}

bool WinPdfDocument::resolve_font_descriptor_dict(const WinPdfObject& font_obj, std::string& descriptor_dict) {
    descriptor_dict.clear();

    auto read_descriptor_from_dict = [&](const std::string& dict) -> bool {
        int descriptor_ref = parse_ref_id_after_key(dict, "/FontDescriptor");
        if (descriptor_ref > 0) {
            WinPdfObject descriptor_obj = read_obj(descriptor_ref);
            descriptor_dict = descriptor_obj.dict;
        }
        if (descriptor_dict.empty()) extract_inline_dict_after_key(dict, "/FontDescriptor", descriptor_dict);
        return !descriptor_dict.empty();
    };

    if (read_descriptor_from_dict(font_obj.dict)) return true;

    WinPdfObject descendant_font_obj;
    if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
        return read_descriptor_from_dict(descendant_font_obj.dict);
    }
    return false;
}

WinPdfDocument::CidToGidMapData WinPdfDocument::load_cid_to_gid_map(const WinPdfObject& font_obj) {
    CidToGidMapData cid_to_gid;

    WinPdfObject descendant_font_obj;
    if (!resolve_type0_descendant_font(font_obj, descendant_font_obj)) return cid_to_gid;

    std::string cid_to_gid_map_val = parse_name_value_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
    if (cid_to_gid_map_val == "Identity") {
        cid_to_gid.has_map = true;
        cid_to_gid.identity = true;
        return cid_to_gid;
    }

    int cid_to_gid_map_ref = parse_ref_id_after_key(descendant_font_obj.dict, "/CIDToGIDMap");
    if (cid_to_gid_map_ref > 0) {
        WinPdfObject map_obj = read_obj(cid_to_gid_map_ref);
        if (map_obj.is_stream && !map_obj.stream.empty()) {
            std::vector<uint8_t> decoded_map = decode_stream_data(map_obj.stream, map_obj.dict, this);
            if (decoded_map.empty()) decoded_map = map_obj.stream;

            if (!decoded_map.empty() && decoded_map.size() % 2 == 0) {
                cid_to_gid.has_map = true;
                cid_to_gid.identity = false;
                size_t num_entries = decoded_map.size() / 2;
                cid_to_gid.values.resize(num_entries);
                for (size_t i = 0; i < num_entries; ++i) {
                    uint16_t gid = (static_cast<uint16_t>(decoded_map[2 * i]) << 8) | static_cast<uint16_t>(decoded_map[2 * i + 1]);
                    cid_to_gid.values[i] = gid;
                }
            }
        }
    }
    return cid_to_gid;
}

void WinPdfDocument::fill_missing_unicode_from_freetype(const WinPdfObject& font_obj,
                                                        std::unordered_map<int, std::vector<int>>& unicode_map,
                                                        const std::map<int, std::string>& diff_names) {
#ifdef WINEXTRACT_USE_FREETYPE
    FT_Library library = get_freetype_library();
    if (!library) return;

    bool is_type0_subtype = (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") ||
                            (font_obj.dict.find("/Subtype /Type0") != std::string::npos) ||
                            (font_obj.dict.find("/Subtype/Type0") != std::string::npos) ||
                            (font_obj.dict.find("/Encoding /Identity-H") != std::string::npos) ||
                            (font_obj.dict.find("/Encoding/Identity-H") != std::string::npos);
    const CidToGidMapData cid_to_gid = load_cid_to_gid_map(font_obj);

    std::string descriptor_dict;
    resolve_font_descriptor_dict(font_obj, descriptor_dict);

    std::string base_font_name = parse_name_value_after_key(font_obj.dict, "/BaseFont");
    if (base_font_name.empty() && is_type0_subtype) {
        WinPdfObject descendant_font_obj;
        if (resolve_type0_descendant_font(font_obj, descendant_font_obj)) {
            base_font_name = parse_name_value_after_key(descendant_font_obj.dict, "/BaseFont");
        }
    }
    base_font_name = normalize_pdf_font_name(base_font_name);

    FT_Face face = nullptr;
    std::vector<uint8_t> font_bytes;

    if (!descriptor_dict.empty()) {
        int font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile2");
        if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile");
        if (font_file_ref <= 0) font_file_ref = parse_ref_id_after_key(descriptor_dict, "/FontFile3");

        if (font_file_ref > 0) {
            WinPdfObject font_file_obj = read_obj(font_file_ref);
            if (font_file_obj.is_stream && !font_file_obj.stream.empty()) {
                font_bytes = decode_stream_data(font_file_obj.stream, font_file_obj.dict, this);
                if (font_bytes.empty()) font_bytes = font_file_obj.stream;
            }
        }
    }

    uint64_t font_hash = fnv1a_hash_bytes(font_bytes);
    if (!base_font_name.empty()) font_hash ^= fnv1a_hash_string(base_font_name);
    std::shared_ptr<CachedFreetypeData> cached_data;

    {
        std::shared_lock<std::shared_mutex> lock(g_global_freetype_cache_mutex);
        auto it = g_global_freetype_cache.find(font_hash);
        if (it != g_global_freetype_cache.end()) cached_data = it->second;
    }

    std::shared_ptr<std::unordered_map<unsigned int, int>> cached_gid_map;
    std::shared_ptr<std::unordered_map<std::string, unsigned int>> cached_name_map;

    bool is_system_font = false;
    if (cached_data) {
        cached_gid_map = cached_data->gid_to_unicode;
        cached_name_map = cached_data->name_to_gid;
        is_system_font = cached_data->is_system_font;
    } else {
        if (!font_bytes.empty()) {
            FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(font_bytes.data()), static_cast<FT_Long>(font_bytes.size()), 0, &face);
        }
        if (!face) {
            std::vector<std::string> system_candidates = get_system_font_candidates(base_font_name);
            for (const std::string& candidate : system_candidates) {
                if (FT_New_Face(library, candidate.c_str(), 0, &face) == 0) {
                    is_system_font = true;
                    break;
                }
            }
        }
        if (!face) return;

        auto gid_to_unicode = std::make_shared<std::unordered_map<unsigned int, int>>();
        auto name_to_gid = std::make_shared<std::unordered_map<std::string, unsigned int>>();

        auto collect_gid_unicode = [&]() {
            if (face->charmap == nullptr) return;
            FT_UInt gid = 0;
            FT_ULong charcode = FT_Get_First_Char(face, &gid);
            while (gid != 0) {
                if (charcode > 0 && charcode <= 0x10FFFFUL && gid_to_unicode->find(gid) == gid_to_unicode->end()) {
                    (*gid_to_unicode)[gid] = static_cast<int>(charcode);
                }
                charcode = FT_Get_Next_Char(face, charcode, &gid);
            }
            if (FT_HAS_GLYPH_NAMES(face)) {
                for (FT_UInt g = 0; g < (FT_UInt)face->num_glyphs; ++g) {
                    char gname[64];
                    if (FT_Get_Glyph_Name(face, g, gname, sizeof(gname)) == 0) {
                        std::string gname_str(gname);
                        (*name_to_gid)[gname_str] = g;
                        if (gid_to_unicode->find(g) == gid_to_unicode->end()) {
                            int cp = glyph_name_to_unicode(gname_str);
                            if (cp > 0) (*gid_to_unicode)[g] = cp;
                        }
                    }
                }
            }
        };

        FT_CharMap saved_charmap = face->charmap;
        if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) collect_gid_unicode();
        if (face->num_charmaps > 0 && face->charmaps != nullptr) {
            for (int ci = 0; ci < face->num_charmaps; ++ci) {
                FT_CharMap cmap = face->charmaps[ci];
                if (cmap == nullptr || cmap == face->charmap) continue;
                if (FT_Set_Charmap(face, cmap) == 0) collect_gid_unicode();
            }
        }
        if (saved_charmap != nullptr && face->charmap != saved_charmap) {
            FT_Set_Charmap(face, saved_charmap);
        }

        cached_gid_map = gid_to_unicode;
        cached_name_map = name_to_gid;
        cached_data = std::make_shared<CachedFreetypeData>();
        cached_data->gid_to_unicode = cached_gid_map;
        cached_data->name_to_gid = cached_name_map;
        cached_data->is_system_font = is_system_font;
        {
            std::unique_lock<std::shared_mutex> lock(g_global_freetype_cache_mutex);
            g_global_freetype_cache[font_hash] = cached_data;
        }
    }

    auto lookup_gid_for_code = [&](int code) -> FT_UInt {
        if (code < 0) return 0;
        if (is_type0_subtype) {
            if (cid_to_gid.has_map) {
                if (cid_to_gid.identity) return static_cast<FT_UInt>(code);
                if (static_cast<size_t>(code) < cid_to_gid.values.size()) {
                    return static_cast<FT_UInt>(cid_to_gid.values[static_cast<size_t>(code)]);
                }
            }
            return 0;
        }
        FT_UInt gid = 0;
        auto diff_it = diff_names.find(code);
        if (diff_it != diff_names.end() && cached_name_map) {
            auto it = cached_name_map->find(diff_it->second);
            if (it != cached_name_map->end()) gid = it->second;
        }
        if (gid == 0 && cached_name_map) {
            char char_name[32];
            snprintf(char_name, sizeof(char_name), "char%02X", code);
            auto it = cached_name_map->find(char_name);
            if (it != cached_name_map->end()) gid = it->second;
        }
        return gid;
    };

    std::vector<int> codes_to_process;
    if (is_type0_subtype) {
        if (cid_to_gid.has_map) {
            int max_code = cid_to_gid.identity ? 65535 : static_cast<int>(cid_to_gid.values.size());
            for (int c = 0; c < max_code; ++c) codes_to_process.push_back(c);
        }
    } else {
        for (int c = 0; c < 256; ++c) codes_to_process.push_back(c);
    }

    for (int code : codes_to_process) {
        if (unicode_map.find(code) != unicode_map.end() && !unicode_map[code].empty() && unicode_map[code][0] != 0xFFFD) {
            continue; // Already has a valid mapping
        }

        int cp = -1;
        if (is_type0_subtype) {
            if (!is_system_font) {
                FT_UInt gid = lookup_gid_for_code(code);
                if (gid > 0) {
                    auto git = cached_gid_map->find(gid);
                    if (git != cached_gid_map->end()) cp = git->second;
                }
            }
        } else {
            auto diff_it = diff_names.find(code);
            if (diff_it != diff_names.end()) cp = glyph_name_to_unicode(diff_it->second);
            if (cp <= 0 && face) {
                FT_UInt gid = lookup_gid_for_code(code);
                if (gid > 0) {
                    auto git = cached_gid_map->find(gid);
                    if (git != cached_gid_map->end()) cp = git->second;
                }
            }
        }
        if (cp > 0 && cp <= 0x10FFFF) unicode_map[code] = {cp};
    }
    if (face) FT_Done_Face(face);
#endif
}

std::map<std::string, int> WinPdfDocument::get_page_font_name_to_id(int page_idx) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    std::map<std::string, int> out;
    if (page_idx < 0 || page_idx >= static_cast<int>(page_ids.size()) || page_ids.empty()) return out;

    std::string resources_dict;
    int node_id = page_ids[page_idx];
    std::unordered_set<int> _visited_nodes;
    while (node_id > 0 && resources_dict.empty()) {
        if (!_visited_nodes.insert(node_id).second) break;
        WinPdfObject node = read_obj(node_id);

        int resources_ref = parse_ref_id_after_key(node.dict, "/Resources");
        if (resources_ref > 0) {
            WinPdfObject resources_obj = read_obj(resources_ref);
            resources_dict = resources_obj.dict;
        }
        if (resources_dict.empty()) extract_inline_dict_after_key(node.dict, "/Resources", resources_dict);
        if (!resources_dict.empty()) break;
        node_id = parse_ref_id_after_key(node.dict, "/Parent");
    }

    if (resources_dict.empty()) return out;

    std::string font_dict;
    int font_ref = parse_ref_id_after_key(resources_dict, "/Font");
    if (font_ref > 0) {
        WinPdfObject font_obj = read_obj(font_ref);
        font_dict = font_obj.dict;
    }
    if (font_dict.empty()) extract_inline_dict_after_key(resources_dict, "/Font", font_dict);
    if (font_dict.empty()) return out;

    return parse_font_refs_from_dict(font_dict);
}

std::shared_ptr<const std::unordered_map<int, WinUnicodeSequence>> WinPdfDocument::get_font_unicode_map_by_id(int font_obj_id) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    auto it = cached_unicode_maps.find(font_obj_id);
    if (it != cached_unicode_maps.end()) return it->second;
    return nullptr;
}

bool WinPdfDocument::patch_font_unicode_map_lazily(int font_obj_id) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex);
    auto it = cached_unicode_maps.find(font_obj_id);
    if (it == cached_unicode_maps.end()) return false;

    std::unordered_map<int, std::vector<int>> cmap = *(it->second);
    WinPdfObject font_obj = read_obj(font_obj_id);
    
    std::string encoding_dict;
    std::string encoding_name = parse_name_value_after_key(font_obj.dict, "/Encoding");
    if (encoding_name.empty()) {
        int encoding_ref = parse_ref_id_after_key(font_obj.dict, "/Encoding");
        if (encoding_ref > 0) {
            WinPdfObject enc_obj = read_obj(encoding_ref);
            encoding_dict = enc_obj.dict;
            encoding_name = parse_name_value_after_key(enc_obj.dict, "/BaseEncoding");
            if (encoding_name.empty()) encoding_name = parse_name_value_after_key(enc_obj.body, "/BaseEncoding");
        }
    }
    if (encoding_dict.empty()) {
        extract_inline_dict_after_key(font_obj.dict, "/Encoding", encoding_dict);
        if (!encoding_dict.empty() && encoding_name.empty()) {
            encoding_name = parse_name_value_after_key(encoding_dict, "/BaseEncoding");
        }
    }
    std::map<int, std::string> diff_names;
    if (!encoding_dict.empty()) {
        std::map<int, int> dummy;
        apply_differences_to_map(encoding_dict, dummy, &diff_names);
    }

    
    // 1. Type0 font safety identification
    bool is_type0_subtype = (parse_name_value_after_key(font_obj.dict, "/Subtype") == "Type0") ||
                            (font_obj.dict.find("/Subtype /Type0") != std::string::npos) ||
                            (font_obj.dict.find("/Subtype/Type0") != std::string::npos);

    std::string base_font = parse_name_value_after_key(font_obj.dict, "/BaseFont");
    
    // 2. If it is Type0 and lacks a name, must access DescendantFonts to extract the name.
    if (base_font.empty() && is_type0_subtype) {
        std::vector<int> descendant_ids = parse_ref_array_after_key(font_obj.dict, "/DescendantFonts");
        if (!descendant_ids.empty()) {
            WinPdfObject cid_font_obj = read_obj(descendant_ids.front());
            base_font = parse_name_value_after_key(cid_font_obj.dict, "/BaseFont");
        }
    }

    // 3. Use `normalize_pdf_font_name` to trim the excess part (e.g., ABCDEF+CMMI10 -> CMMI10).
    std::string base_font_lower = to_lower_ascii(normalize_pdf_font_name(base_font));


    const std::unordered_map<int, int>* fallback_mapping = nullptr;
    if (base_font_lower.find("cmmi") != std::string::npos) {
        fallback_mapping = &get_oml_mapping();
    } else if (base_font_lower.find("cmsy") != std::string::npos) {
        fallback_mapping = &get_oms_mapping();
    } else if (base_font_lower.find("symbol") != std::string::npos) {
        fallback_mapping = &get_symbol_mapping();
    } else if (base_font_lower.find("zapfdingbats") != std::string::npos || base_font_lower.find("dingbats") != std::string::npos) {
        fallback_mapping = &get_zapf_dingbats_mapping();
    }

    if (fallback_mapping) {
        for (int i = 0; i <= 255; ++i) {
            auto it_fallback = fallback_mapping->find(i);
            if (it_fallback != fallback_mapping->end()) {
                cmap[i] = {it_fallback->second};
            }
        }
    }

    bool is_utf16_encoding = (encoding_name.find("UTF16") != std::string::npos || encoding_name.find("UCS2") != std::string::npos);
    if (!is_utf16_encoding) {
        // fill_missing_unicode_from_freetype(font_obj, cmap, diff_names);
    }

    cached_unicode_maps[font_obj_id] = std::make_shared<const std::unordered_map<int, WinUnicodeSequence>>(std::move(cmap));
    return true;
}

} // namespace WinExtract
