#pragma once
#include "sf/recorder.hpp"
namespace sf {
struct EngineConfig {
 size_t queue_capacity=256;int workers=2;uint64_t timeout_ms=500,sequence_timeout_ms=400;
 size_t max_inflight_frames=1024;int work_us=0;int frame_samples=1024,max_incidents=32;
 std::string record_dir,results_out,dsp_backend="fft";bool capture_start=false,replay=false;
};
struct FrameTask{Frame frame;Mode desired=Mode::Normal;};
class Engine {
 EngineConfig cfg_;Metrics metrics_;SeqTracker seq_;FrameAssembler frames_;BoundedQueue<FrameTask> tasks_;PressureController pressure_;
 std::unique_ptr<IAnalyzer> analyzer_;std::unique_ptr<FlightRecorder> recorder_;
 std::vector<std::thread> workers_;std::mutex results_mu_,ingress_mu_;std::ofstream results_file_;bool closed_=false;uint64_t start_ns_;
 void process(FrameTask task);
 void event(const std::string& type,const std::string& info,bool decision=false);
public:
 explicit Engine(EngineConfig config);
 ~Engine();
 void ingest(const uint8_t* data,size_t n,uint64_t when=0);
 void ingest(const std::vector<uint8_t>& b,uint64_t when=0){ingest(b.data(),b.size(),when);}
 void tick(uint64_t when=0);
 void close();
 Metrics& metrics(){return metrics_;}
 const Metrics& metrics()const{return metrics_;}
 size_t queue_size()const{return tasks_.size();}
 uint64_t elapsed_ms()const{return (ns_now()-start_ns_)/1000000ULL;}
};
}
