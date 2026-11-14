#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
// Winsock2 must precede windows.h, including when another header pulls in
// common.hpp before net.hpp on Windows/MSYS2.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace sf {
// All command-line arguments and configuration path values are UTF-8. Native
// Windows filesystem paths are UTF-16. Never let std::filesystem convert a
// narrow Chinese path using the process ANSI/locale code page.
#ifdef _WIN32
inline std::string wide_to_utf8(const std::wstring& value) {
  if(value.empty())return {};
  const int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
  if(length<=0)throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
  std::string output(size_t(length),'\0');
  if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),output.data(),length,nullptr,nullptr)!=length)
    throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
  return output;
}
inline std::wstring utf8_to_wide(const std::string& value) {
  if(value.empty())return {};
  const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
  if(length<=0)throw std::runtime_error("UTF-8 to UTF-16 conversion failed (expected valid UTF-8 path)");
  std::wstring output(size_t(length),L'\0');
  if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),output.data(),length)!=length)
    throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
  return output;
}
inline std::filesystem::path fs_path(const std::string& utf8) {return std::filesystem::path(utf8_to_wide(utf8));}
inline std::string path_utf8(const std::filesystem::path& path) {return wide_to_utf8(path.wstring());}
#else
inline std::filesystem::path fs_path(const std::string& utf8) {return std::filesystem::path(utf8);}
inline std::string path_utf8(const std::filesystem::path& path) {return path.string();}
#endif
using Clock = std::chrono::steady_clock;
inline uint64_t ns_now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
inline uint64_t epoch_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
inline std::string json_escape(const std::string& s) { std::ostringstream o; for(unsigned char c:s) {if(c=='"')o<<"\\\"";else if(c=='\\')o<<"\\\\";else if(c=='\n')o<<"\\n";else if(c=='\r')o<<"\\r";else if(c=='\t')o<<"\\t";else if(c<32)o<<"?";else o<<c;}return o.str(); }
class Args {
  std::map<std::string,std::string> values_;
public:
  Args(int argc,char** argv) {
    // Windows' narrow argv uses an active ANSI code page, which corrupts
    // paths containing characters outside that page. Obtain the original
    // Unicode arguments from the Win32 wide command line, then normalize
    // them to UTF-8. This also works when PowerShell launches our program.
    std::vector<std::string> arguments;
#ifdef _WIN32
    int wide_count=0;
    LPWSTR* wide_argv=CommandLineToArgvW(GetCommandLineW(),&wide_count);
    if(!wide_argv)throw std::runtime_error("CommandLineToArgvW failed");
    try{
      for(int i=1;i<wide_count;++i)arguments.push_back(wide_to_utf8(wide_argv[i]));
    }catch(...){LocalFree(wide_argv);throw;}
    LocalFree(wide_argv);
    (void)argc;(void)argv;
#else
    for(int i=1;i<argc;++i)arguments.emplace_back(argv[i]);
#endif
    for(size_t i=0;i<arguments.size();++i) {
      std::string s=arguments[i];if(s.rfind("--",0)!=0)throw std::runtime_error("invalid argument: "+s);
      auto eq=s.find('=');if(eq!=std::string::npos){values_[s.substr(2,eq-2)]=s.substr(eq+1);continue;}
      if(i+1<arguments.size()&&arguments[i+1].rfind("--",0)!=0)values_[s.substr(2)]=arguments[++i];
      else values_[s.substr(2)]="true";
    }
    if(values_.count("config")) {
      std::ifstream f(fs_path(values_.at("config")));require_file_open(f,values_.at("config"));
      std::string line;int n=0;
      while(std::getline(f,line)) {++n;auto hash=line.find('#');if(hash!=std::string::npos)line.resize(hash);
        auto eq=line.find('=');if(eq==std::string::npos)continue;
        auto trim=[](std::string x){auto a=x.find_first_not_of(" \t\r\n");if(a==std::string::npos)return std::string{};auto b=x.find_last_not_of(" \t\r\n");return x.substr(a,b-a+1);};
        auto k=trim(line.substr(0,eq)),v=trim(line.substr(eq+1));
        if(k.empty())throw std::runtime_error("empty config key at line "+std::to_string(n));
        if(v.size()>=2&&v.front()=='"'&&v.back()=='"')v=v.substr(1,v.size()-2);
        if(!values_.count(k))values_[k]=v;
      }
    }
  }
  static void require_file_open(std::istream& f,const std::string& path){if(!f)throw std::runtime_error("cannot open config: "+path);}

  bool has(const std::string& k)const{return values_.count(k)>0;}
  std::string get(const std::string& k,const std::string& def="")const {auto it=values_.find(k);return it==values_.end()?def:it->second;}
  int integer(const std::string& k,int def)const {auto s=get(k);if(s.empty())return def;size_t n=0;int v=std::stoi(s,&n);if(n!=s.size())throw std::runtime_error("invalid integer --"+k);return v;}
  double real(const std::string& k,double def)const {auto s=get(k);if(s.empty())return def;size_t n=0;double v=std::stod(s,&n);if(n!=s.size()||!std::isfinite(v))throw std::runtime_error("invalid float --"+k);return v;}
  bool flag(const std::string& k)const {return has(k)&&get(k)!="false"&&get(k)!="0";}
};
inline void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
inline void put16(std::vector<uint8_t>& b,uint16_t x){b.push_back(uint8_t(x>>8));b.push_back(uint8_t(x));}
inline void put32(std::vector<uint8_t>& b,uint32_t x){for(int i=3;i>=0;--i)b.push_back(uint8_t(x>>(i*8)));}
inline void put64(std::vector<uint8_t>& b,uint64_t x){for(int i=7;i>=0;--i)b.push_back(uint8_t(x>>(i*8)));}
inline uint16_t get16(const uint8_t* p){return uint16_t((uint16_t(p[0])<<8)|p[1]);}
inline uint32_t get32(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
inline uint64_t get64(const uint8_t* p){uint64_t x=0;for(int i=0;i<8;i++)x=(x<<8)|p[i];return x;}
inline void write_u32(std::ostream& o,uint32_t x){char b[4];for(int i=0;i<4;i++)b[i]=char(x>>(i*8));o.write(b,4);}
inline void write_u64(std::ostream& o,uint64_t x){for(int i=0;i<8;i++){char b=char(x>>(i*8));o.write(&b,1);}}
inline bool read_u32(std::istream& i,uint32_t& x){char b[4];if(!i.read(b,4))return false;x=0;for(int n=3;n>=0;n--)x=(x<<8)|uint8_t(b[n]);return true;}
inline bool read_u64(std::istream& i,uint64_t& x){char b[8];if(!i.read(b,8))return false;x=0;for(int n=7;n>=0;n--)x=(x<<8)|uint8_t(b[n]);return true;}
inline std::string json_kv(const std::string& k,const std::string& v){return "\""+json_escape(k)+"\":\""+json_escape(v)+"\"";}
}
