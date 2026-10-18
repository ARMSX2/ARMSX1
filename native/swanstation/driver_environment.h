// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace SwanDriverEnvironment {
using Values = std::map<std::string,std::string>;
inline bool Parse(std::string_view text,Values& output) {
  if(text.size()>16384 || text.find('\0')!=text.npos)return false;
  Values next;
  const auto trim=[](std::string_view value){const auto first=value.find_first_not_of(" \t");
    return first==value.npos?std::string_view():value.substr(first,value.find_last_not_of(" \t")-first+1);};
  while(!text.empty()) {
    const auto end=text.find('\n');auto raw=text.substr(0,end);
    if(end!=text.npos&&!raw.empty()&&raw.back()=='\r')raw.remove_suffix(1);
    const auto line=trim(raw);
    if(!line.empty()&&line.front()!='#') {
      const auto equal=line.find('=');if(equal==line.npos||equal==0)return false;
      const auto name=trim(line.substr(0,equal)),value=trim(line.substr(equal+1));
      bool allowed=false;for(const auto prefix:{"FD_","TU_","IR3_","MESA_"}) {
        const std::string_view p(prefix);if(name.size()>p.size()&&name.substr(0,p.size())==p)allowed=true;
      }
      if(!allowed||name.size()>128||value.size()>2048)return false;
      for(const unsigned char c:name)if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;
      for(const unsigned char c:value)if(c<32||c==127)return false;
      if(value.find(std::string_view("\xc0\x80",2))!=value.npos)return false;
      if(!next.emplace(std::string(name),std::string(value)).second||next.size()>64)return false;
    }
    if(end==text.npos)break;text.remove_prefix(end+1);
  }
  output=std::move(next);return true;
}

/** Restore only keys WE changed, including their original absent/empty distinction. */
class Environment {
  std::map<std::string,std::optional<std::string>> original;
public:
  template<class Write> bool Restore(Write write) {
    bool okay=true;
    for(auto it=original.begin();it!=original.end();) {
      if(write(it->first,it->second))it=original.erase(it);else {okay=false;++it;}
    }
    return okay;
  }
  template<class Read,class Write> bool Apply(const Values& values,Read read,Write write) {
    if(!Restore(write))return false;
    for(const auto& value:values) {
      original[value.first]=read(value.first);
      if(!write(value.first,std::optional<std::string>(value.second))) {Restore(write);return false;}
    }
    return true;
  }
};
}
