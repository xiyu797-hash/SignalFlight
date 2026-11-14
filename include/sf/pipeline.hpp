#pragma once
#include "sf/protocol.hpp"
namespace sf {
inline uint64_t checked_timeout_ns(uint64_t ms){
 require(ms>0 && ms<=std::numeric_limits<uint64_t>::max()/1000000ULL,"timeout out of range");
 return ms*1000000ULL;
}
struct Frame {uint32_t source_id=0,stream_id=0;uint64_t id=0,source_ts_ns=0,received_ns=0;std::vector<uint8_t> iq;};
struct Metrics {
 std::atomic<uint64_t> packets_rx{0},bytes_rx{0},bad_packets{0},crc_errors{0},duplicates{0},reordered{0},missing_final{0},packets_too_old{0};
 std::atomic<uint64_t> frames_ready{0},frames_expired{0},frames_duplicate{0},frames_rejected{0};
 std::atomic<uint64_t> analyzed_full{0},analyzed_light{0},analyzed_failed{0},queue_dropped{0},queue_peak{0},record_dropped{0},record_suppressed{0},record_completed{0},record_write_failed{0};
 std::array<std::atomic<uint64_t>,15> lat_hist{};
 // Fixed-memory exact microsecond quantiles of the latest samples. Separate
 // from the lifetime histogram; the bounded sliding window never grows.
 static constexpr size_t kLatencyWindow=16384;
 mutable std::mutex latency_mu_;
 std::vector<uint64_t> latency_window_ns_ = std::vector<uint64_t>(kLatencyWindow);
 size_t latency_next_=0,latency_used_=0;
 void observe_latency_ns(uint64_t n);
 std::string json(size_t queue_size=0)const;
 void print(size_t queue_size=0)const;
};
struct StreamKey {uint32_t source=0,stream=0;bool operator==(const StreamKey& o)const{return source==o.source&&stream==o.stream;}};
struct StreamKeyHash {size_t operator()(const StreamKey& k)const{return (uint64_t(k.source)<<32)^k.stream;}};
struct FrameKey {StreamKey s;uint64_t id;bool operator==(const FrameKey& o)const{return s==o.s&&id==o.id;}};
struct FrameKeyHash {size_t operator()(const FrameKey& k)const{return StreamKeyHash{}(k.s) ^ (std::hash<uint64_t>{}(k.id)*0x9e3779b97f4a7c15ULL);}};
class SeqTracker {
 struct State {bool initialized=false;uint64_t max_seq=0,last_seen_ns=0;std::unordered_set<uint64_t> seen;std::deque<uint64_t> order;std::unordered_map<uint64_t,uint64_t> gaps;std::unordered_set<uint64_t> finalized;};
 std::unordered_map<StreamKey,State,StreamKeyHash> streams_;size_t max_streams_=256;
 size_t window_;uint64_t timeout_ns_,stream_idle_ns_;
public:
 explicit SeqTracker(size_t window=4096,uint64_t timeout_ms=400,uint64_t stream_idle_ms=300000):window_(window),timeout_ns_(checked_timeout_ns(timeout_ms)),stream_idle_ns_(checked_timeout_ns(stream_idle_ms)){require(window>0&&window<=65536,"invalid sequence tracker limits");}
 enum class Outcome{New,Duplicate,Reordered,TooOld,Rejected};
 Outcome accept(const Packet& p,uint64_t when,Metrics& m);
 void expire(uint64_t when,Metrics& m);
 void finish(Metrics& m);
};
class FrameAssembler {
 struct Entry {uint16_t count=0;size_t received=0,bytes=0;uint64_t first_ns=0,ts=0;std::vector<std::vector<uint8_t>> chunks;};
 std::unordered_map<FrameKey,Entry,FrameKeyHash> inflight_;
 std::unordered_set<FrameKey,FrameKeyHash> recent_;std::deque<FrameKey> recent_order_;
 size_t max_inflight_,recent_limit_,max_frame_bytes_;uint64_t timeout_ns_;
 void remember(const FrameKey& k);
public:
 FrameAssembler(size_t max_inflight=1024,uint64_t timeout_ms=500,size_t max_frame_bytes=16384*8):max_inflight_(max_inflight),recent_limit_(8192),max_frame_bytes_(max_frame_bytes),timeout_ns_(checked_timeout_ns(timeout_ms)){require(max_inflight>0&&max_frame_bytes>0,"invalid frame capacity");}
 std::optional<Frame> accept(const Packet& p,uint64_t when,Metrics& m);
 void expire(uint64_t when,Metrics& m);
 void finish(Metrics& m);
};
template<typename T>class BoundedQueue {
 mutable std::mutex mu_;std::condition_variable cv_;std::deque<T> q_;size_t cap_;bool stopped_=false;
public:
 explicit BoundedQueue(size_t cap):cap_(cap){require(cap>0,"queue capacity must be positive");}
 bool try_push(T t,size_t* level=nullptr){
  {std::lock_guard<std::mutex> lk(mu_);if(stopped_||q_.size()>=cap_)return false;q_.push_back(std::move(t));if(level)*level=q_.size();}
  cv_.notify_one();return true;
 }
 bool pop(T& out){std::unique_lock<std::mutex> lk(mu_);cv_.wait(lk,[this]{return stopped_||!q_.empty();});if(q_.empty())return false;out=std::move(q_.front());q_.pop_front();return true;}
 size_t size()const{std::lock_guard<std::mutex> lk(mu_);return q_.size();}
 size_t capacity()const{return cap_;}
 void stop(){{std::lock_guard<std::mutex> lk(mu_);stopped_=true;}cv_.notify_all();}
};
enum class Mode: uint8_t{Normal=0,Degraded=1,Critical=2};
inline const char* mode_name(Mode m){return m==Mode::Normal?"normal":m==Mode::Degraded?"degraded":"critical";}
class PressureController {
 Mode mode_=Mode::Normal;
public:
 Mode update(size_t occupancy,size_t capacity);
 Mode mode()const{return mode_;}
};
struct AnalysisResult {double mean_power=0,spectral_centroid=0;};
class IAnalyzer {public:virtual ~IAnalyzer()=default;virtual AnalysisResult analyze(const Frame& f,bool light)=0;};
class DspAnalyzer:public IAnalyzer {
 bool reference_dft_=false;
public:
 explicit DspAnalyzer(bool reference_dft=false):reference_dft_(reference_dft){}
 AnalysisResult analyze(const Frame& f,bool light)override;
};
// dsp_backend: "fft" (default), or "reference" for a deliberately slower comparison.
std::unique_ptr<IAnalyzer> make_analyzer(const std::string& dsp_backend);
}
