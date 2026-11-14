#include "sf/protocol.hpp"
#include "sf/net.hpp"
#include <random>
using namespace sf;
static void help(){std::cout<<"sf_source: --host 127.0.0.1 --port 9009 --frames 300 --sources 1 --rate 200\n  --frame-samples 1024 --chunk-samples 128 --modulation qpsk --snr-db 25\n  --drop-every 0 --duplicate-every 0 --swap-every 0 --corrupt-every 0\n  --jitter-ms 0 --burst-every 0 --iq-file recording.sigmf-data --loop\n  --stream-id 0 (0=auto new session on restart)\n";}
int main(int argc,char** argv){
 try{
 Args a(argc,argv);if(a.flag("help")){help();return 0;}
 int frames=a.integer("frames",300),sources=a.integer("sources",1),rate=a.integer("rate",200);
 int frame_samples=a.integer("frame-samples",1024),chunk_samples=a.integer("chunk-samples",128);
 int drop=a.integer("drop-every",0),duplicate=a.integer("duplicate-every",0),swap=a.integer("swap-every",0),corrupt=a.integer("corrupt-every",0);
 int jitter=a.integer("jitter-ms",0),burst=a.integer("burst-every",0);
 int seed=a.integer("seed",42),port=a.integer("port",9009);double snr=a.real("snr-db",25);
 const int requested_stream=a.integer("stream-id",0);require(requested_stream>=0,"stream-id must be 0 (auto) or positive");
 const uint32_t stream_id=requested_stream?uint32_t(requested_stream):uint32_t((epoch_ms() ^ ns_now()) & 0xffffffffU);
 require(frames>0&&frames<=100000000&&sources>0&&sources<=100&&rate>0&&rate<=1000000,"invalid frames/sources/rate");
 require(frame_samples>0&&frame_samples<=16384&&chunk_samples>0&&chunk_samples<=150,"invalid sample counts");
 require((frame_samples+chunk_samples-1)/chunk_samples<=SFP_MAX_CHUNKS,"too many chunks");
 require(port>0&&port<=65535&&drop>=0&&duplicate>=0&&swap>=0&&corrupt>=0&&jitter>=0&&jitter<=1000&&burst>=0,"invalid faults/port");
 std::string mod=a.get("modulation","qpsk");require(mod=="qpsk"||mod=="bpsk"||mod=="16qam","modulation must be bpsk/qpsk/16qam");
 std::mt19937 rng{uint32_t(seed)};std::uniform_int_distribution<int> bits(0,15);std::normal_distribution<float> gaussian(0,1);std::uniform_int_distribution<int> jitter_dist(0,jitter);
 double noise_sd=std::pow(10.0,-snr/20.0)/std::sqrt(2.0);
 [[maybe_unused]] net::Runtime network;net::OwnedSocket sock(::socket(AF_INET,SOCK_DGRAM,0));net::require_socket(sock.get(),"socket");
 sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_port=htons(uint16_t(port));
 require(inet_pton(AF_INET,a.get("host","127.0.0.1").c_str(),&dst.sin_addr)==1,"invalid destination IPv4");
 std::ifstream iq;
 if(!a.get("iq-file").empty()){iq.open(fs_path(a.get("iq-file")),std::ios::binary);require(bool(iq),"cannot open IQ file");}
 std::vector<uint64_t> seq(size_t(sources),0);uint64_t prepared=0,sent=0,dropped=0,duplicated=0,corrupted=0,send_failed=0;
 auto start=Clock::now();bool exhausted=false;
 for(int frame=0;frame<frames&&!exhausted;frame++){
  for(int src=0;src<sources&&!exhausted;src++){
   std::vector<uint8_t> iqframe; iqframe.reserve(size_t(frame_samples)*8);
   if(iq.is_open()){
    iqframe.resize(size_t(frame_samples)*8);
    if(!iq.read(reinterpret_cast<char*>(iqframe.data()),std::streamsize(iqframe.size()))){
     if(!a.flag("loop")){exhausted=true;break;}
     iq.clear();iq.seekg(0);require(bool(iq.read(reinterpret_cast<char*>(iqframe.data()),std::streamsize(iqframe.size()))),"IQ file shorter than one frame");
    }
   }else{
    for(int i=0;i<frame_samples;i++){
     int sym=bits(rng),x=sym&3,y=(sym>>2)&3;float re=0,im=0;
     if(mod=="bpsk")re=(sym&1)?1.f:-1.f;
     else if(mod=="qpsk"){re=(sym&1)?.70710678f:-.70710678f;im=(sym&2)?.70710678f:-.70710678f;}
     else{re=float(2*x-3)/std::sqrt(10.f);im=float(2*y-3)/std::sqrt(10.f);}
     re+=float(noise_sd)*gaussian(rng);im+=float(noise_sd)*gaussian(rng);
     append_float_le(iqframe,re);append_float_le(iqframe,im);
    }
   }
   std::vector<std::vector<uint8_t>> datagrams;
   int chunks=(frame_samples+chunk_samples-1)/chunk_samples;
   const uint64_t ts=ns_now();
   for(int chunk=0;chunk<chunks;chunk++){
    int begin=chunk*chunk_samples,end=std::min(begin+chunk_samples,frame_samples);
    Packet p;p.source_id=uint32_t(src+1);p.stream_id=stream_id;p.seq=seq[src]++;p.frame_id=uint64_t(frame);
    p.chunk_index=uint16_t(chunk);p.chunk_count=uint16_t(chunks);p.source_ts_ns=ts;
    p.payload.assign(iqframe.begin()+begin*8,iqframe.begin()+end*8);
    datagrams.push_back(encode(p));
   }
   if(swap>0&&(frame+1)%swap==0)for(size_t i=0;i+1<datagrams.size();i+=2)std::swap(datagrams[i],datagrams[i+1]);
   for(auto& packet:datagrams){
    prepared++;if(drop>0&&prepared%uint64_t(drop)==0){dropped++;continue;}
    if(corrupt>0&&prepared%uint64_t(corrupt)==0){packet.back()^=0x3f;corrupted++;}
    auto transmit=[&](){int count=::sendto(sock.get(),reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&dst),sizeof(dst));if(count==int(packet.size()))sent++;else send_failed++;};
    transmit();if(duplicate>0&&prepared%uint64_t(duplicate)==0){transmit();duplicated++;}
   }
   if(jitter)std::this_thread::sleep_for(std::chrono::milliseconds(jitter_dist(rng)));
  }
  if(burst==0||(frame+1)%burst==0){
   auto target=start+std::chrono::microseconds(int64_t((frame+1)*1000000.0/rate));std::this_thread::sleep_until(target);
  }
 }
 std::cout<<"SOURCE {\"prepared\":"<<prepared<<",\"sent\":"<<sent<<",\"injected_drops\":"<<dropped
  <<",\"injected_duplicates\":"<<duplicated<<",\"corrupted\":"<<corrupted<<",\"send_failed\":"<<send_failed<<",\"stream_id\":"<<stream_id<<"}"<<std::endl;
 return send_failed?2:0;
 }catch(const std::exception& e){std::cerr<<"sf_source ERROR: "<<e.what()<<std::endl;return 1;}
}
