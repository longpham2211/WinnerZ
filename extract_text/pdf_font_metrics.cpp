#include "pdf_font_metrics.hpp"
#include <cctype>
#include <cstdlib>

namespace WinExtract {

void parse_cid_w2_array(const std::string& array_text, WinW2MetricsMap& w2_metrics) {
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
        if (i + 1 < tokens.size() && tokens[i + 1] == "[") {
            // Format 1: c [ w1_1y v1_1x v1_1y w1_2y v1_2x v1_2y ... ]
            int cid = std::atoi(tokens[i].c_str());
            i += 2;
            while (i + 2 < tokens.size() && tokens[i] != "]") {
                WinW2Metrics m;
                m.w2_y = static_cast<float>(std::atof(tokens[i].c_str()) / 1000.0);
                m.v_x = static_cast<float>(std::atof(tokens[i + 1].c_str()) / 1000.0);
                m.v_y = static_cast<float>(std::atof(tokens[i + 2].c_str()) / 1000.0);
                w2_metrics[cid] = m;
                ++cid;
                i += 3;
            }
            if (i < tokens.size() && tokens[i] == "]") {
                ++i;
            }
        } else if (i + 4 < tokens.size()) {
            // Format 2: c_first c_last w2y vx vy
            int first_cid = std::atoi(tokens[i].c_str());
            int last_cid = std::atoi(tokens[i + 1].c_str());
            WinW2Metrics m;
            m.w2_y = static_cast<float>(std::atof(tokens[i + 2].c_str()) / 1000.0);
            m.v_x = static_cast<float>(std::atof(tokens[i + 3].c_str()) / 1000.0);
            m.v_y = static_cast<float>(std::atof(tokens[i + 4].c_str()) / 1000.0);
            for (int cid = first_cid; cid <= last_cid; ++cid) {
                w2_metrics[cid] = m;
            }
            i += 5;
        } else {
            ++i;
        }
    }
}

} // namespace WinExtract
