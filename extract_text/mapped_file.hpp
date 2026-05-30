// =============================================================================
// mapped_file.hpp  —  WinnerZ MappedFile: Zero-copy PDF I/O
// =============================================================================
//
// Thay thế std::string / std::vector<uint8_t> read_all() bằng mmap.
//
// Lợi ích:
//   • File 1 GB = RAM vật lý chỉ dùng đúng lượng page OS đang đọc.
//   • Trả về std::string_view → zero-copy tuyệt đối khi truyền sang parser.
//   • Không bao giờ throw std::bad_alloc dù file có vài GB.
//   • Tự động tương thích Windows (MapViewOfFile) và Linux/macOS (mmap).
//
// Cách dùng:
//   WinExtract::MappedFile mf("huge.pdf");
//   if (!mf.ok()) { /* handle error */ }
//   std::string_view view = mf.view();      // zero-copy, dùng như string
//   const uint8_t*   ptr  = mf.data();      // raw bytes nếu cần
//   size_t           sz   = mf.size();
//
// MappedFile là move-only (không copy được) — giống unique_ptr.
// =============================================================================

#pragma once
#ifndef WINNERZ_MAPPED_FILE_HPP
#define WINNERZ_MAPPED_FILE_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <cstring>
#include <cerrno>

// ─── Platform detection ──────────────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
#   define WINNERZ_PLATFORM_WINDOWS 1
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   define WINNERZ_PLATFORM_POSIX 1
#   include <sys/mman.h>
#   include <sys/stat.h>
#   include <fcntl.h>
#   include <unistd.h>
#endif
// ─────────────────────────────────────────────────────────────────────────────

namespace WinExtract {

// =============================================================================
// class MappedFile
// =============================================================================
class MappedFile {
public:
    MappedFile() = default;

    // ── Constructor: mở và map file ──────────────────────────────────────────
    explicit MappedFile(const std::string& path) noexcept
        : path_(path)
    {
        open_and_map(path.c_str());
    }

    explicit MappedFile(const char* path) noexcept
        : path_(path ? path : "")
    {
        if (path) open_and_map(path);
    }

    // ── Destructor: tự động unmap ─────────────────────────────────────────────
    ~MappedFile() noexcept { unmap(); }

    // ── Move-only (không được copy) ───────────────────────────────────────────
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& o) noexcept { move_from(o); }
    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) { unmap(); move_from(o); }
        return *this;
    }

    // ── Truy vấn ─────────────────────────────────────────────────────────────

    /// Trả về true nếu file đã được map thành công.
    bool ok()   const noexcept { return ptr_ != nullptr && size_ > 0; }

    /// Kích thước file tính bằng byte.
    size_t size() const noexcept { return size_; }

    /// Con trỏ raw tới đầu dữ liệu (read-only).
    const uint8_t* data() const noexcept {
        return reinterpret_cast<const uint8_t*>(ptr_);
    }

    /// Zero-copy view — dùng thay thế cho std::string khi parse.
    std::string_view view() const noexcept {
        return ptr_ ? std::string_view(static_cast<const char*>(ptr_), size_)
                    : std::string_view{};
    }

    /// Truy cập theo byte index (không kiểm tra bounds — giống raw array).
    uint8_t operator[](size_t idx) const noexcept {
        return reinterpret_cast<const uint8_t*>(ptr_)[idx];
    }

    /// Path đã mở.
    const std::string& path() const noexcept { return path_; }

    /// Message lỗi nếu ok() == false.
    const std::string& error() const noexcept { return error_; }

    // ── Tạo std::vector<uint8_t> nếu code cũ bắt buộc phải dùng ─────────────
    // CHỈ dùng khi thật sự không tránh được — sẽ copy toàn bộ file vào RAM.
    // Phần lớn trường hợp hãy dùng view() hoặc data() thay vào.
    [[nodiscard]]
    std::vector<uint8_t> to_vector() const {
        if (!ok()) return {};
        const uint8_t* p = data();
        return std::vector<uint8_t>(p, p + size_);
    }

private:
    const void* ptr_  = nullptr;
    size_t      size_ = 0;
    std::string path_;
    std::string error_;

