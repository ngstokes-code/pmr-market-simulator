#include "msim/replay.hpp"
#include "msim/input_file.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#include <unistd.h>

#include "check.hpp"

namespace {
using Bytes=std::vector<unsigned char>;
void integer(Bytes& b,std::uint64_t n,unsigned width) {
  for (unsigned shift=width;shift;--shift)
    b.push_back(static_cast<unsigned char>(n>>(8*(shift-1))));
}
Bytes payload(char type,std::uint16_t loc,std::uint64_t time) {
  Bytes b{static_cast<unsigned char>(type)};
  integer(b,loc,2); integer(b,0,2); integer(b,time,6); return b;
}
void frame(Bytes& out,const Bytes& b) {
  integer(out,b.size(),2); out.insert(out.end(),b.begin(),b.end());
}
void stock(Bytes& b,const char* name) {
  for (std::size_t n=0;n<8;++n) b.push_back(static_cast<unsigned char>(n<std::strlen(name)?name[n]:' '));
}
void system(Bytes& out,char code,std::uint64_t time) {
  auto b=payload('S',0,time); b.push_back(static_cast<unsigned char>(code)); frame(out,b);
}
void directory(Bytes& out,std::uint16_t loc,const char* name,std::uint64_t time) {
  auto b=payload('R',loc,time); stock(b,name); b.resize(39,' ');
  b[19]='Q'; b[21]=0; b[22]=0; b[23]=0; b[24]=100; b[29]='P'; frame(out,b);
  b=payload('H',loc,time+1); stock(b,name); b.resize(25,' '); b[19]='T'; frame(out,b);
}
void add(Bytes& out,std::uint16_t loc,std::uint64_t ref,char side,
         std::uint32_t size,std::uint32_t price,const char* name,std::uint64_t time) {
  auto b=payload('A',loc,time); integer(b,ref,8); b.push_back(static_cast<unsigned char>(side));
  integer(b,size,4); stock(b,name); integer(b,price,4); frame(out,b);
}
Bytes valid_feed() {
  Bytes out;
  system(out,'O',1); system(out,'S',2);
  directory(out,1,"ONE",3); directory(out,2,"TWO",5);
  directory(out,3,"THREE",7); directory(out,4,"FOUR",9);
  system(out,'Q',11);
  const auto far=(std::uint64_t{1}<<3)+msim::RefMap::kMaxJump+5;
  add(out,1,far,'B',10,100,"ONE",20);
  add(out,1,far-6,'B',10,100,"ONE",21);
  add(out,1,far+1,'S',8,101,"ONE",22);
  auto b=payload('E',1,30); integer(b,far,8); integer(b,4,4); integer(b,1,8); frame(out,b);
  b=payload('X',1,31); integer(b,far-6,8); integer(b,2,4); frame(out,b);
  b=payload('U',1,32); integer(b,far,8); integer(b,100,8); integer(b,6,4); integer(b,100,4); frame(out,b);
  add(out,2,200,'B',12,900,"TWO",35);
  add(out,3,300,'S',7,1100,"THREE",36);
  add(out,4,400,'B',5,1000,"FOUR",37);
  system(out,'M',40); system(out,'E',41); system(out,'C',42);
  return out;
}
void write(const std::filesystem::path& path,const Bytes& bytes) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
  out.close(); if (!out) throw std::runtime_error("fixture write failed");
}
msim::ReplayOptions small() {
  msim::ReplayOptions o;
  o.global_orders=32; o.max_levels=16; o.output_records=32;
  o.reference_window_bits=3; o.chunk_messages=3; o.capture_state=true;
  return o;
}
void compare_workers(const std::filesystem::path& path,const Bytes& bytes) {
  write(path,bytes);
  std::vector<msim::ReplayRow> expected{
      {1,msim::Side::BUY,100,msim::RefMap::kMaxJump+7,8},
      {1,msim::Side::BUY,100,100,6},
      {1,msim::Side::SELL,101,msim::RefMap::kMaxJump+14,8},
      {2,msim::Side::BUY,900,200,12},
      {3,msim::Side::SELL,1100,300,7},
      {4,msim::Side::BUY,1000,400,5}};
  std::vector<msim::ReplaySymbol> baseline;
  for (auto index : {msim::RefMap::Mode::Window,msim::RefMap::Mode::HashOnly})
    for (std::size_t chunk : {std::size_t{1},std::size_t{3},std::size_t{7},std::size_t{31}})
      for (std::size_t count : {std::size_t{1},std::size_t{2},std::size_t{3},std::size_t{4},std::size_t{4}}) {
        auto o=small(); o.chunk_messages=chunk; o.workers=count; o.reference_mode=index;
        o.reference=count==1;
        const auto r=msim::replay(path.string(),o);
        CHECK(r.state==expected && r.live_orders==6 && r.messages==23 && r.terminal_event=='C');
        CHECK(r.reference_births==7 && r.day_reference_blocks==2);
        CHECK((r.day_references==std::vector<std::uint64_t>{100,200,300,400,
            msim::RefMap::kMaxJump+7,msim::RefMap::kMaxJump+13,msim::RefMap::kMaxJump+14}));
        CHECK(r.bytes==bytes.size() && r.input_sha256.size()==64 && r.clock_regressions==0);
        if (baseline.empty()) baseline=r.symbols;
        CHECK(r.symbols.size()==baseline.size());
        for (std::size_t n=0;n<baseline.size();++n) {
          CHECK(r.symbols[n].locate==baseline[n].locate);
          CHECK(r.symbols[n].state.stock==baseline[n].state.stock);
          CHECK(r.symbols[n].state.high_timestamp==baseline[n].state.high_timestamp);
          CHECK(r.symbols[n].state.trading_update==baseline[n].state.trading_update);
          CHECK(r.symbols[n].state.trading==baseline[n].state.trading);
        }
      }
}
template<class Fn> void must_fail(Fn&& action,const char* reason) {
  bool failed=false;
  try { action(); }
  catch (const std::exception& e) { failed=std::string(e.what()).find(reason)!=std::string::npos; }
  CHECK(failed);
}
void failure_campaign(const std::filesystem::path& path,const Bytes& bytes) {
  write(path,bytes);
  auto o=small(); o.workers=4;
  msim::ReplayHooks hooks;
  hooks.before_launch=[](std::size_t n) {
    if (n==2) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
  };
  o.hooks=&hooks;
  must_fail([&] { (void)msim::replay(path.string(),o); },"");
  hooks.before_launch={};
  hooks.before_worker_event=[](std::size_t,std::uint64_t n) { if (n==14) throw std::bad_alloc(); };
  must_fail([&] { (void)msim::replay(path.string(),o); },"bad_alloc");
  o.hooks=nullptr; o.worker_orders=1;
  must_fail([&] { (void)msim::replay(path.string(),o); },"capacity");
  o=small(); o.global_orders=1;
  must_fail([&] { (void)msim::replay(path.string(),o); },"capacity");
  o=small(); o.max_levels=1;
  must_fail([&] { (void)msim::replay(path.string(),o); },"capacity");
  o=small(); o.output_records=1;
  must_fail([&] { (void)msim::replay(path.string(),o); },"output capacity");
  o=small(); o.day_reference_blocks=0;
  must_fail([&] { (void)msim::replay(path.string(),o); },"day-reference capacity");

  // A prefix ending on a complete frame is still an incomplete session.
  auto cut=bytes; cut.resize(cut.size()-14); write(path,cut); o=small();
  must_fail([&] { (void)msim::replay(path.string(),o); },"incomplete session");
  cut=bytes; cut.pop_back(); write(path,cut);
  must_fail([&] { (void)msim::replay(path.string(),o); },"truncated");
  write(path,bytes);
  o.hooks=&hooks; std::atomic<bool> changed{false};
  hooks.before_worker_event=[&](std::size_t,std::uint64_t n) {
    if (n==12 && !changed.exchange(true)) {
      std::ofstream out(path,std::ios::app|std::ios::binary);
      const unsigned char extra[14]={0,12,'S',0,0,0,0,0,0,0,0,0,0,'C'};
      out.write(reinterpret_cast<const char*>(extra),14); out.close();
    }
  };
  must_fail([&] { (void)msim::replay(path.string(),o); },"input changed");
  CHECK(changed.load());
  write(path,bytes); changed=false;
  hooks.before_worker_event=[&](std::size_t,std::uint64_t n) {
    if (n==12 && !changed.exchange(true)) std::filesystem::resize_file(path,0);
  };
  must_fail([&] { (void)msim::replay(path.string(),o); },"input");
  write(path,bytes);
  msim::InputFile source(path.string());
  must_fail([&] { source.reject_alias(path.string()); },"aliases input");
  unsigned char buffer[7];
  while (source.read(buffer,sizeof buffer)) {}
  source.finish();
}
}  // namespace
int main() {
  char pattern[]="/tmp/msim-replay-unit-XXXXXX";
  const char* name=::mkdtemp(pattern);
  if (!name) return 2;
  const std::filesystem::path dir(name),path=dir/"generated.itch";
  try {
    const auto bytes=valid_feed(); compare_workers(path,bytes); failure_campaign(path,bytes);
  } catch (const std::exception& e) { std::fprintf(stderr,"replay test: %s\n",e.what()); CHECK(false); }
  if (!check::failures()) std::filesystem::remove_all(dir);
  else std::fprintf(stderr,"failed fixture retained: %s\n",name);
  return check::finish("exact replay ownership and failure boundaries");
}
