#pragma once
#include <vector>
#include <cstdint>

namespace WinExtract {

// 1:1 CLONE MINIMALIST INFLATE (For FlateDecode)
// Pure C++ decoder for PDF content streams
class WinInflate {
public:
    static std::vector<uint8_t> decompress(const std::vector<uint8_t>& src);
};

} // namespace WinExtract
