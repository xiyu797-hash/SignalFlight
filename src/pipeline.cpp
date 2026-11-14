#include "sf/pipeline.hpp"
namespace sf {
void Metrics::observe_latency_ns(uint64_t n){static constexpr uint64_t lim[]={100000,200000,500000,1000000,2000000,5000000,10000000,20000000,50000000,100000000,200000000,500000000,1000000000,5000000000ULL};size_t i=0;while(i<14&&n>lim[i])i++;lat_hist[i].fetch_add(1,std::memory_order_relaxed);
 std::lock_guard<std::mutex> lk(latency_mu_);
 latency_window_ns_[latency_next_]=n;
 latency_next_=(latency_next_+1)%kLatencyWindow;
 if(latency_used_<kLatencyWindow)++latency_used_;
}
std::string Metrics::json(size_t queue_size)const{
 auto v=[](const std::atomic<uint64_t>& a){return a.load();};
 uint64_t n=0;for(const auto& x:lat_hist)n+=v(x);
 static constexpr uint64_t lim[]={100,200,500,1000,2000,5000,10000,20000,50000,100000,200000,500000,1000000,5000000};
 uint64_t seen=0,p99=0;bool overflow=false;
 const uint64_t threshold=n/100*99+(n%100*99+99)/100;
 for(size_t i=0;i<15;i++){seen+=v(lat_hist[i]);if(n&&seen>=threshold){if(i<14)p99=lim[i];else overflow=true;break;}}
 // Exact quantiles over a bounded recent window. Copy under the lock, then
 // sort outside the lock so workers are not blocked by a reporting request.
 std::vector<uint64_t> latest;
 {
  std::lock_guard<std::mutex> lk(latency_mu_);
  latest.assign(latency_window_ns_.begin(),latency_window_ns_.begin()+latency_used_);
 }
 std::sort(latest.begin(),latest.end());
 auto quantile=[&](unsigned pct)->uint64_t{
  const size_t index=(latest.size()*pct+99)/100-1;
  const auto ns=latest[index];
  return ns/1000+(ns%1000!=0); // round up without overflowing uint64_t
 };
 std::ostringstream o;
 o<<"{\"packets_rx\":"<<v(packets_rx)<<",\"bytes_rx\":"<<v(bytes_rx)<<",\"bad_packets\":"<<v(bad_packets)
  <<",\"crc_errors\":"<<v(crc_errors)<<",\"duplicates\":"<<v(duplicates)<<",\"reordered\":"<<v(reordered)
  <<",\"missing_final\":"<<v(missing_final)<<",\"packets_too_old\":"<<v(packets_too_old)<<",\"frames_ready\":"<<v(frames_ready)
  <<",\"frames_expired\":"<<v(frames_expired)<<",\"frames_duplicate\":"<<v(frames_duplicate)
  <<",\"frames_rejected\":"<<v(frames_rejected)<<",\"analyzed_full\":"<<v(analyzed_full)
  <<",\"analyzed_light\":"<<v(analyzed_light)
  <<",\"analyzed_failed\":"<<v(analyzed_failed)<<",\"queue_dropped\":"<<v(queue_dropped)
  <<",\"queue_peak\":"<<v(queue_peak)<<",\"record_dropped\":"<<v(record_dropped)
  <<",\"record_suppressed\":"<<v(record_suppressed)<<",\"record_completed\":"<<v(record_completed)<<",\"record_write_failed\":"<<v(record_write_failed)
  <<",\"queue_size\":"<<queue_size<<",\"latency_samples\":"<<n<<",\"p99_latency_upper_us\":";
 if(n==0||overflow)o<<"null";else o<<p99;
 o<<",\"p99_latency_overflow\":"<<(overflow?"true":"false");
 o<<",\"latency_window_samples\":"<<latest.size()
  <<",\"latency_window_truncated\":"<<(n>kLatencyWindow?"true":"false")
  <<",\"p50_latency_us\":";
 if(latest.empty())o<<"null";else o<<quantile(50);
 o<<",\"p95_latency_us\":";
 if(latest.empty())o<<"null";else o<<quantile(95);
 o<<",\"p99_latency_us\":";
 if(latest.empty())o<<"null";else o<<quantile(99);
 o<<"}";return o.str();
}
void Metrics::print(size_t queue_size)const{std::cout<<"METRICS "<<json(queue_size)<<std::endl;}
SeqTracker::Outcome SeqTracker::accept(const Packet& p,uint64_t when,Metrics& m){
 StreamKey key{p.source_id,p.stream_id};
 if(!streams_.count(key) && streams_.size()>=max_streams_){m.frames_rejected++;return Outcome::Rejected;}
 auto& s=streams_[key];
 // A far older sequence is outside the bounded deduplication window. Reject it
 // rather than accidentally admitting a previously-seen packet after eviction.
 if(s.initialized&&p.seq<s.max_seq&&s.max_seq-p.seq>window_){m.packets_too_old++;return Outcome::TooOld;}
 // A gap that passed its deadline is a permanent loss decision: do not
 // later process it as valid while still counting it as irrecoverably lost.
 if(s.finalized.count(p.seq)){m.packets_too_old++;return Outcome::TooOld;}
 if(s.seen.count(p.seq)){m.duplicates++;return Outcome::Duplicate;}
 // Finalized-gap tombstones are needed only inside the active reorder window.
 for(auto it=s.finalized.begin();it!=s.finalized.end();){
  if(p.seq>*it && p.seq-*it>window_)it=s.finalized.erase(it);else ++it;
 }
 s.last_seen_ns=std::max(s.last_seen_ns,when);
 s.seen.insert(p.seq);s.order.push_back(p.seq);
 while(s.order.size()>window_*2){s.seen.erase(s.order.front());s.order.pop_front();}
 Outcome result=Outcome::New;
 if(!s.initialized){s.max_seq=p.seq;s.initialized=true;return result;}
 if(p.seq<s.max_seq){m.reordered++;s.gaps.erase(p.seq);result=Outcome::Reordered;}
 else if(p.seq>s.max_seq){
   const uint64_t low=s.max_seq+1,high=p.seq;
   const uint64_t first=high-low>window_?high-window_:low;
   if(first>low)m.missing_final.fetch_add(first-low);
   for(uint64_t missing=first;missing<high;++missing){
    if(s.gaps.size()>=window_){auto oldest=s.gaps.begin();for(auto it=s.gaps.begin();it!=s.gaps.end();++it)if(it->second<oldest->second)oldest=it;m.missing_final++;s.finalized.insert(oldest->first);s.gaps.erase(oldest);}
    s.gaps.emplace(missing,when);
   }
   s.max_seq=high;
 }
 return result;
}
void SeqTracker::expire(uint64_t when,Metrics& m){
 for(auto it=streams_.begin();it!=streams_.end();){
  auto& state=it->second;
  auto& gaps=state.gaps;
  for(auto missing=gaps.begin();missing!=gaps.end();){
   if(when>=missing->second&&when-missing->second>=timeout_ns_){
    m.missing_final++;state.finalized.insert(missing->first);missing=gaps.erase(missing);
   }else ++missing;
  }
  // Recycle idle stream slots so repeated source reconnects do not exhaust
  // the fixed per-stream state budget. Pending losses were finalized above.
  if(when>=state.last_seen_ns&&when-state.last_seen_ns>=stream_idle_ns_){
   m.missing_final+=state.gaps.size();it=streams_.erase(it);
  }else ++it;
 }
}
void SeqTracker::finish(Metrics& m){for(auto& s:streams_){m.missing_final+=s.second.gaps.size();s.second.gaps.clear();}}
void FrameAssembler::remember(const FrameKey& k){recent_.insert(k);recent_order_.push_back(k);if(recent_order_.size()>recent_limit_){recent_.erase(recent_order_.front());recent_order_.pop_front();}}
std::optional<Frame> FrameAssembler::accept(const Packet& p,uint64_t when,Metrics& m){
 // Public callers can construct Packets without wire decode; validate before indexing.
 if(!p.chunk_count || p.chunk_count>SFP_MAX_CHUNKS || p.chunk_index>=p.chunk_count ||
    p.format!=1 || p.flags!=0 || p.payload.empty() ||
    p.payload.size()>SFP_MAX_PAYLOAD || p.payload.size()%8!=0){m.frames_rejected++;return {};}
 FrameKey k{{p.source_id,p.stream_id},p.frame_id};
 if(recent_.count(k)){m.frames_duplicate++;return {};} // Each chunk contains at least one IQ sample; reject impossible metadata early.
 if(size_t(p.chunk_count)*8>max_frame_bytes_){m.frames_rejected++;remember(k);return {};}
 auto it=inflight_.find(k);
 if(it==inflight_.end()){
  if(inflight_.size()>=max_inflight_){auto old=inflight_.begin();for(auto cur=inflight_.begin();cur!=inflight_.end();++cur)if(cur->second.first_ns<old->second.first_ns)old=cur;m.frames_expired++;remember(old->first);inflight_.erase(old);}
  Entry e;e.count=p.chunk_count;e.first_ns=when;e.ts=p.source_ts_ns;e.chunks.resize(p.chunk_count);it=inflight_.emplace(k,std::move(e)).first;
 }
 auto& e=it->second;
 if(e.count!=p.chunk_count||e.ts!=p.source_ts_ns){m.frames_rejected++;return {};}
 if(!e.chunks[p.chunk_index].empty()){m.frames_duplicate++;return {};}
 if(p.payload.empty()){m.frames_rejected++;return {};}
 if(p.payload.size()>max_frame_bytes_-e.bytes){m.frames_rejected++;remember(k);inflight_.erase(it);return {};}
 e.bytes+=p.payload.size();e.chunks[p.chunk_index]=p.payload;e.received++;
 if(e.received!=e.count)return {};
 Frame f;f.source_id=p.source_id;f.stream_id=p.stream_id;f.id=p.frame_id;f.source_ts_ns=e.ts;f.received_ns=e.first_ns;
 size_t total=0;for(const auto& c:e.chunks)total+=c.size();f.iq.reserve(total);
 for(auto& c:e.chunks)f.iq.insert(f.iq.end(),c.begin(),c.end());
 remember(k);inflight_.erase(it);m.frames_ready++;return f;
}
void FrameAssembler::expire(uint64_t when,Metrics& m){for(auto it=inflight_.begin();it!=inflight_.end();){if(when>=it->second.first_ns&&when-it->second.first_ns>=timeout_ns_){m.frames_expired++;remember(it->first);it=inflight_.erase(it);}else ++it;}}
void FrameAssembler::finish(Metrics& m){m.frames_expired+=inflight_.size();inflight_.clear();}
Mode PressureController::update(size_t occ,size_t cap){const double load=double(occ)/std::max<size_t>(1,cap);switch(mode_){case Mode::Normal:if(load>=.90)mode_=Mode::Critical;else if(load>=.70)mode_=Mode::Degraded;break;case Mode::Degraded:if(load>=.90)mode_=Mode::Critical;else if(load<.50)mode_=Mode::Normal;break;case Mode::Critical:if(load<.65)mode_=Mode::Degraded;break;}return mode_;}
}
