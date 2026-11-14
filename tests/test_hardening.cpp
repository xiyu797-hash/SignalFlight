// SignalFlight high-risk regression tests. No third-party test framework required.
#include "sf/engine.hpp"
#include <random>
#include <cstdio>
using namespace sf;
// A metrics object can be embedded in stack-allocated engines. Keep the
// bounded sample buffer off the stack, including on 1 MiB Windows stacks.
static_assert(sizeof(Metrics) < 4096, "Metrics must not reserve the latency window on the stack");
#define CHECK(expr) do { if(!(expr)) throw std::runtime_error(std::string("CHECK failed: ") + #expr + " at " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
static Packet mk(uint64_t sequence,uint64_t frame,uint32_t source=1) {
    Packet p; p.seq=sequence; p.frame_id=frame; p.source_id=source;
    p.source_ts_ns=123456; append_float_le(p.payload,1.f); append_float_le(p.payload,-0.5f);
    return p;
}
static bool throws(const std::function<void()>& fun) {
    try {fun(); return false;}catch(const std::exception&) {return true;}
}
int main(){
 try {
    // Known checksum test vector and strict protocol limits.
    {
        const char* example="123456789";
        CHECK(crc32(reinterpret_cast<const uint8_t*>(example),9)==0xcbf43926U);
        Packet p=mk(0,0); auto encoded=encode(p); Packet out;std::string error;
        CHECK(decode(encoded,out,error));
        auto bad=encoded;bad[49]=0;bad[48]=0; // declared payload length zero
        CHECK(!decode(bad,out,error));
        bad=encoded;bad[2]^=0xff;
        CHECK(!decode(bad,out,error));
        CHECK(throws([]{Packet empty; (void)encode(empty);}));
        CHECK(!decode(nullptr,0,out,error));
        CHECK(!decode(nullptr,SFP_HEADER_BYTES,out,error));
        CHECK(!error.empty());
        std::mt19937 random(901);
        for(int i=0;i<200;i++){
            Packet next=mk(uint64_t(i),uint64_t(i/3));
            next.payload.clear();
            const int n=1+int(random()%150);
            for(int j=0;j<n;j++){
                append_float_le(next.payload,float(random()%200)/100.f-1.f);
                append_float_le(next.payload,float(random()%200)/100.f-1.f);
            }
            auto wire=encode(next);CHECK(decode(wire,out,error));
            CHECK(out.seq==next.seq&&out.payload==next.payload);
            const size_t k=56+random()%next.payload.size(); wire[k]^=0x80;
            CHECK(!decode(wire,out,error)&&error=="crc mismatch");
        }
    }
    // Cross-check accelerated table CRC against the original bitwise reference
    // over empty, short, protocol-length and large buffers. This catches a
    // platform-specific polynomial/endianness mistake before benchmarking.
    {
        auto reference=[](const std::vector<uint8_t>& data){
            uint32_t c=0xFFFFFFFFU;
            for(uint8_t x:data){
                c^=x;
                for(int i=0;i<8;++i)c=(c>>1)^(0xEDB88320U & (0U-(c&1U)));
            }
            return ~c;
        };
        std::mt19937 rng(20261106);
        for(size_t size: {0U,1U,4U,8U,56U,128U,1076U,4096U,65535U}){
            std::vector<uint8_t> bytes(size);
            for(auto& b:bytes)b=uint8_t(rng());
            CHECK(crc32(bytes.data(),bytes.size())==reference(bytes));
        }
    }
    // Raw quantiles use a bounded recent window while legacy histogram
    // bucket and empty-sample behavior stay backwards compatible.
    {
        Metrics m;
        CHECK(m.json().find("\"p99_latency_us\":null")!=std::string::npos);
        for(uint64_t i=1;i<=100;++i)m.observe_latency_ns(i*1000);
        const auto j=m.json();
        CHECK(j.find("\"p50_latency_us\":50")!=std::string::npos);
        CHECK(j.find("\"p95_latency_us\":95")!=std::string::npos);
        CHECK(j.find("\"p99_latency_us\":99")!=std::string::npos);
        CHECK(j.find("\"latency_window_samples\":100")!=std::string::npos);
        for(uint64_t i=101;i<=16500;++i)m.observe_latency_ns(i*1000);
        const auto latest=m.json();
        CHECK(latest.find("\"latency_window_samples\":16384")!=std::string::npos);
        CHECK(latest.find("\"latency_window_truncated\":true")!=std::string::npos);
    }
    // Timeout milliseconds must not wrap when converted to nanoseconds.
    {
        const auto too_large=std::numeric_limits<uint64_t>::max()/1000000ULL+1;
        CHECK(throws([&]{SeqTracker tracker(16,too_large);}));
        CHECK(throws([&]{SeqTracker tracker(16,1,too_large);}));
        CHECK(throws([&]{FrameAssembler assembler(2,too_large);}));
        CHECK(throws([]{FrameAssembler assembler(2,0);}));
        CHECK(!throws([&]{SeqTracker tracker(16,too_large-1,too_large-1);}));
        CHECK(!throws([&]{FrameAssembler assembler(2,too_large-1);}));
    }
    {
        Metrics huge;
        huge.observe_latency_ns(std::numeric_limits<uint64_t>::max());
        CHECK(huge.json().find("\"p99_latency_us\":18446744073709552")!=std::string::npos);
    }
    // Old packets beyond the deduplication window must not be processed as new.
    {
        Metrics m; SeqTracker seq(4,100);
        CHECK(seq.accept(mk(1,1),100,m)==SeqTracker::Outcome::New);
        CHECK(seq.accept(mk(10,10),200,m)==SeqTracker::Outcome::New);
        CHECK(m.missing_final==4); // 2..5 finalized; 6..9 still pending
        CHECK(seq.accept(mk(2,2),300,m)==SeqTracker::Outcome::TooOld);
        CHECK(m.packets_too_old==1);
        CHECK(seq.accept(mk(9,9),400,m)==SeqTracker::Outcome::Reordered);
        seq.finish(m);CHECK(m.missing_final==7);
    }
    // A timeout-based "final loss" must stay final even if the packet
    // appears later while still inside the sequence reordering window.
    {
        Metrics m;SeqTracker tracker(16,1);
        CHECK(tracker.accept(mk(10,10),100,m)==SeqTracker::Outcome::New);
        CHECK(tracker.accept(mk(12,12),200,m)==SeqTracker::Outcome::New);
        tracker.expire(1000200,m);
        CHECK(m.missing_final==1);
        CHECK(tracker.accept(mk(11,11),1000300,m)==SeqTracker::Outcome::TooOld);
        CHECK(m.missing_final==1&&m.packets_too_old==1);
    }
    // Expiration, conflicting frame metadata, and zero-capacity misuse.
    {
        CHECK(throws([]{FrameAssembler assembler(0);}));
        Metrics m;FrameAssembler assembler(2,1);
        auto p=mk(0,42);p.chunk_count=2;p.chunk_index=0;
        CHECK(!assembler.accept(p,100,m));
        auto mismatch=mk(1,42);mismatch.chunk_count=3;mismatch.chunk_index=1;
        CHECK(!assembler.accept(mismatch,200,m)&&m.frames_rejected==1);
        assembler.expire(10000000,m);CHECK(m.frames_expired==1);
        CHECK(throws([]{BoundedQueue<int> q(0);}));
    }
    // Public assembler entry must refuse malformed Packet objects even when
    // callers bypass wire decode; rejection must not poison later frames.
    {
        Metrics m; FrameAssembler assembler(2,100);
        auto invalid=mk(0,11); invalid.chunk_count=0;
        CHECK(!assembler.accept(invalid,100,m));
        invalid.chunk_count=1; invalid.chunk_index=1;
        CHECK(!assembler.accept(invalid,101,m));
        invalid.chunk_count=SFP_MAX_CHUNKS+1; invalid.chunk_index=0;
        CHECK(!assembler.accept(invalid,102,m));
        invalid.chunk_count=1; invalid.format=2;
        CHECK(!assembler.accept(invalid,102,m));
        invalid.format=1; invalid.flags=1;
        CHECK(!assembler.accept(invalid,102,m));
        invalid.flags=0; invalid.payload={1};
        CHECK(!assembler.accept(invalid,102,m));
        invalid.payload=std::vector<uint8_t>(SFP_MAX_PAYLOAD+8,0);
        CHECK(!assembler.accept(invalid,102,m));
        CHECK(m.frames_rejected==7);
        auto valid=mk(1,11);
        CHECK(assembler.accept(valid,103,m).has_value());
    }
    // Validate the optimized FFT numerically against the original reference.
    {
        std::mt19937 random(77);std::uniform_real_distribution<float> dist(-1.0f,1.0f);
        DspAnalyzer fft,reference(true);
        for(int samples : {1,3,16,63,64,65,1024}){
            for(int trial=0;trial<24;++trial){
                Frame f;
                for(int i=0;i<samples;i++){
                    append_float_le(f.iq,dist(random));append_float_le(f.iq,dist(random));
                }
                auto fast=fft.analyze(f,false),slow=reference.analyze(f,false);
                CHECK(std::abs(fast.mean_power-slow.mean_power)<1e-12);
                CHECK(std::abs(fast.spectral_centroid-slow.spectral_centroid)<1e-11);
                CHECK(std::abs(fft.analyze(f,true).mean_power-fast.mean_power)<1e-12);
            }
        }
        Frame invalid;append_float_le(invalid.iq,std::numeric_limits<float>::quiet_NaN());append_float_le(invalid.iq,0);
        CHECK(throws([&]{fft.analyze(invalid,false);}));
    }
    // Concurrent producers/consumers must process each item exactly once.
    {
        constexpr int producers=4,per_producer=600,total=producers*per_producer;
        BoundedQueue<int> queue(17);
        std::vector<std::atomic<int>> seen(total);
        for(auto& n:seen)n=0;
        std::vector<std::thread> consumers,senders;
        for(int c=0;c<3;c++)consumers.emplace_back([&]{int val;while(queue.pop(val)){if(val>=0&&val<total)seen[val]++;}});
        for(int p=0;p<producers;p++)senders.emplace_back([&,p]{
            for(int k=0;k<per_producer;k++){
                const int value=p*per_producer+k;
                while(!queue.try_push(value))std::this_thread::yield();
            }
        });
        for(auto& t:senders)t.join();
        queue.stop();
        for(auto& t:consumers)t.join();
        for(auto& v:seen)CHECK(v==1);
    }
    // The Engine ingress boundary is serialized even when several producer
    // threads call ingest() concurrently. Drain on close, no lost frames.
    {
        EngineConfig cfg;cfg.workers=4;cfg.queue_capacity=512;cfg.frame_samples=1;
        Engine e(cfg);std::vector<std::thread> senders;
        for(uint32_t source=1;source<=4;source++)senders.emplace_back([&,source]{
            for(uint64_t k=0;k<80;k++)e.ingest(encode(mk(k,k,source)));
        });
        for(auto& t:senders)t.join();
        e.close();
        e.close();
        CHECK(e.metrics().frames_ready==320);
        CHECK(e.metrics().analyzed_full==320);
        CHECK(e.metrics().queue_dropped==0);
        CHECK(e.metrics().analyzed_failed==0);
    }
    // A corrupt or truncated recorder file must fail hard rather than silently
    // replaying fewer packets or treating the data as valid.
    {
        const auto base=std::filesystem::temp_directory_path()/std::filesystem::path("sf_hardening_"+std::to_string(epoch_ms())+"_"+std::to_string(ns_now()%1000000));
        {
            FlightRecorder recorder(path_utf8(base),2,2);
            recorder.trigger("hardening");
            recorder.observe(encode(mk(0,0)),ns_now());
            recorder.observe(encode(mk(1,1)),ns_now());
            recorder.finish();CHECK(recorder.completed()==1&&recorder.failures()==0);
        }
        std::filesystem::path recording;
        for(const auto& entry:std::filesystem::directory_iterator(base))recording=entry.path()/"packets.bin";
        CHECK(!recording.empty());
        auto bytes=std::vector<uint8_t>{};
        {std::ifstream in(recording,std::ios::binary);bytes.assign(std::istreambuf_iterator<char>(in),{});}
        CHECK(bytes.size()>40);
        {
            CaptureReader reader(recording);PacketRecord r;CHECK(reader.next(r));CHECK(reader.next(r));CHECK(!reader.next(r));
        }
        auto damaged=base/"bad.bin",truncated=base/"truncated.bin",boundary=base/"boundary.bin",footer_corrupt=base/"footer_corrupt.bin";
        auto modified=bytes;modified[35]^=0x20;
        {std::ofstream f(damaged,std::ios::binary);f.write(reinterpret_cast<const char*>(modified.data()),std::streamsize(modified.size()));}
        {std::ofstream f(truncated,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()-5));}
        // Remove the entire footer: the old SFR0001 implementation treated this
        // aligned truncation as a normal EOF. New SFR0002 MUST fail closed.
        {std::ofstream f(boundary,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()-24));}
        auto bad_footer=bytes;bad_footer.back()^=0xFF;
        {std::ofstream f(footer_corrupt,std::ios::binary);f.write(reinterpret_cast<const char*>(bad_footer.data()),std::streamsize(bad_footer.size()));}
        CHECK(throws([&]{CaptureReader reader(damaged);PacketRecord r;reader.next(r);}));
        CHECK(throws([&]{CaptureReader reader(truncated);PacketRecord r;while(reader.next(r)){};}));
        CHECK(throws([&]{CaptureReader reader(boundary);PacketRecord r;while(reader.next(r)){};}));
        CHECK(throws([&]{CaptureReader reader(footer_corrupt);PacketRecord r;while(reader.next(r)){};}));
        std::filesystem::remove_all(base);
    }
    // Missing data is null, not a fake zero p99; overflow is not clamped to 5s.
    {
        Metrics m;auto j=m.json();CHECK(j.find("\"p99_latency_upper_us\":null")!=std::string::npos);
        m.observe_latency_ns(6000000000ULL);
        j=m.json();CHECK(j.find("\"p99_latency_overflow\":true")!=std::string::npos);
    }
    // Repeated source sessions must be independent and idle sessions must be
    // reclaimed rather than exhausting 256 tracker slots forever.
    {
        Metrics m; SeqTracker seq(8,1,10);
        for (uint32_t source=1; source<=256; ++source) {
            Packet p=mk(0,0,source);
            CHECK(seq.accept(p,1000000,m)==SeqTracker::Outcome::New);
        }
        CHECK(seq.accept(mk(0,0,257),1000001,m)==SeqTracker::Outcome::Rejected);
        seq.expire(12000000,m);
        CHECK(seq.accept(mk(0,0,257),12000001,m)==SeqTracker::Outcome::New);
        Packet second=mk(0,0,257); second.stream_id=2;
        CHECK(seq.accept(second,12000002,m)==SeqTracker::Outcome::New);
    }
    // Frame assembly must enforce an exact memory budget, including deliberately
    // contradictory chunk metadata. It must not poison later healthy frames.
    {
        Metrics m; FrameAssembler assembler(4,100,16);
        Packet invalid=mk(0,1);invalid.chunk_count=3;
        CHECK(!assembler.accept(invalid,100,m));
        CHECK(m.frames_rejected==1);
        Packet p=mk(1,2);p.chunk_count=2;
        CHECK(!assembler.accept(p,200,m));
        Packet q=mk(2,2);q.chunk_count=2;q.chunk_index=1;
        CHECK(assembler.accept(q,201,m).has_value());
        CHECK(m.frames_ready==1);
        Packet oversized=mk(3,3);oversized.payload.insert(oversized.payload.end(),16,0);
        CHECK(!assembler.accept(oversized,300,m));
        CHECK(m.frames_rejected>=2);
    }
    // Unknown protocol flags and reserved bytes must not be interpreted as
    // valid data, even when the packet has a deliberately recomputed CRC.
    {
        Packet p=mk(4,4);p.flags=1;
        CHECK(throws([&]{(void)encode(p);}));
        p.flags=0;
        auto wire=encode(p);wire[50]=3;
        for(int i=52;i<56;i++)wire[size_t(i)]=0;
        uint32_t c=crc32(wire.data(),wire.size());
        for(int i=0;i<4;i++)wire[size_t(52+i)]=uint8_t(c>>(8*(3-i)));
        Packet decoded;std::string err;
        CHECK(!decode(wire,decoded,err));
    }
    // Forensic recordings must preserve malformed datagrams: a short payload
    // and an oversized UDP payload are just as important as valid packets.
    {
        const auto base=std::filesystem::temp_directory_path()/std::filesystem::path("sf_forensics_"+std::to_string(ns_now()));
        {
            FlightRecorder recorder(path_utf8(base),3,3);
            recorder.trigger("bad_udp_investigation");
            recorder.observe(std::vector<uint8_t>{0x11,0x22,0x33},ns_now());
            recorder.observe(std::vector<uint8_t>(1500,0x42),ns_now());
            recorder.finish();CHECK(recorder.completed()==1&&recorder.failures()==0);
        }
        std::filesystem::path file;
        for(const auto& entry:std::filesystem::directory_iterator(base))file=entry.path()/"packets.bin";
        CHECK(!file.empty());
        // Scope the CaptureReader so its std::ifstream releases the native
        // file handle BEFORE the temporary directory is removed. Linux can
        // unlink open files, but Windows will refuse (sharing violation).
        {
            CaptureReader reader(file);
            PacketRecord a,b;
            CHECK(reader.next(a)&&a.wire==std::vector<uint8_t>({0x11,0x22,0x33}));
            CHECK(reader.next(b)&&b.wire==std::vector<uint8_t>(1500,0x42));
            CHECK(!reader.next(a));
        }
        CHECK(std::filesystem::remove_all(base)>0);
        CHECK(!std::filesystem::exists(base));
    }

    // Recorder incident limits must not masquerade as packet loss or a
    // successful persisted snapshot: explicit suppressed-trigger accounting.
    {
        const auto base=std::filesystem::temp_directory_path()/std::filesystem::path("sf_recorder_limit_"+std::to_string(ns_now()));
        {
            FlightRecorder recorder(path_utf8(base),2,1,4,2);
            for(int i=0;i<3;++i){
                recorder.trigger("unique_incident_"+std::to_string(i));
                recorder.observe(encode(mk(uint64_t(i),uint64_t(i))),ns_now());
            }
            recorder.finish();
            CHECK(recorder.completed()==2);
            CHECK(recorder.suppressed()==1);
            CHECK(recorder.discarded()==0 && recorder.failures()==0);
        }
        size_t completed_folders=0;
        for(const auto& item:std::filesystem::directory_iterator(base))
            if(item.is_directory()&&item.path().filename().string().rfind("incident_",0)==0)++completed_folders;
        CHECK(completed_folders==2);
        std::filesystem::remove_all(base);
    }
    // Race all shutdown callers against the same recorder writer. Every
    // finish() call MUST return only after the recording is published.
    // On Windows this also guards against deleting a still-open packets.bin.
    {
        const auto base=std::filesystem::temp_directory_path()/
            std::filesystem::path("sf_concurrent_finish_"+std::to_string(ns_now()));
        {
            FlightRecorder recorder(path_utf8(base),2,2,4,4);
            recorder.trigger("concurrent_close");
            recorder.observe(encode(mk(0,0)),ns_now());
            recorder.observe(encode(mk(1,1)),ns_now());
            std::vector<std::thread> closers;
            for(int n=0;n<8;++n)closers.emplace_back([&recorder]{recorder.finish();});
            for(auto& t:closers)t.join();
            CHECK(recorder.completed()==1);
            CHECK(recorder.failures()==0);
            recorder.finish();
            // A close/ingress race should reject late arrivals with explicit
            // accounting, never silently mutate a finished capture.
            recorder.observe(encode(mk(99,99)),ns_now());
            recorder.trigger("after_close");
            CHECK(recorder.discarded()==1);
            CHECK(recorder.suppressed()==1);
        }
        size_t incidents=0;
        for(const auto& d:std::filesystem::directory_iterator(base)){
            if(d.path().filename().string().rfind("incident_",0)!=0)continue;
            ++incidents;
            CaptureReader reader(d.path()/"packets.bin");
            PacketRecord r;uint64_t count=0;
            while(reader.next(r))++count;
            CHECK(count==2);
        }
        CHECK(incidents==1);
        CHECK(std::filesystem::remove_all(base)>0);
    }
    // Empty / small latency histories must not underflow quantile indices.
    {
        Metrics m;
        auto j=m.json();
        CHECK(j.find("\"p50_latency_us\":null")!=std::string::npos);
        m.observe_latency_ns(1234567);
        j=m.json();
        CHECK(j.find("\"p50_latency_us\":1235")!=std::string::npos);
        CHECK(j.find("\"p95_latency_us\":1235")!=std::string::npos);
    }
    // Continuously fuzz the binary decoder with malformed length, payload,
    // control fields and random corruption. Decoding must never throw/crash.
    {
        std::mt19937 rng(20261105);
        auto valid=encode(mk(1,1));
        Packet decoded;std::string err;
        for(int iter=0;iter<10000;++iter){
            auto b=valid;
            const size_t flips=1+rng()%4;
            for(size_t n=0;n<flips;n++)b[size_t(rng()%b.size())]^=uint8_t(1+rng()%255);
            if(iter%5==0)b.resize(size_t(rng()%140));
            if(iter%7==0)b.insert(b.end(),size_t(rng()%64),0xaa);
            (void)decode(b,decoded,err);
        }
    }
    // Very large finite input remains valid under double accumulation;
    // nonfinite payloads are explicitly refused instead of producing JSON NaN.
    {
        Frame f;const auto huge=std::numeric_limits<float>::max();
        append_float_le(f.iq,huge);append_float_le(f.iq,huge);
        CHECK(std::isfinite(DspAnalyzer().analyze(f,false).mean_power));
        f.iq.clear();append_float_le(f.iq,std::numeric_limits<float>::infinity());
        append_float_le(f.iq,0.f);
        CHECK(throws([&]{(void)DspAnalyzer().analyze(f,false);}));
    }
    // Stress repeated source reconnects while the receiver stays online.
    // Sessions reuse seq/frame IDs, but stream IDs disambiguate them.
    {
        EngineConfig cfg;cfg.frame_samples=1;cfg.queue_capacity=512;cfg.workers=3;
        Engine engine(cfg);
        for(int session=1;session<=4;++session)
            for(uint64_t j=0;j<30;++j){
                auto p=mk(j,j);p.stream_id=uint32_t(session);
                engine.ingest(encode(p));
            }
        engine.close();
        CHECK(engine.metrics().frames_ready==120);
        CHECK(engine.metrics().analyzed_full==120);
        CHECK(engine.metrics().frames_duplicate==0);
    }
    std::cout<<"test_hardening PASS (protocol, FFT, concurrent shutdown, recorder, metrics)\n";
    return 0;
 }catch(const std::exception& ex){std::cerr<<"test_hardening FAIL: "<<ex.what()<<std::endl;return 1;}
}
