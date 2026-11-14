#include "sf/protocol.hpp"
namespace sf {
static const std::array<uint32_t,256>& crc_table(){
 // One-time table initialization is thread safe since C++11. The 256-entry
 // table stays fixed and small; no per-packet allocation or temporary copy.
 static const auto table=[] {
  std::array<uint32_t,256> t{};
  for(uint32_t i=0;i<256;++i){
   uint32_t c=i;
   for(int j=0;j<8;++j)c=(c>>1)^(0xEDB88320U & (0U-(c&1U)));
   t[i]=c;
  }
  return t;
 }();
 return table;
}
static uint32_t crc_update(uint32_t c,const uint8_t* p,size_t n){
 const auto& table=crc_table();
 for(size_t i=0;i<n;++i)c=(c>>8)^table[(c^p[i])&0xffU];
 return c;
}
uint32_t crc32(const uint8_t* p,size_t n){
 return ~crc_update(0xFFFFFFFFU,p,n);
}
static uint32_t crc32_zeroed_checksum_field(const uint8_t* b,size_t n){
 uint32_t c=crc_update(0xFFFFFFFFU,b,52);
 constexpr uint8_t zeros[4]{};
 c=crc_update(c,zeros,4);
 return ~crc_update(c,b+56,n-56);
}
std::vector<uint8_t> encode(const Packet& p){
 require(p.chunk_count>0&&p.chunk_count<=SFP_MAX_CHUNKS&&p.chunk_index<p.chunk_count,"invalid chunk indexes");
 require(p.format==1&&p.flags==0,"unsupported IQ format or protocol flags");
 require(!p.payload.empty()&&p.payload.size()<=SFP_MAX_PAYLOAD && p.payload.size()%8==0,"payload must be 8..1200 bytes of cf32 IQ");
 std::vector<uint8_t> b;b.reserve(SFP_HEADER_BYTES+p.payload.size());
 put32(b,SFP_MAGIC);put16(b,1);put16(b,SFP_HEADER_BYTES);put32(b,p.source_id);put32(b,p.stream_id);
 put64(b,p.seq);put64(b,p.frame_id);put16(b,p.chunk_index);put16(b,p.chunk_count);
 put16(b,p.format);put16(b,p.flags);put64(b,p.source_ts_ns);put16(b,uint16_t(p.payload.size()));put16(b,0);put32(b,0);
 b.insert(b.end(),p.payload.begin(),p.payload.end());const uint32_t crc=crc32(b.data(),b.size());
 for(int i=0;i<4;i++)b[52+i]=uint8_t(crc>>(8*(3-i)));
 return b;
}
bool decode(const uint8_t* b,size_t n,Packet& out,std::string& err){
 if(!b&&n){err="null input";return false;}
 if(n<SFP_HEADER_BYTES){err="short header";return false;}
 if(get32(b)!=SFP_MAGIC||get16(b+4)!=1||get16(b+6)!=SFP_HEADER_BYTES){err="bad magic/version/header";return false;}
 uint16_t chunks=get16(b+34),idx=get16(b+32),format=get16(b+36),payload=get16(b+48);
 if(!chunks||chunks>SFP_MAX_CHUNKS||idx>=chunks||format!=1||get16(b+38)!=0||get16(b+50)!=0||payload==0||payload>SFP_MAX_PAYLOAD||payload%8||n!=SFP_HEADER_BYTES+size_t(payload)){err="invalid header fields or payload size";return false;}
 // The checksum field is zero during CRC calculation. Compute it in
 // three spans without copying or allocating a temporary datagram.
 uint32_t expected=get32(b+52);
 uint32_t actual=crc32_zeroed_checksum_field(b,n);
 if(actual!=expected){err="crc mismatch";return false;}
 out.source_id=get32(b+8);out.stream_id=get32(b+12);out.seq=get64(b+16);out.frame_id=get64(b+24);
 out.chunk_index=idx;out.chunk_count=chunks;out.format=format;out.flags=get16(b+38);
 out.source_ts_ns=get64(b+40);out.payload.assign(b+SFP_HEADER_BYTES,b+n);err.clear();return true;
}
}
