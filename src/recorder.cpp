#include "sf/recorder.hpp"
namespace sf {
static constexpr char REC_MAGIC_V1[8]={'S','F','R','0','0','0','1','\n'};
static constexpr char REC_MAGIC_V2[8]={'S','F','R','0','0','0','2','\n'};
static constexpr uint64_t FOOTER_MARKER=UINT64_MAX;
static uint64_t feed_digest(uint64_t digest, const uint8_t* bytes, size_t length){
 for(size_t i=0;i<length;++i){digest^=bytes[i];digest*=1099511628211ULL;}
 return digest;
}
static uint64_t feed_int(uint64_t digest,uint64_t value,size_t n){
 for(size_t i=0;i<n;++i){const uint8_t b=uint8_t(value>>(8*i));digest=feed_digest(digest,&b,1);}
 return digest;
}
FlightRecorder::FlightRecorder(const std::string& root,size_t pre,size_t post,size_t max_mb,size_t max_incidents):root_(fs_path(root)),pre_limit_(pre),post_limit_(post),max_bytes_(max_mb*1024*1024),start_ns_(ns_now()){
 require(pre_limit_>0&&post_limit_>0&&max_mb>0&&max_mb<=1024&&max_incidents>0&&max_incidents<=256,"invalid recording limits");
 max_incidents_=max_incidents;
 std::filesystem::create_directories(root_);writer_=std::thread([this]{writer_loop();});
}
FlightRecorder::~FlightRecorder(){try{finish();}catch(const std::exception& e){std::cerr<<"recorder finish: "<<e.what()<<'\n';}}
void FlightRecorder::observe(const std::vector<uint8_t>& packet,uint64_t when){
 std::lock_guard<std::mutex> lk(mu_);
 // Late callers cannot re-open capture state after the writer is stopped.
 // Count the rejected datagram rather than silently losing evidence.
 if(stopping_){discarded_++;return;}
 // A single pathological caller cannot force unbounded capture memory.
 if(packet.size()>SFR_MAX_CAPTURE_DATAGRAM){discarded_++;return;}
 PacketRecord p{when>=start_ns_?when-start_ns_:0,packet};ring_bytes_+=p.wire.size();ring_.push_back(p);
 while(ring_.size()>pre_limit_||ring_bytes_>max_bytes_/2){ring_bytes_-=ring_.front().wire.size();ring_.pop_front();}
 if(active_){const size_t used=active_snapshot_.packet_bytes;
  const size_t overhead=8+24+16*(active_snapshot_.packets.size()+1);
  if(active_snapshot_.packets.size()<pre_limit_+post_limit_ && overhead<=max_bytes_ && used<=max_bytes_-overhead && p.wire.size()<=max_bytes_-overhead-used){
   active_snapshot_.packet_bytes+=p.wire.size();active_snapshot_.packets.push_back(std::move(p));
  }else {active_snapshot_.truncated=true;if(active_snapshot_.notes.size()<2048)active_snapshot_.notes.push_back({p.offset_ns,"capture_limit","packet omitted due to bounded recording",false});}
  if(post_left_>0)--post_left_;
  if(post_left_==0)enqueue_locked();}
}
void FlightRecorder::trigger(const std::string& reason){
 std::lock_guard<std::mutex> lk(mu_);
 if(stopping_){suppressed_++;return;}
 if(active_){
   if(active_snapshot_.notes.size()<2048)active_snapshot_.notes.push_back({ns_now()-start_ns_,"coalesced_trigger",reason,false});
   return;
 }
 if(accepted_>=max_incidents_){suppressed_++;return;}
 ++accepted_;
 active_=true;post_left_=post_limit_;active_snapshot_=Snapshot{};active_snapshot_.reason=reason;active_snapshot_.created_ms=epoch_ms();
 active_snapshot_.packets.assign(ring_.begin(),ring_.end());
 for(const auto& p:active_snapshot_.packets)active_snapshot_.packet_bytes+=p.wire.size();
 active_snapshot_.notes.push_back({ns_now()-start_ns_,"trigger",reason,false});
}
void FlightRecorder::note(const std::string& event,const std::string& detail,bool decision){
 std::lock_guard<std::mutex> lk(mu_);if(active_&&active_snapshot_.notes.size()<2048)active_snapshot_.notes.push_back({ns_now()-start_ns_,event,detail,decision});
}
void FlightRecorder::enqueue_locked(){
 if(!active_)return;
 active_=false;
 if(writing_.size()>=2){discarded_++;active_snapshot_=Snapshot{};return;}
 writing_.push_back(std::move(active_snapshot_));active_snapshot_=Snapshot{};cv_.notify_one();
}
void FlightRecorder::finish(){
 // All concurrent callers must wait for the same writer to finish.  A plain
 // stopping_ guard returned early for the second caller, before join().
 std::call_once(finish_once_,[this]{
  {
   std::lock_guard<std::mutex> lk(mu_);
   if(active_&&post_left_>0)active_snapshot_.truncated=true;
   enqueue_locked();
   stopping_=true;
   cv_.notify_all();
  }
  if(writer_.joinable())writer_.join();
 });
}
void FlightRecorder::writer_loop(){
 for(;;){Snapshot s;{std::unique_lock<std::mutex> lk(mu_);cv_.wait(lk,[this]{return stopping_||!writing_.empty();});if(writing_.empty()){if(stopping_)return;continue;}s=std::move(writing_.front());writing_.pop_front();}
  try{write_snapshot(s);completed_++;}catch(const std::exception& e){failures_++;std::cerr<<"recording write error: "<<e.what()<<std::endl;}}
}
void FlightRecorder::write_snapshot(const Snapshot& s){
 // Never reuse a previous incident directory; otherwise a restart in the
 // same millisecond could silently overwrite a previous diagnostic capture.
 std::filesystem::path target,working;
 for(int attempt=0;attempt<1000;++attempt){
  const auto candidate=root_/("incident_"+std::to_string(s.created_ms)+"_"+std::to_string(completed_.load()+1)+"_"+std::to_string(attempt));
  const auto unfinished=root_/ (".partial_"+candidate.filename().string());
  if(!std::filesystem::exists(candidate)&&std::filesystem::create_directory(unfinished)){
   target=candidate;working=unfinished;break;
  }
 }
 require(!target.empty(),"failed to allocate a unique recording directory");
 // Create every capture file in a non-public staging directory. A crash or
 // disk-full event leaves .partial_* rather than a misleading incident_*.
 std::ofstream packets(working/"packets.bin",std::ios::binary);require(bool(packets),"open packets.bin failed");
 packets.write(REC_MAGIC_V2,8);size_t stored=0;uint64_t bytes=8;
 uint64_t digest=14695981039346656037ULL;
 for(const auto& p:s.packets){
  const size_t size=p.wire.size();
  // Reserve 24 bytes for the mandatory end-of-file footer.
  if(bytes+16+size+24>max_bytes_)break;
  const auto checksum=crc32(p.wire.data(),p.wire.size());
  write_u64(packets,p.offset_ns);write_u32(packets,uint32_t(size));write_u32(packets,checksum);
  packets.write(reinterpret_cast<const char*>(p.wire.data()),std::streamsize(size));
  digest=feed_int(digest,p.offset_ns,8);digest=feed_int(digest,size,4);
  digest=feed_int(digest,checksum,4);digest=feed_digest(digest,p.wire.data(),size);
  stored++;bytes+=16+size;
 }
 write_u64(packets,FOOTER_MARKER);write_u64(packets,stored);write_u64(packets,digest);
 bytes+=24;
 packets.close();require(bool(packets),"record write incomplete");
 std::ofstream meta(working/"manifest.json");require(bool(meta),"cannot create recording manifest");
 meta<<"{\n  \"format\": \"SFR0002\",\n  \"created_epoch_ms\": "<<s.created_ms<<",\n  \"trigger\": \""<<json_escape(s.reason)<<"\",\n  \"packet_count\": "<<stored<<",\n  \"recorded_bytes\": "<<bytes<<",\n  \"complete_capture\": "<<(stored==s.packets.size()&&!s.truncated?"true":"false")<<"\n}\n";
 meta.close();require(bool(meta),"recording manifest write failed");
 auto dump=[&](const char* file,bool only_decisions){
  std::ofstream f(working/file);require(bool(f),std::string("cannot create ")+file);
  for(const auto& n:s.notes)if(!only_decisions||n.decision)
   f<<"{\"offset_ns\":"<<n.offset_ns<<","<<json_kv("event",n.event)<<","<<json_kv("detail",n.detail)<<"}\n";
  f.flush();require(bool(f),std::string("recording write failed: ")+file);
 };
 dump("events.jsonl",false);dump("decisions.jsonl",true);
 std::ofstream m(working/"metrics.json");require(bool(m),"cannot create recording metrics");
 m<<"{\"captured_packets\":"<<stored<<",\"recorded_events\":"<<s.notes.size()<<"}\n";
 m.close();require(bool(m),"recording metrics write failed");
 std::filesystem::rename(working,target);
 std::cout<<"RECORDING "<<path_utf8(target)<<" packets="<<stored<<std::endl;
}
CaptureReader::CaptureReader(const std::filesystem::path& filename):in_(filename,std::ios::binary){
 require(bool(in_),"cannot open replay: "+path_utf8(filename));char hdr[8];
 require(bool(in_.read(hdr,8)),"truncated recording magic");
 version_2_=std::memcmp(hdr,REC_MAGIC_V2,8)==0;
 require(version_2_||std::memcmp(hdr,REC_MAGIC_V1,8)==0,"bad recording magic");
}
bool CaptureReader::next(PacketRecord& rec){
 if(verified_footer_)return false;
 int v=in_.peek();
 if(v==EOF){
  require(!in_.bad(),"recording read failure");
  require(!version_2_,"recording footer missing (truncated at record boundary)");
  return false; // Legacy SFR0001 has no end-to-end footer.
 }
 uint64_t offset=0;
 require(read_u64(in_,offset),"truncated record offset");
 if(version_2_&&offset==FOOTER_MARKER){
  uint64_t count=0,expected_digest=0;
  require(read_u64(in_,count)&&read_u64(in_,expected_digest),"truncated recording footer");
  require(count==records_seen_&&expected_digest==digest_,"recording footer count/digest mismatch");
  require(in_.peek()==EOF&&!in_.bad(),"trailing bytes after recording footer");
  verified_footer_=true;return false;
 }
 uint32_t n=0,c=0;
 require(read_u32(in_,n)&&read_u32(in_,c),"truncated record header");
 require(n<=SFR_MAX_CAPTURE_DATAGRAM,"record size out of range");
 std::vector<uint8_t> b(n);
 require(bool(in_.read(reinterpret_cast<char*>(b.data()),n)),"truncated record body");
 require(crc32(b.data(),b.size())==c,"record integrity mismatch");
 if(version_2_){
  digest_=feed_int(digest_,offset,8);digest_=feed_int(digest_,n,4);
  digest_=feed_int(digest_,c,4);digest_=feed_digest(digest_,b.data(),b.size());
 }
 ++records_seen_;rec={offset,std::move(b)};return true;
}
}
