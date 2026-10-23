// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string>
#include <string_view>

namespace SwanStateMedia {
struct StagedPath {
  std::string_view cache;
  std::string_view directory;
  std::string_view filename;
};

// Match Ps1SafAccess/Ps1SafText, not arbitrary basenames or game serials. The
// SHA-256 in game-<digest> identifies the granted document across launch copies.
inline StagedPath Parse(std::string_view path) {
  if (path.empty() || path.size() > 4096 || path.front() != '/' ||
      path.find('\0') != path.npos || path.find('\\') != path.npos) return {};
  for (size_t start = 1; start < path.size();) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == path.npos ? path.size()-start : end-start);
    if (part.empty() || part == "." || part == "..") return {};
    if (end == path.npos) break;
    start = end+1;
  }
  constexpr std::string_view marker = "/cache/saf-launch/";
  const auto marker_at = path.find(marker);
  if (marker_at == path.npos || marker_at == 0) return {};
  const auto session_at = marker_at+marker.size();
  const auto filename_at = path.find('/', session_at);
  if (filename_at == path.npos) return {};
  const auto session = path.substr(session_at, filename_at-session_at);
  const auto dash = session.find('-');
  const auto digits = [](std::string_view text) {
    return !text.empty() && text.size() <= 24 &&
      std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  if (dash == session.npos || !digits(session.substr(0, dash)) ||
      !digits(session.substr(dash+1))) return {};
  const auto name = path.substr(filename_at+1);
  if (name.size() < 71 || name.size() > 78 || name.substr(0, 5) != "game-" || name[69] != '.') return {};
  if (!std::all_of(name.begin()+5, name.begin()+69, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }) || !std::all_of(name.begin()+70, name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      })) return {};
  return {path.substr(0, session_at), path.substr(0, filename_at+1), name};
}

// Only return paths the frontend already loaded. Never open the stale path,
// weaken IsKnownMediaPath, guess by title, or silently substitute another disc.
template<class IsKnown>
std::string Resolve(const std::string& saved, const std::string& current, IsKnown known) {
  if (saved.empty() || saved.size() > 4096 || saved.find('\0') != saved.npos) return {};
  if (saved == current || known(saved)) return saved;
  const auto old = Parse(saved), live = Parse(current);
  if (old.filename.empty() || live.filename.empty() || old.cache != live.cache) return {};
  const std::string candidate = std::string(live.directory)+std::string(old.filename);
  return known(candidate) ? candidate : std::string{};
}
} // namespace SwanStateMedia
