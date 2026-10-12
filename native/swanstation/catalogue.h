// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>
#include <string>
namespace SwanCatalogue {
struct Group { std::string name, code, description; unsigned lines=0; bool unsupported=false; };
inline std::string Trim(std::string text) {
  const auto first=text.find_first_not_of(" \t\r\n"),last=text.find_last_not_of(" \t\r\n");
  return first==std::string::npos?std::string():text.substr(first,last-first+1);
}
inline std::vector<Group> Parse(const std::string& text) {
  std::vector<Group> groups;
  static const std::regex code(R"(^([0-9A-Fa-f]{8})\s*[:,]?\s*([0-9A-Fa-f]{1,8})\s*(?:(?:#|;|//).*)?$)");
  std::istringstream input(text);std::string line;
  while (std::getline(input,line)) {
    line=Trim(line);if (line.compare(0,3,"\xef\xbb\xbf")==0)line.erase(0,3);
    if(line.empty() || line[0]=='#' || line[0]==';' || line.rfind("//",0)==0)continue;
    if(line[0]=='[') {
      if(groups.size()>=4096)break;
      const auto end=line.rfind(']');if(end==std::string::npos)continue;
      auto name=Trim(line.substr(1,end-1));if(name.empty())name="(unnamed)";
      // Record separators must never escape into the frontend wire format.
      for(char& c:name)if(static_cast<unsigned char>(c)<32)c=' ';
      groups.push_back({name});continue;
    }
    if(line.rfind("@desc",0)==0) {
      if(!groups.empty()) { if(!groups.back().description.empty())groups.back().description+=' ';groups.back().description+=Trim(line.substr(5)); }
      continue;
    }
    std::smatch match;if(!std::regex_match(line,match,code))continue;
    if(groups.empty())groups.push_back({"Unnamed codes"});
    auto& group=groups.back();if(group.lines>=4096){group.unsupported=true;continue;}
    // Only advertise the code families our catalogue UI and tests cover.
    unsigned type=std::stoul(match[1].str().substr(0,2),nullptr,16);
    if(!(type==0x30 || type==0x80 || type==0x50 || (type>=0xd0&&type<=0xd3) || (type>=0xe0&&type<=0xe3)))group.unsupported=true;
    if(!group.code.empty())group.code+='+';
    group.code+=match[1].str()+" "+match[2].str();group.lines++;
  }
  return groups;
}
inline bool Read(const std::string& path,std::vector<Group>& groups) {
  groups.clear();if(path.empty())return true;
  std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input || input.tellg()>2*1024*1024)return false;
  const auto length=input.tellg();if(length<0)return false;
  std::string text(static_cast<size_t>(length),'\0');input.seekg(0);
  if(!input.read(text.data(),text.size()))return false;
  groups=Parse(text);return true;
}
}
