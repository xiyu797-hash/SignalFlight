#include "sf/engine.hpp"
using namespace sf;
static void help(){std::cout<<"sf_replay --input recordings/incident_... [--speed 0] [--work-us 0] [--metrics-out replay.json]\n --speed 0: fastest logical-time replay; --speed 1: original packet timing; --speed 2: twice as fast\n";}
int main(int argc,char** argv){
 try{
 Args a(argc,argv);if(a.flag("help")){help();return 0;}
 require(!a.has("onnx-model"),"Systems Edition does not support --onnx-model");
 require(a.has("input"),"--input directory or packets.bin required");
 auto path=fs_path(a.get("input"));if(std::filesystem::is_directory(path))path/="packets.bin";
 // Preflight the complete recording before creating the result file or
 // running analysis. An invalid footer must never produce partial output that
 // looks like a successful regression run.
 {
  CaptureReader verifier(path);PacketRecord candidate;
  bool first=true;uint64_t prev=0,initial=0;
  while(verifier.next(candidate)){
   if(first){initial=candidate.offset_ns;first=false;}
   require(candidate.offset_ns>=prev,"recording timestamps are not monotonic");
   require(candidate.offset_ns-initial<=86400ULL*1000000000ULL,"recording timestamp span exceeds 24 hours");
   prev=candidate.offset_ns;
  }
 }
 CaptureReader reader(path);
 if(!reader.has_integrity_footer())
  std::cerr<<"WARNING: legacy SFR0001 recording has no end-to-end footer; "
             "aligned truncation cannot be detected. Use new SFR0002 captures.\n";
 EngineConfig cfg;cfg.replay=true;cfg.work_us=a.integer("work-us",0);
 cfg.frame_samples=a.integer("frame-samples",1024);cfg.results_out=a.get("results-out");cfg.dsp_backend=a.get("dsp-backend","fft");
 cfg.timeout_ms=uint64_t(a.integer("timeout-ms",500));cfg.sequence_timeout_ms=uint64_t(a.integer("sequence-timeout-ms",400));
 Engine e(cfg);PacketRecord packet;uint64_t count=0,last_offset=0,first_offset=0;double speed=a.real("speed",0);
 require(speed>=0&&speed<=1000,"speed must be 0..1000");const auto wall_start=Clock::now();
 constexpr uint64_t logical_start=1000000000ULL;
 while(reader.next(packet)){
  if(count==0)first_offset=packet.offset_ns;
  require(packet.offset_ns>=first_offset,"recording timestamps out of order");
  const uint64_t relative=packet.offset_ns-first_offset;
  if(speed>0){auto t=wall_start+std::chrono::nanoseconds(uint64_t(relative/speed));std::this_thread::sleep_until(t);}
  last_offset=relative;e.ingest(packet.wire,logical_start+relative);count++;
  if(count%64==0)e.tick(logical_start+relative);
 }
 e.tick(logical_start+last_offset+1000000000ULL);e.close();
 std::cout<<"REPLAY {\"recorded_packets\":"<<count<<",\"metrics\":"<<e.metrics().json()<<"}"<<std::endl;
 if(!a.get("metrics-out").empty()){std::ofstream f(fs_path(a.get("metrics-out")));require(bool(f),"cannot open metrics-out");f<<e.metrics().json()<<'\n';}
 return 0;
 }catch(const std::exception& ex){std::cerr<<"sf_replay ERROR: "<<ex.what()<<std::endl;return 1;}
}
