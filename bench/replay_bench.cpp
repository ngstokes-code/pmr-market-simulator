// Matched bounded pipeline diagnostics, never a public feed-throughput claim.
// File read/content hash, metadata, global identity, dispatch, owned mutation,
// synchronization and final canonical state are all inside library_total_s.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "msim/replay.hpp"

int main(int argc,char** argv) {
  try {
    if(argc!=9 && argc!=10)throw std::invalid_argument("usage: replay_bench INPUT WORKERS CHUNK window|hash identity|unsafe profile|plain warm|perturbed REPS [MAX_LIVE]");
    const std::string path=argv[1],index=argv[4],identity=argv[5],profile=argv[6],cache=argv[7];
    const auto workers=std::stoull(argv[2]),chunk=std::stoull(argv[3]),reps=std::stoull(argv[8]);
    if((index!="window"&&index!="hash")||(identity!="identity"&&identity!="unsafe")||(profile!="profile"&&profile!="plain")||(cache!="warm"&&cache!="perturbed")||!reps||reps>20)
      throw std::invalid_argument("invalid diagnostic arm");
    msim::ReplayOptions options;
    options.workers=1;options.chunk_messages=static_cast<std::size_t>(chunk);
    const auto live_budget=argc==10?std::stoull(argv[9]):100000ULL;
    if(live_budget<100||live_budget>4000000)throw std::invalid_argument("live-state budget");
    options.global_orders=static_cast<std::size_t>(live_budget);options.worker_orders=static_cast<std::size_t>(live_budget);options.max_levels=static_cast<std::size_t>(live_budget);
    options.reference_window_bits=18;options.output_records=30000000;options.capture_state=true;
    // Exact-state admission against the fully validated, matched one-worker path.
    const auto expected=msim::replay(path,options);
    options.workers=static_cast<std::size_t>(workers);
    options.reference_mode=index=="window"?msim::RefMap::Mode::Window:msim::RefMap::Mode::HashOnly;
    options.experimental_without_identity=identity=="unsafe";options.instrument_stages=profile=="profile";
    auto gate=options;gate.capture_state=true;
    const auto admitted=msim::replay(path,gate);
    if(expected.state!=admitted.state || (!options.experimental_without_identity && expected.day_references!=admitted.day_references))
      throw std::logic_error("diagnostic exact-state gate");
    // Deliberate cache pressure is not certified cold input or a cache flush.
    std::vector<std::uint64_t> pressure(cache=="perturbed"?(256ULL<<20)/8:0,1);
    volatile std::uint64_t sink=0;
    std::puts("scope,sha256,messages,workers,chunk,index,identity,profile,cache,rep,total_s,setup_s,pipeline_s,validation_s,read_hash_s,coordinator_wall_s,coordinator_cpu_s,coordinator_pipeline_cpu_s,worker_cpu_s,max_worker_busy_s,sum_worker_wait_s,min_worker_events,max_worker_events,buffer_bytes");
    for(std::size_t rep=0;rep<reps;++rep) {
      for(std::size_t i=0;i<pressure.size();i+=8) {pressure[i]+=i;sink+=pressure[i];}
      const auto r=msim::replay(path,options);
      if(r.state!=expected.state || r.messages!=expected.messages || r.input_sha256!=expected.input_sha256)
        throw std::logic_error("diagnostic repetition state/input mismatch");
      double worker_cpu=0,max_busy=0,wait=0;
      std::uint64_t minimum=UINT64_MAX,maximum=0;
      for(const auto& w:r.workers) {worker_cpu+=w.cpu_s;max_busy=std::max(max_busy,w.busy_s);wait+=w.wait_s;minimum=std::min(minimum,w.events);maximum=std::max(maximum,w.events);}
      std::printf("%s,%s,%llu,%llu,%llu,%s,%s,%s,%s,%zu,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%llu,%llu,%zu\n",identity=="unsafe"?"EXPERIMENTAL_NO_GLOBAL_IDENTITY":"VALIDATED_DIAGNOSTIC",r.input_sha256.c_str(),static_cast<unsigned long long>(r.messages),workers,chunk,index.c_str(),identity.c_str(),profile.c_str(),cache.c_str(),rep,r.total_s,r.setup_s,r.pipeline_s,r.final_validation_s,r.read_hash_s,r.coordinator_s,r.coordinator_cpu_s,r.coordinator_pipeline_cpu_s,worker_cpu,max_busy,wait,static_cast<unsigned long long>(minimum),static_cast<unsigned long long>(maximum),r.buffer_bytes);
      if(profile=="profile") for(std::size_t i=0;i<r.workers.size();++i) {
        const auto& w=r.workers[i];
        std::fprintf(stderr,"rep=%zu worker=%zu events=%llu busy=%.9f wait=%.9f cpu=%.9f decode_residual=%.9f metadata=%.9f mutation=%.9f\n",rep,i,static_cast<unsigned long long>(w.events),w.busy_s,w.wait_s,w.cpu_s,w.decode_s,w.metadata_s,w.mutation_s);
      }
    }
    if(sink==UINT64_MAX)std::fputs("cache pressure sentinel\n",stderr);
    return std::fflush(stdout)||std::ferror(stdout)?2:0;
  }catch(const std::exception& e) {std::fprintf(stderr,"replay_bench: %s\n",e.what());return 2;}
}
