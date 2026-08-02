#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "frontend/archive.h"

namespace {

bool ReadContains(const std::filesystem::path& path, const std::string& expected) {
    std::ifstream input(path, std::ios::binary);
    const std::string content(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
    return content.find(expected) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "ZIP_INTEGRATION failed reason=usage\n";
        return 1;
    }

    const std::filesystem::path valid_zip = argv[1];
    const std::filesystem::path unsafe_zip = argv[2];
    std::error_code ec;

    std::string error;
    const auto candidate = armsx::ExtractZipLaunchCandidate(valid_zip, error);
    if (!candidate || candidate->extension() != ".cue" ||
        !std::filesystem::is_regular_file(*candidate) ||
        !std::filesystem::is_regular_file(candidate->parent_path() / "disc.bin") ||
        !ReadContains(*candidate, "disc.bin")) {
        std::cerr << "ZIP_INTEGRATION failed reason=valid-archive error=" << error << '\n';
        return 1;
    }

    const auto extracted_root = candidate->parent_path().parent_path();
    std::filesystem::remove_all(extracted_root, ec);

    error.clear();
    const auto unsafe_candidate = armsx::ExtractZipLaunchCandidate(unsafe_zip, error);
    if (unsafe_candidate || error.find("unsafe path") == std::string::npos) {
        std::cerr << "ZIP_INTEGRATION failed reason=traversal-not-rejected error=" << error << '\n';
        return 1;
    }

    std::cout << "ZIP_INTEGRATION passed candidate=cue companions=preserved traversal=rejected\n";
    return 0;
}
