#include "sf/engine.hpp"
#include <stdexcept>
#define CHECK(x) do { if(!(x)) throw std::runtime_error("CHECK failed: " #x); } while(0)
#include <iostream>
using namespace sf;
static Packet packet(uint64_t seq,uint64_t frame,uint16_t chunk,uint16_t count){Packet p;p.source_id=2;p.seq=seq;p.frame_id=frame;p.chunk_index=chunk;p.chunk_count=count;p.source_ts_ns=12345;append_float_le(p.payload,1.0f);append_float_le(p.payload,0.0f);return p;}
int main(){
 try{
  {auto p=packet(42,10,0,1);auto wire=encode(p);CHECK(wire.size()==64);Packet decoded;std::string error;CHECK(decode(wire,decoded,error));CHECK(decoded.seq==42&&decoded.source_id==2&&decoded.payload==p.payload);wire.back()^=0xff;CHECK(!decode(wire,decoded,error)&&error=="crc mismatch");}
  {Metrics m;SeqTracker seq;auto p1=packet(1,0,0,1),p3=packet(3,2,0,1),p2=packet(2,1,0,1);CHECK(seq.accept(p1,100,m)==SeqTracker::Outcome::New);CHECK(seq.accept(p3,200,m)==SeqTracker::Outcome::New);CHECK(seq.accept(p2,300,m)==SeqTracker::Outcome::Reordered);CHECK(seq.accept(p2,400,m)==SeqTracker::Outcome::Duplicate);seq.finish(m);CHECK(m.reordered==1&&m.duplicates==1&&m.missing_final==0);}
  {Metrics m;SeqTracker seq;seq.accept(packet(0,0,0,1),100,m);seq.accept(packet(2,1,0,1),200,m);seq.expire(500000000ULL,m);CHECK(m.missing_final==1);}
  {Metrics m;FrameAssembler f;auto last=f.accept(packet(8,4,1,2),10,m);CHECK(!last);last=f.accept(packet(7,4,0,2),20,m);CHECK(last&&last->iq.size()==16&&last->id==4&&m.frames_ready==1);last=f.accept(packet(7,4,0,2),30,m);CHECK(!last&&m.frames_duplicate==1);}
  {Metrics m;FrameAssembler f(2,1);f.accept(packet(1,1,0,2),100,m);f.expire(2000000,m);CHECK(m.frames_expired==1);}
  {BoundedQueue<int> q(1);size_t depth=0;CHECK(q.try_push(7,&depth)&&depth==1);CHECK(!q.try_push(9));int v=0;CHECK(q.pop(v)&&v==7);q.stop();CHECK(!q.pop(v));}
  {PressureController c;CHECK(c.update(72,100)==Mode::Degraded);CHECK(c.update(91,100)==Mode::Critical);CHECK(c.update(60,100)==Mode::Degraded);CHECK(c.update(40,100)==Mode::Normal);}
  {Frame f;append_float_le(f.iq,1.f);append_float_le(f.iq,0.f);auto result=DspAnalyzer().analyze(f,false);CHECK(std::abs(result.mean_power-1.)<1e-6);}
  {const auto folder=std::filesystem::temp_directory_path()/("sf_test_"+std::to_string(epoch_ms()));
   {FlightRecorder r(path_utf8(folder),2,2);r.trigger("test");for(int i=0;i<3;i++)r.observe(encode(packet(i,i,0,1)),ns_now());r.finish();CHECK(r.completed()==1&&r.failures()==0);}
   bool found=false;for(auto& x:std::filesystem::directory_iterator(folder)){CaptureReader r(x.path()/"packets.bin");PacketRecord p;int n=0;while(r.next(p))n++;CHECK(n==2);found=true;}CHECK(found);std::filesystem::remove_all(folder);
  }
  {EngineConfig c;c.replay=true;c.frame_samples=1;Engine e(c);e.ingest(encode(packet(0,1,0,1)),1000000000ULL);e.close();CHECK(e.metrics().frames_ready==1&&e.metrics().analyzed_full==1);}
  std::cout<<"test_core PASS (protocol, CRC, seq, reorder, frames, queue, pressure, DSP, recorder, replay)\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<"test_core FAIL: "<<e.what()<<std::endl;return 1;}
}
