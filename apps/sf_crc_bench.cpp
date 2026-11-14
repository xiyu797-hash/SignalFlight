// Compares the original bitwise CRC32 against the optimized table CRC32.
// Does not measure end-to-end UDP throughput. Both paths read identical bytes.
#include "sf/protocol.hpp"
#include <random>
#include <chrono>
using namespace sf;
static uint32_t slow_crc32(const uint8_t* p,size_t n){
 uint32_t c=0xFFFFFFFFU;
 for(size_t i=0;i<n;i++){
  c^=p[i];
  for(int j=0;j<8;j++)c=(c>>1)^(0xEDB88320U & (0U-(c&1U)));
 }
 return ~c;
}
int main(int argc,char** argv){
 try{
  Args args(argc,argv);
  int frames=args.integer("frames",4000);
  int bytes=args.integer("bytes",1080);
  const std::string algorithm=args.get("backend","table");
  require(frames>=1&&frames<=1000000&&bytes>=64&&bytes<=65535,"invalid CRC dimensions");
  require(algorithm=="table"||algorithm=="reference","--backend table|reference");
  std::mt19937 rng(20261105);
  std::vector<uint8_t> payload(static_cast<size_t>(bytes));
  for(auto& b:payload)b=uint8_t(rng());
  uint64_t checksum=0;
  const auto start=Clock::now();
  for(int i=0;i<frames;++i){
   // Changing one byte prevents the compiler from hoisting repeated CRC calls.
   payload[0]=uint8_t(i);
   const uint32_t c=algorithm=="table"?crc32(payload.data(),payload.size()):slow_crc32(payload.data(),payload.size());
   checksum+=c;
  }
  const auto elapsed=std::chrono::duration<double>(Clock::now()-start).count();
  std::cout<<std::fixed<<std::setprecision(9)
   <<"CRC_BENCH {\"algorithm\":\""<<algorithm<<"\",\"frames\":"<<frames
   <<",\"bytes\":"<<bytes<<",\"elapsed_seconds\":"<<elapsed
   <<",\"checksum\":"<<checksum<<"}"<<std::endl;
  return 0;
 }catch(const std::exception& e){std::cerr<<"sf_crc_bench ERROR: "<<e.what()<<std::endl;return 1;}
}
