// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/state_media.h"
#include <cassert>
#include <iostream>
#include <vector>

int main() {
  const std::string root = "/data/user/0/com.nanodata.armsx/cache/saf-launch/";
  const std::string pbp = "game-"+std::string(64, 'a')+".pbp";
  const std::string disc2 = "game-"+std::string(64, 'b')+".chd";
  const auto old = root+"291269105836553-14/";
  const auto live = root+"292339375814166-19/";
  std::vector<std::string> loaded{live+pbp, live+disc2};
  auto known = [&](const std::string& path) {
    return std::find(loaded.begin(), loaded.end(), path) != loaded.end();
  };
  auto resolve = [&](const std::string& saved) { return SwanStateMedia::Resolve(saved, live+pbp, known); };
  // The report's old/current PBP launch directory mismatch, including relaunch.
  assert(resolve(old+pbp) == live+pbp);
  assert(resolve(live+pbp) == live+pbp);
  // An M3U state for another known disc must select that disc, not the mounted one.
  assert(resolve(old+disc2) == live+disc2);
  assert(resolve(live+disc2) == live+disc2);
  // Unknown games, title-only matches, other sandboxes and path traversal fail closed.
  assert(resolve(old+"game-"+std::string(64, 'c')+".pbp").empty());
  assert(resolve(old+"Racing Lagoon.pbp").empty());
  assert(resolve("/data/user/0/other.app/cache/saf-launch/1-1/"+pbp).empty());
  assert(resolve(old+"../"+pbp).empty());
  assert(resolve(root+"../1-1/"+pbp).empty());
  assert(resolve(root+"1--1/"+pbp).empty());
  assert(resolve(root+"1-1/nested/"+pbp).empty());
  assert(resolve(old+pbp+std::string(1, '\0')+"ignored").empty());
  assert(resolve(std::string(5000, 'a')).empty());
  assert(resolve("").empty());
  assert(SwanStateMedia::Parse(old+"game-"+std::string(64, 'G')+".pbp").filename.empty());
  assert(SwanStateMedia::Parse(old+pbp+"/").filename.empty());
  assert(SwanStateMedia::Parse(old+"game-"+std::string(64, 'a')+".").filename.empty());
  // Ordinary current/local media still use the original exact-path allowlist.
  const std::string direct = "/storage/emulated/0/Games/game.pbp";
  assert(SwanStateMedia::Resolve(direct, direct, known) == direct);
  assert(SwanStateMedia::Resolve("/old/game.pbp", direct, known).empty());
  // Even a well-formed relocated name is unusable unless already loaded.
  assert(SwanStateMedia::Resolve(old+pbp, live+pbp, [](const auto&) { return false; }).empty());
  std::cout << "Save-state media relocation regressions passed\n";
}
