#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sf/engine.hpp"
#include "sf/net.hpp"
#include <csignal>
#ifndef _WIN32
#include <sys/epoll.h>
#include <sys/uio.h>
#endif
using namespace sf;
static volatile std::sig_atomic_t interrupted=0;
static void on_sig(int){interrupted=1;}
static void help(){std::cout<<"sf_engine options:\n --host 0.0.0.0 --port 9009 --duration 10 --workers 2 --queue 256\n --work-us 0 --frame-samples 1024 --timeout-ms 500 --batch\n --record-dir recordings --capture-start --max-incidents 32\n --dsp-backend fft|reference (default fft)\n --results-out results/analysis.jsonl --metrics-out metrics.json\n --stop-file stop.flag --ready-file ready.flag (cross-platform automation)\n";}
int main(int argc,char** argv){
 try{
 Args a(argc,argv);if(a.flag("help")){help();return 0;}
 require(!a.has("onnx-model"),"Systems Edition does not support --onnx-model");
 EngineConfig c;c.workers=a.integer("workers",2);int queue=a.integer("queue",256);require(queue>0&&queue<=1000000,"invalid queue capacity");c.queue_capacity=size_t(queue);
 int timeout=a.integer("timeout-ms",500),seq_timeout=a.integer("sequence-timeout-ms",400);
 require(timeout>0&&seq_timeout>0,"invalid timeouts");c.timeout_ms=uint64_t(timeout);c.sequence_timeout_ms=uint64_t(seq_timeout);
 c.work_us=a.integer("work-us",0);c.frame_samples=a.integer("frame-samples",1024);
 c.max_incidents=a.integer("max-incidents",32);c.record_dir=a.get("record-dir");c.results_out=a.get("results-out");c.capture_start=a.flag("capture-start");c.dsp_backend=a.get("dsp-backend","fft");
 int port=a.integer("port",9009),duration=a.integer("duration",10);require(port>0&&port<=65535&&duration>=0,"invalid port / duration");
 const bool batch=a.flag("batch");const std::string stop_file=a.get("stop-file"),ready_file=a.get("ready-file");
 [[maybe_unused]] net::Runtime network;
 net::OwnedSocket sock(::socket(AF_INET,SOCK_DGRAM,0));net::require_socket(sock.get(),"socket");
 require(net::nonblocking(sock.get()),"cannot set nonblocking mode");
 int recvbuf=4*1024*1024;
 ::setsockopt(sock.get(),SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&recvbuf),sizeof(recvbuf));
 sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(uint16_t(port));
 require(inet_pton(AF_INET,a.get("host","0.0.0.0").c_str(),&addr.sin_addr)==1,"invalid bind IPv4 address");
 require(::bind(sock.get(),reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"bind failed: port may be in use (error="+std::to_string(net::last_error())+")");
#ifndef _WIN32
 int ep=epoll_create1(EPOLL_CLOEXEC);require(ep>=0,"epoll_create1 failed");
 epoll_event add{};add.events=EPOLLIN;add.data.fd=sock.get();require(epoll_ctl(ep,EPOLL_CTL_ADD,sock.get(),&add)==0,"epoll_ctl failed");
#endif
 Engine engine(c);std::signal(SIGINT,on_sig);
#ifndef _WIN32
 std::signal(SIGTERM,on_sig);
#endif
 std::cout<<"LISTEN "<<a.get("host","0.0.0.0")<<":"<<port<<" workers="<<c.workers<<" queue="<<c.queue_capacity<<" batch="<<batch<<" platform="<<
#ifdef _WIN32
 "winsock-select"
#else
 "linux-epoll"
#endif
 <<std::endl;
 if(!ready_file.empty()){
   auto parent=fs_path(ready_file).parent_path();
   if(!parent.empty())std::filesystem::create_directories(parent);
   std::ofstream ready(fs_path(ready_file),std::ios::trunc);require(bool(ready),"cannot create ready-file");ready<<port<<"\n";ready.flush();
 }
 const auto started=Clock::now();auto last=Clock::now();uint64_t max_packets=uint64_t(std::max(0,a.integer("max-packets",0)));
 while(!interrupted){
  if(!stop_file.empty() && std::filesystem::exists(fs_path(stop_file)))break;
  if(duration>0&&Clock::now()-started>=std::chrono::seconds(duration))break;
  if(max_packets&&engine.metrics().packets_rx>=max_packets)break;
#ifdef _WIN32
  // A UDP socket is a single readiness source. Winsock select works in native PowerShell.
  fd_set readfds;FD_ZERO(&readfds);FD_SET(sock.get(),&readfds);
  timeval tv{};tv.tv_usec=100000;
  int ready=::select(0,&readfds,nullptr,nullptr,&tv);
  if(ready==SOCKET_ERROR){auto e=net::last_error();if(net::interrupted(e))continue;throw std::runtime_error("select failed, error="+std::to_string(e));}
  if(ready>0 && FD_ISSET(sock.get(),&readfds)){
    // --batch is a bounded drain on Windows, NOT Linux recvmmsg syscall batching.
    const int max_read=batch?16:256;
    for(int i=0;i<max_read;i++){
      std::array<uint8_t,1500> buf;
      int n=::recvfrom(sock.get(),reinterpret_cast<char*>(buf.data()),int(buf.size()),0,nullptr,nullptr);
      if(n==SOCKET_ERROR){
        const auto e=net::last_error();
        if(net::would_block(e))break;
        if(net::interrupted(e))continue;
        // A datagram larger than the receiving buffer is discarded by
        // Winsock. Treat it as malformed traffic, not a server-fatal error.
        if(e==WSAEMSGSIZE){engine.metrics().packets_rx++;engine.metrics().bad_packets++;continue;}
        throw std::runtime_error("recvfrom failed, error="+std::to_string(e));
      }
      engine.ingest(buf.data(),size_t(n));
    }
  }
#else
  epoll_event evs[8];int ready=epoll_wait(ep,evs,8,100);if(ready<0){if(errno==EINTR)continue;throw std::runtime_error("epoll_wait failed");}
  for(int j=0;j<ready;j++)if(evs[j].data.fd==sock.get()){
   if(batch){
    for(;;){
     constexpr unsigned N=16;std::array<std::array<uint8_t,1500>,N> bufs;std::array<iovec,N> iov{};std::array<mmsghdr,N> msgs{};
     for(unsigned k=0;k<N;k++){iov[k].iov_base=bufs[k].data();iov[k].iov_len=bufs[k].size();msgs[k].msg_hdr.msg_iov=&iov[k];msgs[k].msg_hdr.msg_iovlen=1;}
     int got=recvmmsg(sock.get(),msgs.data(),N,MSG_DONTWAIT,nullptr);
     if(got<0){if(errno==EAGAIN||errno==EWOULDBLOCK)break;if(errno==EINTR)continue;throw std::runtime_error("recvmmsg failed");}
     if(got==0)break;
     for(int k=0;k<got;k++)engine.ingest(bufs[k].data(),msgs[k].msg_len);
     if(got<int(N))break;
    }
   }else{
    for(;;){std::array<uint8_t,1500> buf;ssize_t n=recvfrom(sock.get(),buf.data(),buf.size(),0,nullptr,nullptr);
     if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)break;if(errno==EINTR)continue;throw std::runtime_error("recvfrom failed");}
     engine.ingest(buf.data(),size_t(n));}
   }
  }
#endif
  engine.tick();if(Clock::now()-last>=std::chrono::seconds(1)){engine.metrics().print(engine.queue_size());last=Clock::now();}
 }
 engine.close();engine.metrics().print(engine.queue_size());
 const auto elapsed=std::chrono::duration<double>(Clock::now()-started).count();
 std::cout<<"FINAL {\"elapsed_seconds\":"<<std::fixed<<std::setprecision(3)<<elapsed<<",\"metrics\":"<<engine.metrics().json(engine.queue_size())<<"}"<<std::endl;
 if(!a.get("metrics-out").empty()){std::ofstream out(fs_path(a.get("metrics-out")));require(bool(out),"cannot open metrics-out");out<<engine.metrics().json(engine.queue_size())<<'\n';}
#ifndef _WIN32
 ::close(ep);
#endif
 return 0;
 }catch(const std::exception& e){std::cerr<<"sf_engine ERROR: "<<e.what()<<std::endl;return 1;}
}
