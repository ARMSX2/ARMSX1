#include "archive.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <system_error>
#include <vector>

#ifdef USE_CHD
extern "C" {
#include "miniz.h"
}
#endif

namespace armsx {
namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsExePath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".exe" || ext == ".ps-exe" || ext == ".psexe";
}

#ifdef USE_CHD
bool SafeArchiveRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

int LaunchCandidatePriority(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    if (ext == ".cue") return 0;
    if (ext == ".chd") return 1;
    if (ext == ".iso") return 2;
    if (ext == ".img") return 3;
    if (ext == ".bin") return 4;
    if (IsExePath(path)) return 5;
    return 100;
}
#endif

}  // namespace

bool IsZipPath(const std::filesystem::path& path) {
    return ToLower(path.extension().string()) == ".zip";
}

#ifdef USE_CHD
std::optional<std::filesystem::path> ExtractZipLaunchCandidate(
    const std::filesystem::path& archive_path,
    std::string& error
) {
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, archive_path.string().c_str(), 0)) {
        error = "The ZIP archive could not be opened.";
        return std::nullopt;
    }

    struct ArchiveGuard {
        mz_zip_archive* archive;
        ~ArchiveGuard() { mz_zip_reader_end(archive); }
    } guard{&archive};

#if defined(__EMSCRIPTEN__)
    constexpr mz_uint64 kMaxArchiveBytes = 1536ull * 1024ull * 1024ull;
#else
    constexpr mz_uint64 kMaxArchiveBytes = 8ull * 1024ull * 1024ull * 1024ull;
#endif

    std::error_code ec;
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const std::filesystem::path output_root =
        std::filesystem::temp_directory_path(ec) /
        "armsx-zip" /
        (archive_path.stem().string() + "-" + std::to_string(nonce));
    if (ec) {
        error = "ARMSX could not resolve temporary ZIP storage.";
        return std::nullopt;
    }
    std::filesystem::create_directories(output_root, ec);
    if (ec) {
        error = "ARMSX could not create temporary ZIP storage.";
        return std::nullopt;
    }

    mz_uint64 total_uncompressed = 0;
    std::vector<std::filesystem::path> candidates;
    const mz_uint file_count = mz_zip_reader_get_num_files(&archive);
    if (file_count > 4096u) {
        error = "The ZIP contains too many entries.";
        return std::nullopt;
    }
    for (mz_uint index = 0; index < file_count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
            error = "The ZIP central directory is invalid.";
            return std::nullopt;
        }

        const mz_uint filename_size = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
        if (filename_size < 2u || filename_size > 4096u) {
            error = "The ZIP contains an invalid file name.";
            return std::nullopt;
        }
        std::vector<char> filename(filename_size);
        if (mz_zip_reader_get_filename(&archive, index, filename.data(), filename_size) != filename_size) {
            error = "The ZIP file name could not be decoded.";
            return std::nullopt;
        }
        const std::filesystem::path relative =
            std::filesystem::path(filename.data()).lexically_normal();
        if (!SafeArchiveRelativePath(relative)) {
            error = "The ZIP contains an unsafe path.";
            return std::nullopt;
        }

        const std::filesystem::path destination = output_root / relative;
        if (mz_zip_reader_is_file_a_directory(&archive, index)) {
            std::filesystem::create_directories(destination, ec);
            if (ec) {
                error = "ARMSX could not create a ZIP output directory.";
                return std::nullopt;
            }
            continue;
        }
        if (mz_zip_reader_is_file_encrypted(&archive, index)) {
            error = "Encrypted ZIP archives are not supported.";
            return std::nullopt;
        }
        if (!mz_zip_reader_is_file_supported(&archive, index)) {
            error = "The ZIP uses an unsupported compression method.";
            return std::nullopt;
        }

        if (stat.m_uncomp_size > kMaxArchiveBytes ||
            total_uncompressed > (kMaxArchiveBytes - stat.m_uncomp_size)) {
            error = "The ZIP expands beyond ARMSX's safety limit.";
            return std::nullopt;
        }
        total_uncompressed += stat.m_uncomp_size;

        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec || !mz_zip_reader_extract_to_file(
                &archive, index, destination.string().c_str(), 0)) {
            error = "A file could not be extracted from the ZIP archive.";
            return std::nullopt;
        }
        if (LaunchCandidatePriority(destination) < 100) {
            candidates.push_back(destination);
        }
    }

    if (candidates.empty()) {
        error = "The ZIP does not contain a supported PlayStation image or executable.";
        return std::nullopt;
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return LaunchCandidatePriority(lhs) < LaunchCandidatePriority(rhs);
    });
    return candidates.front();
}
#endif

}  // namespace armsx
