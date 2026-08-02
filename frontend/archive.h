#ifndef ARMSX_FRONTEND_ARCHIVE_H
#define ARMSX_FRONTEND_ARCHIVE_H

#include <filesystem>
#include <optional>
#include <string>

namespace armsx {

bool IsZipPath(const std::filesystem::path& path);

#ifdef USE_CHD
std::optional<std::filesystem::path> ExtractZipLaunchCandidate(
    const std::filesystem::path& archive_path,
    std::string& error
);
#endif

}  // namespace armsx

#endif
