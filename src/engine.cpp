#include "sf/engine.hpp"
namespace sf {
Engine::Engine(EngineConfig config)
 :cfg_(std::move(config)),seq_(4096,cfg_.sequence_timeout_ms),frames_(cfg_.max_inflight_frames,cfg_.timeout_ms,size_t(cfg_.frame_samples)*8),tasks_(cfg_.queue_capacity),start_ns_(ns_now()){
 require(cfg_.workers>=1&&cfg_.workers<=128,"workers must be 1..128");
 require(cfg_.frame_samples>=1&&cfg_.frame_samples<=16384,"frame samples outside supported range");
 require(cfg_.work_us>=0&&cfg_.work_us<=10000000,"work-us out of bounds");
 require(cfg_.timeout_ms>0&&cfg_.sequence_timeout_ms>0,"timeouts must be positive");
 require(cfg_.max_incidents>=1&&cfg_.max_incidents<=256,"max-incidents must be 1..256");
 analyzer_=make_analyzer(cfg_.dsp_backend);
 if(!cfg_.results_out.empty()){
  const auto parent=fs_path(cfg_.results_out).parent_path();if(!parent.empty())std::filesystem::create_directories(parent);
  results_file_.open(fs_path(cfg_.results_out));require(bool(results_file_),"cannot open results-out");
 }
 if(!cfg_.record_dir.empty())recorder_=std::make_unique<FlightRecorder>(cfg_.record_dir,128,128,16,size_t(cfg_.max_incidents));
 if(recorder_&&cfg_.capture_start)recorder_->trigger("manual_start");
 if(!cfg_.replay)for(int i=0;i<cfg_.workers;i++)workers_.emplace_back([this]{FrameTask t;while(tasks_.pop(t))process(std::move(t));});
}
Engine::~Engine(){close();}
void Engine::event(const std::string& type,const std::string& info,bool decision){if(recorder_)recorder_->note(type,info,decision);}
void Engine::process(FrameTask task){
 try{
  if(cfg_.work_us>0)std::this_thread::sleep_for(std::chrono::microseconds(cfg_.work_us));
  bool light=task.desired!=Mode::Normal;
  const auto result=analyzer_->analyze(task.frame,light);
  {
   std::lock_guard<std::mutex> lock(results_mu_);
   // The ostream status flags are written during output; inspect them only
   // while holding the same mutex to avoid an iostream data race.
   if(results_file_.is_open()){
   results_file_<<"{\"source_id\":"<<task.frame.source_id<<",\"stream_id\":"<<task.frame.stream_id
    <<",\"frame_id\":"<<task.frame.id<<",\"mode\":\""<<(light?"light":"full")
    <<"\",\"mean_power\":"<<result.mean_power<<",\"spectral_centroid\":"<<result.spectral_centroid
    <<"}\n";
    if(!results_file_)throw std::runtime_error("failed to write analysis result");
   }
  }
  if(light)metrics_.analyzed_light++;else metrics_.analyzed_full++;
  if(!cfg_.replay&&task.frame.received_ns){auto now=ns_now();if(now>=task.frame.received_ns)metrics_.observe_latency_ns(now-task.frame.received_ns);}
 }catch(const std::exception& e){metrics_.analyzed_failed++;std::cerr<<"ANALYZER_ERROR frame="<<task.frame.id<<" "<<e.what()<<std::endl;event("analyzer_failure",e.what());}
}
void Engine::ingest(const uint8_t* data,size_t n,uint64_t when){
 std::lock_guard<std::mutex> ingress(ingress_mu_);
 if(closed_)return;
 if(!when)when=ns_now();
 metrics_.packets_rx++;metrics_.bytes_rx+=n;
 if(recorder_)recorder_->observe(std::vector<uint8_t>(data,data+n),when);
 Packet p;std::string err;
 if(!decode(data,n,p,err)){
  metrics_.bad_packets++;if(err=="crc mismatch")metrics_.crc_errors++;
  if(recorder_){recorder_->trigger("invalid_packet");event("invalid_packet",err);}return;
 }
 auto result=seq_.accept(p,when,metrics_);
 if(result==SeqTracker::Outcome::Rejected){event("source_rejected","source/stream limit exceeded");return;}
 if(result==SeqTracker::Outcome::TooOld){event("stale_packet","outside sequence deduplication window");return;}
 if(result==SeqTracker::Outcome::Duplicate){event("duplicate_packet",std::to_string(p.seq));return;}
 if(result==SeqTracker::Outcome::Reordered){if(recorder_)recorder_->trigger("packet_reordering");event("reordered_packet",std::to_string(p.seq));}
 auto f=frames_.accept(p,when,metrics_);if(!f)return;
 if(f->iq.size()!=size_t(cfg_.frame_samples)*8){metrics_.frames_rejected++;event("invalid_frame_size",std::to_string(f->iq.size()));return;}
 FrameTask task{std::move(*f),Mode::Normal};
 if(cfg_.replay){process(std::move(task));return;}
 auto occupancy=tasks_.size();const auto previous=pressure_.mode();auto mode=pressure_.update(occupancy,tasks_.capacity());
 if(mode!=previous){event("mode_change",std::string(mode_name(previous))+" -> "+mode_name(mode),true);if(recorder_)recorder_->trigger("mode_change");}
 task.desired=mode;
 event("analysis_decision",std::string("frame=")+std::to_string(task.frame.id)+",mode="+mode_name(mode),true);
 if(mode==Mode::Critical||!tasks_.try_push(std::move(task),&occupancy)){
  metrics_.queue_dropped++;event("drop_frame","queue pressure / policy",true);if(recorder_)recorder_->trigger("queue_overload");return;
 }
 uint64_t peak=metrics_.queue_peak.load();while(peak<occupancy&&!metrics_.queue_peak.compare_exchange_weak(peak,occupancy)){}
}
void Engine::tick(uint64_t when){
 std::lock_guard<std::mutex> ingress(ingress_mu_);
 if(closed_)return;
 if(!when)when=ns_now();
 const uint64_t missing=metrics_.missing_final.load(),expired=metrics_.frames_expired.load();
 seq_.expire(when,metrics_);frames_.expire(when,metrics_);
 if(metrics_.missing_final.load()>missing){if(recorder_)recorder_->trigger("confirmed_missing_packet");event("missing_packets",std::to_string(metrics_.missing_final.load()-missing));}
 if(metrics_.frames_expired.load()>expired){if(recorder_)recorder_->trigger("incomplete_frames");event("incomplete_frames",std::to_string(metrics_.frames_expired.load()-expired));}
}
void Engine::close(){
 // Incoming UDP packets, timer expiration and shutdown all mutate the same
 // sequence/frame state. Keep one serialized ingress boundary.
 std::lock_guard<std::mutex> ingress(ingress_mu_);
 if(closed_)return;
 closed_=true;
 seq_.finish(metrics_);frames_.finish(metrics_);
 tasks_.stop();for(auto& t:workers_)if(t.joinable())t.join();
 if(recorder_){
  recorder_->finish();
  metrics_.record_dropped=recorder_->discarded()+recorder_->failures();
  metrics_.record_suppressed=recorder_->suppressed();
  metrics_.record_completed=recorder_->completed();
  metrics_.record_write_failed=recorder_->failures();
 }
 if(results_file_.is_open())results_file_.flush();
}
}