#if defined(WINNERZ_PLATFORM_WINDOWS)
    // ── Windows handles ───────────────────────────────────────────────────────
    HANDLE hFile_    = INVALID_HANDLE_VALUE;
    HANDLE hMapping_ = nullptr;

    static std::wstring utf8_to_utf16(const char* utf8) {
        if (!utf8 || utf8[0] == '\0') return std::wstring();
        int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (size == 0) return std::wstring();
        std::wstring utf16(size, 0);
        ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &utf16[0], size);
        utf16.resize(size - 1);
        return utf16;
    }

    void open_and_map(const char* path) noexcept {
        std::wstring wpath = utf8_to_utf16(path);
        // Mở file (read-only, share read)
        hFile_ = ::CreateFileW(
            wpath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);

        if (hFile_ == INVALID_HANDLE_VALUE) {
            error_ = win_last_error("CreateFileW");
            return;
        }

        // Lấy kích thước
        LARGE_INTEGER li{};
        if (!::GetFileSizeEx(hFile_, &li) || li.QuadPart == 0) {
            // File rỗng hoặc lỗi — không cần map
            if (li.QuadPart == 0) {
                // Hợp lệ nhưng rỗng
                size_ = 0;
                ptr_  = nullptr; // ok() sẽ trả false — caller tự xử
            } else {
                error_ = win_last_error("GetFileSizeEx");
            }
            return;
        }
        size_ = static_cast<size_t>(li.QuadPart);

        // Tạo mapping
        hMapping_ = ::CreateFileMappingA(
            hFile_,
            nullptr,
            PAGE_READONLY,
            0, 0,   // map toàn bộ file
            nullptr);

        if (!hMapping_) {
            error_ = win_last_error("CreateFileMappingA");
            size_ = 0;
            return;
        }

        // Map view vào address space
        ptr_ = ::MapViewOfFile(hMapping_, FILE_MAP_READ, 0, 0, 0);
        if (!ptr_) {
            error_ = win_last_error("MapViewOfFile");
            size_ = 0;
        }
    }

    void unmap() noexcept {
        if (ptr_)      { ::UnmapViewOfFile(ptr_);   ptr_ = nullptr; }
        if (hMapping_) { ::CloseHandle(hMapping_);  hMapping_ = nullptr; }
        if (hFile_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(hFile_);
            hFile_ = INVALID_HANDLE_VALUE;
        }
        size_ = 0;
    }

    void move_from(MappedFile& o) noexcept {
        ptr_      = o.ptr_;      o.ptr_      = nullptr;
        size_     = o.size_;     o.size_     = 0;
        path_     = std::move(o.path_);
        error_    = std::move(o.error_);
        hFile_    = o.hFile_;    o.hFile_    = INVALID_HANDLE_VALUE;
        hMapping_ = o.hMapping_; o.hMapping_ = nullptr;
    }

    static std::string win_last_error(const char* fn) noexcept {
        DWORD err = ::GetLastError();
        char buf[256] = {};
        ::FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, err,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            buf, static_cast<DWORD>(sizeof(buf) - 1), nullptr);
        return std::string(fn) + ": " + buf;
    }

#else
    // ── POSIX (Linux / macOS) ─────────────────────────────────────────────────
    int fd_ = -1;

    void open_and_map(const char* path) noexcept {
        fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ == -1) {
            error_ = std::string("open: ") + ::strerror(errno);
            return;
        }

        struct stat st{};
        if (::fstat(fd_, &st) == -1) {
            error_ = std::string("fstat: ") + ::strerror(errno);
            ::close(fd_); fd_ = -1;
            return;
        }

        if (st.st_size == 0) {
            // File rỗng — không cần mmap
            size_ = 0;
            return;
        }
        size_ = static_cast<size_t>(st.st_size);

        // MAP_PRIVATE + PROT_READ — OS tự quản lý paging
        void* m = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (m == MAP_FAILED) {
            error_ = std::string("mmap: ") + ::strerror(errno);
            size_ = 0;
            m = nullptr;
        }
        ptr_ = m;

        // Hint tuần tự → OS prefetch tối ưu cho PDF parse
        if (ptr_) {
            ::madvise(const_cast<void*>(ptr_), size_, MADV_SEQUENTIAL);
        }
    }

    void unmap() noexcept {
        if (ptr_ && size_) {
            ::munmap(const_cast<void*>(ptr_), size_);
            ptr_ = nullptr;
        }
        if (fd_ != -1) { ::close(fd_); fd_ = -1; }
        size_ = 0;
    }

    void move_from(MappedFile& o) noexcept {
        ptr_   = o.ptr_;   o.ptr_   = nullptr;
        size_  = o.size_;  o.size_  = 0;
        fd_    = o.fd_;    o.fd_    = -1;
        path_  = std::move(o.path_);
        error_ = std::move(o.error_);
    }
#endif
};

} // namespace WinExtract

#endif // WINNERZ_MAPPED_FILE_HPP
