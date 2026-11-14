#pragma once
#include "sf/common.hpp"
namespace sf {
// SFPv1 wire format: all header integers big endian, float32 IQ samples little endian.
constexpr uint32_t SFP_MAGIC=0x53465031; // SFP1
constexpr uint16_t SFP_HEADER_BYTES=56;
constexpr uint16_t SFP_MAX_CHUNKS=128;
constexpr size_t SFP_MAX_PAYLOAD=1200;
struct Packet {
 uint32_t source_id=1,stream_id=1; uint64_t seq=0,frame_id=0,source_ts_ns=0;
 uint16_t chunk_index=0,chunk_count=1,format=1,flags=0;
 std::vector<uint8_t> payload;
};
uint32_t crc32(const uint8_t* p,size_t n);
std::vector<uint8_t> encode(const Packet& p);
bool decode(const uint8_t* p,size_t n,Packet& out,std::string& err);
inline bool decode(const std::vector<uint8_t>& b,Packet& out,std::string& err){return decode(b.data(),b.size(),out,err);}
inline void append_float_le(std::vector<uint8_t>& b,float v){static_assert(sizeof(float)==4,"IEEE-754 float required");uint32_t u;std::memcpy(&u,&v,4);for(int j=0;j<4;j++)b.push_back(uint8_t(u>>(8*j)));}
inline float read_float_le(const uint8_t* b){uint32_t u=uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);float f;std::memcpy(&f,&u,4);return f;}
}
