#pragma once
#include "sf/pipeline.hpp"
namespace sf {
// A capture is a raw UDP datagram; invalid/short packets are diagnostically important.
constexpr size_t SFR_MAX_CAPTURE_DATAGRAM=65535;
struct PacketRecord {uint64_t offset_ns=0;std::vector<uint8_t> wire;};
class FlightRecorder {
 struct Note {uint64_t offset_ns=0;std::string event,detail;bool decision=false;};
 struct Snapshot {std::string reason;bool truncated=false;uint64_t created_ms=0;size_t packet_bytes=0;std::vector<PacketRecord> packets;std::vector<Note> notes;};
 std::filesystem::path root_;size_t pre_limit_,post_limit_,max_bytes_;uint64_t start_ns_;
 std::deque<PacketRecord> ring_;size_t ring_bytes_=0;bool active_=false;size_t post_left_=0;Snapshot active_snapshot_;
 std::mutex mu_;std::condition_variable cv_;std::once_flag finish_once_;std::deque<Snapshot> writing_;bool stopping_=false;size_t accepted_=0,max_incidents_=32;std::atomic<uint64_t> suppressed_{0};std::thread writer_;
 std::atomic<uint64_t> completed_{0},discarded_{0},failures_{0};
 void enqueue_locked();void writer_loop();void write_snapshot(const Snapshot& s);
public:
 FlightRecorder(const std::string& root,size_t pre=128,size_t post=128,size_t max_mb=16,size_t max_incidents=32);
 ~FlightRecorder();
 FlightRecorder(const FlightRecorder&)=delete;FlightRecorder& operator=(const FlightRecorder&)=delete;
 void observe(const std::vector<uint8_t>& packet,uint64_t when);
 void trigger(const std::string& reason);
 void note(const std::string& event,const std::string& detail,bool decision=false);
 void finish();
 uint64_t completed()const{return completed_.load();}
 uint64_t discarded()const{return discarded_.load();}
 uint64_t failures()const{return failures_.load();}
 uint64_t suppressed()const{return suppressed_.load();}
};
class CaptureReader {
 std::ifstream in_;
 bool verified_footer_=false, version_2_=false;
 uint64_t records_seen_=0, digest_=14695981039346656037ULL;
public:
 explicit CaptureReader(const std::filesystem::path& filename);
 bool has_integrity_footer() const{return version_2_;}
 bool next(PacketRecord& rec); // false only at verified SFR2 footer or legacy SFR1 EOF
};
}
