// Isolated structure kernels, not an attribution of whole-book speedup.
// Each timed trace has a separate exact-state/digest gate against std::set/map.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "msim/ladder.hpp"
#include "msim/ref_map.hpp"

namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t next(std::uint64_t& x) { x^=x<<13;x^=x>>7;x^=x<<17;return x; }
struct Sorted {
  std::vector<std::uint32_t> keys;
  explicit Sorted(std::size_t n) { keys.reserve(n);for(std::size_t i=0;i<n;++i) keys.push_back(static_cast<std::uint32_t>(2*i)); }
  void change(std::uint32_t old,std::uint32_t now) {
    auto i=msim::detail::lower_bound_from_best(keys.data(),keys.size(),old);
    if(i==keys.size()||keys[i]!=old) throw std::logic_error("missing sorted key");
    keys.erase(keys.begin()+static_cast<std::ptrdiff_t>(i));
    i=msim::detail::lower_bound_from_best(keys.data(),keys.size(),now);
    keys.insert(keys.begin()+static_cast<std::ptrdiff_t>(i),now);
  }
  std::uint32_t best() const { return keys.back(); }
};
struct Tree {
  std::set<std::uint32_t> keys;
  explicit Tree(std::size_t n) { for(std::size_t i=0;i<n;++i) keys.insert(static_cast<std::uint32_t>(2*i)); }
  void change(std::uint32_t old,std::uint32_t now) { if(keys.erase(old)!=1||!keys.insert(now).second) throw std::logic_error("tree key"); }
  std::uint32_t best() const { return *keys.rbegin(); }
};
struct Change { std::uint32_t before,after; };
std::vector<Change> changes(std::size_t levels,std::size_t ops,bool near) {
  std::vector<std::uint32_t> live(levels);for(std::size_t i=0;i<levels;++i)live[i]=static_cast<std::uint32_t>(2*i);
  std::vector<Change> result;result.reserve(ops);std::uint64_t random=17;
  for(std::size_t i=0;i<ops;++i) {
    const auto slot=near?levels-1-next(random)%std::min<std::size_t>(levels,8):next(random)%levels;
    const auto before=live[slot];live[slot]^=1;result.push_back({before,live[slot]});
  }
  return result;
}
template<class T> std::uint64_t ladder(T& subject,const std::vector<Change>& trace) {
  std::uint64_t digest=0;
  for(auto [old,now]:trace) { subject.change(old,now);digest=digest*0x9e3779b97f4a7c15ULL+subject.best(); }
  return digest;
}
struct IndexOp { char kind;std::uint64_t ref;std::uint32_t value; };
std::vector<IndexOp> operations(std::size_t n,const std::string& pattern) {
  std::vector<IndexOp> result;result.reserve(n*4);std::vector<std::uint64_t> live;
  for(std::size_t i=0;i<n;++i) {
    const auto ref=pattern=="jump"?static_cast<std::uint64_t>(i)*((1ULL<<32)+9000):static_cast<std::uint64_t>(i);
    result.push_back({'A',ref,static_cast<std::uint32_t>(i)});live.push_back(ref);
    const auto target=pattern=="aged"?i%std::min<std::size_t>(i+1,100):i;
    result.push_back({'F',live[target],0});
    if(live.size()>(pattern=="aged"?2148U:2048U))result.push_back({'D',live[live.size()-2049],0});
  }
  if(pattern=="aged")for(std::size_t i=0;i<std::min<std::size_t>(n,100);++i)result.push_back({'D',live[i],0});
  for(std::size_t i=n>(pattern=="aged"?2148U:2048U)?n-2048:(pattern=="aged"?std::min<std::size_t>(n,100):0);i<n;++i) result.push_back({'D',live[i],0});
  return result;
}
std::uint64_t index(msim::RefMap& subject,const std::vector<IndexOp>& trace) {
  std::uint64_t digest=0;
  for(auto op:trace) {
    if(op.kind=='A') { if(!subject.insert(op.ref,op.value))throw std::logic_error("index insert failed"); }
    else if(op.kind=='F') digest=digest*0x9e3779b97f4a7c15ULL+subject.find(op.ref);
    else if(!subject.take(op.ref))throw std::logic_error("index erase failed");
  }
  if(subject.size())throw std::logic_error("index final state not empty");
  return digest;
}
std::uint64_t map_gate(const std::vector<IndexOp>& trace) {
  std::map<std::uint64_t,std::uint32_t> map;std::uint64_t digest=0;
  for(auto op:trace) {
    if(op.kind=='A') { if(!map.emplace(op.ref,op.value).second)throw std::logic_error("duplicate gate key"); }
    else if(op.kind=='F') { const auto found=map.find(op.ref);digest=digest*0x9e3779b97f4a7c15ULL+(found==map.end()?UINT32_MAX:found->second); }
    else if(map.erase(op.ref)!=1)throw std::logic_error("missing gate key");
  }
  if(!map.empty())throw std::logic_error("gate nonempty");
  return digest;
}
}
int main(int argc,char** argv) {
  try {
    if(argc!=6)throw std::invalid_argument("usage: structure_bench ladder|index ARM SIZE OPS near|uniform|recent|aged|jump");
    const std::string kind=argv[1],arm=argv[2],pattern=argv[5];
    const auto size=std::stoull(argv[3]),ops=std::stoull(argv[4]);
    if(!size||size>1000000||!ops||ops>10000000)throw std::invalid_argument("bounded kernel sizes required");
    std::printf("kind,arm,pattern,size,operations,repetition,setup_seconds,kernel_seconds,digest\n");
    if(kind=="ladder") {
      if((arm!="vector"&&arm!="tree")||(pattern!="near"&&pattern!="uniform"))throw std::invalid_argument("ladder arm/pattern");
      const auto trace=changes(static_cast<std::size_t>(size),static_cast<std::size_t>(ops),pattern=="near");
      Sorted a(static_cast<std::size_t>(size));Tree b(static_cast<std::size_t>(size));
      const auto expected=ladder(b,trace);
      if(ladder(a,trace)!=expected||!std::equal(a.keys.begin(),a.keys.end(),b.keys.begin(),b.keys.end()))throw std::logic_error("ladder exact-state gate");
      for(int rep=-1;rep<5;++rep) {
        const auto start=Clock::now();std::uint64_t digest=0;double setup=0,kernel=0;
        if(arm=="vector") { Sorted s(static_cast<std::size_t>(size));const auto before=Clock::now();digest=ladder(s,trace);const auto end=Clock::now();setup=std::chrono::duration<double>(before-start).count();kernel=std::chrono::duration<double>(end-before).count(); }
        else { Tree s(static_cast<std::size_t>(size));const auto before=Clock::now();digest=ladder(s,trace);const auto end=Clock::now();setup=std::chrono::duration<double>(before-start).count();kernel=std::chrono::duration<double>(end-before).count(); }
        if(digest!=expected)throw std::logic_error("timed ladder digest");
        if(rep>=0)std::printf("%s,%s,%s,%llu,%llu,%d,%.9f,%.9f,%llu\n",kind.c_str(),arm.c_str(),pattern.c_str(),size,ops,rep,setup,kernel,static_cast<unsigned long long>(digest));
      }
    } else if(kind=="index") {
      if((arm!="window"&&arm!="hash")||(pattern!="recent"&&pattern!="aged"&&pattern!="jump")||size<3||size>24)throw std::invalid_argument("index arm/pattern/window bits");
      const auto trace=operations(static_cast<std::size_t>(ops),pattern);const auto expected=map_gate(trace);
      msim::RefMap::Stats stats;
      msim::RefMap diagnostic(4096,arm=="window"?msim::RefMap::Mode::Window:msim::RefMap::Mode::HashOnly,static_cast<unsigned>(size),&stats);
      if(index(diagnostic,trace)!=expected)throw std::logic_error("instrumented index gate");
      std::fprintf(stderr,"diagnostic ring_hits=%llu hash_hits=%llu hash_misses=%llu scanned_slots=%llu migrations=%llu peak_hash_entries=%zu\n",static_cast<unsigned long long>(stats.ring_hits),static_cast<unsigned long long>(stats.hash_hits),static_cast<unsigned long long>(stats.hash_misses),static_cast<unsigned long long>(stats.scanned_slots),static_cast<unsigned long long>(stats.migrations),stats.peak_hash_entries);
      for(int rep=-1;rep<5;++rep) {
        const auto start=Clock::now();msim::RefMap subject(4096,arm=="window"?msim::RefMap::Mode::Window:msim::RefMap::Mode::HashOnly,static_cast<unsigned>(size));
        const auto before=Clock::now();const auto digest=index(subject,trace);const auto end=Clock::now();
        if(digest!=expected)throw std::logic_error("index map gate");
        if(rep>=0)std::printf("%s,%s,%s,%llu,%zu,%d,%.9f,%.9f,%llu\n",kind.c_str(),arm.c_str(),pattern.c_str(),size,trace.size(),rep,std::chrono::duration<double>(before-start).count(),std::chrono::duration<double>(end-before).count(),static_cast<unsigned long long>(digest));
      }
    } else throw std::invalid_argument("unknown kernel");
    return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"structure_bench: %s\n",e.what());return 2;}
}
