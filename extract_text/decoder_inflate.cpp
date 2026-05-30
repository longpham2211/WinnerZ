#include "decoder_inflate.hpp"
#include <vector>
#include <algorithm>
#ifdef __cplusplus
extern "C" {
#endif
#include "puff.h"
#ifdef __cplusplus
}
#endif

namespace WinExtract {

namespace {

static bool has_zlib_header(const std::vector<uint8_t>& src) {
    if (src.size() < 2) {
        return false;
    }
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0F) != 8) {
        return false;
    }
    if ((((int)cmf << 8) + flg) % 31 != 0) {
        return false;
    }
    if ((flg & 0x20) != 0) {
        return false;
    }
    return true;
}

static std::vector<uint8_t> try_puff(const std::vector<uint8_t>& src, size_t offset) {
    if (offset >= src.size()) {
        return {};
    }

    unsigned long in_len = static_cast<unsigned long>(src.size() - offset);
    unsigned long out_cap = static_cast<unsigned long>(std::max<size_t>(1024, src.size() * 4));
    for (int attempt = 0; attempt < 9; ++attempt) {
        std::vector<uint8_t> out(out_cap);
        unsigned long out_len = out_cap;
        unsigned long used_in = in_len;

        const int res = puff(out.data(), &out_len, src.data() + offset, &used_in);
        if (res == 0) {
            out.resize(out_len);
            return out;
        }
        if (res != 1) {
            break;
        }

        if (out_cap > static_cast<unsigned long>(128 * 1024 * 1024)) {
            break;
        }
        out_cap *= 2;
    }

    return {};
}

} // namespace

std::vector<uint8_t> WinInflate::decompress(const std::vector<uint8_t>& src) {
    if (src.empty()) {
        return {};
    }

    if (has_zlib_header(src)) {
        std::vector<uint8_t> z = try_puff(src, 2);
        if (!z.empty()) {
            return z;
        }
    }

    std::vector<uint8_t> raw = try_puff(src, 0);
    if (!raw.empty()) {
        return raw;
    }

    // Fallback: giữ nguyên stream thô nếu không giải nén được.
    return src;
}

} // namespace WinExtract
