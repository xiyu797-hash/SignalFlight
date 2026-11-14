#include "sf/engine.hpp"
using namespace sf;
int main(int argc,char** argv){
 try{
 Args a(argc,argv);require(!a.has("onnx-model"),"Systems Edition does not support --onnx-model");if(a.flag("help")){std::cout<<
    "sf_bench --frames 2000 --workers 2 --queue 256 --work-us 0\n"
    " --dsp-backend fft|reference --compute-only (isolates analysis runtime)\n"
    "\n";
    return 0;
 }
 int frames=a.integer("frames",2000),samples=a.integer("frame-samples",1024),chunk=a.integer("chunk-samples",128);
 int queue=a.integer("queue",256);
 require(frames>0&&samples>0&&chunk>0&&chunk<=150&&samples<=16384&&queue>0,"invalid benchmark dimensions");
 require((samples+chunk-1)/chunk<=SFP_MAX_CHUNKS,"too many chunks");
 const std::string backend=a.get("dsp-backend","fft");
 require(backend=="fft"||backend=="reference","invalid dsp backend");
 std::vector<uint8_t> data;data.reserve(size_t(samples)*8);for(int i=0;i<samples;i++){append_float_le(data,(i&1)?1.f:-1.f);append_float_le(data,0.3f);}
 if(a.flag("compute-only")){
  auto analyzer=make_analyzer(backend);
  Frame f;f.iq=std::move(data);double checksum=0;
  const auto start=Clock::now();
  for(int i=0;i<frames;++i){
   // Change the first sample so the optimizer cannot hoist computation.
   float v=float((i%31)-15)/16.0f;uint32_t bits=0;std::memcpy(&bits,&v,4);
   for(int j=0;j<4;j++)f.iq[j]=uint8_t(bits>>(j*8));
   auto result=analyzer->analyze(f,false);
   checksum+=result.mean_power+result.spectral_centroid;
  }
  auto elapsed=std::chrono::duration<double>(Clock::now()-start).count();
  std::cout<<"BENCH {\"mode\":\"compute-only\",\"backend\":\""<<backend<<"\",\"frames\":"<<frames
           <<",\"elapsed_seconds\":"<<elapsed<<",\"analysis_fps\":"<<frames/elapsed
           <<",\"checksum\":"<<std::setprecision(16)<<checksum<<"}"<<std::endl;
  return 0;
 }
 EngineConfig c;c.workers=a.integer("workers",2);c.queue_capacity=size_t(queue);c.frame_samples=samples;c.work_us=a.integer("work-us",0);c.dsp_backend=backend;
 Engine e(c);uint64_t seq=0;auto start=Clock::now();
 for(int f=0;f<frames;f++)for(int j=0;j<(samples+chunk-1)/chunk;j++){
  Packet p;p.seq=seq++;p.frame_id=uint64_t(f);p.chunk_index=uint16_t(j);p.chunk_count=uint16_t((samples+chunk-1)/chunk);p.source_ts_ns=f+1;
  const int begin=j*chunk,end=std::min(begin+chunk,samples);p.payload.assign(data.begin()+begin*8,data.begin()+end*8);
  e.ingest(encode(p));
 }
 e.close();double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
 std::cout<<"BENCH {\"elapsed_seconds\":"<<elapsed<<",\"incoming_fps\":"<<frames/elapsed<<",\"metrics\":"<<e.metrics().json()<<"}"<<std::endl;
 return 0;
 }catch(const std::exception& ex){std::cerr<<"sf_bench ERROR: "<<ex.what()<<std::endl;return 1;}
}
