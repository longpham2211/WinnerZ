#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace WinExtract {

struct WinW2Metrics {
    float w2_y = -1.0f;
    float v_x = 0.5f;
    float v_y = 0.88f;
};

using WinW2MetricsMap = std::unordered_map<int, WinW2Metrics>;

// Parses the PDF /W2 array of a CIDFont dict
void parse_cid_w2_array(const std::string& array_text, WinW2MetricsMap& w2_metrics);

} // namespace WinExtract
