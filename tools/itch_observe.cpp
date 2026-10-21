// Research bridge: verified displayed state, before/after each panel event.
// Raw rows stay outside Git. Analytics do not mutate the historical book.
#include <algorithm>
#include <csignal>
#include <fstream>
#include <iostream>
#include <set>
#include <tuple>
#include "feed_output.hpp"
#include "msim/itch_file.hpp"
#include "msim/protocol_state.hpp"
#include "msim/sha256.hpp"

namespace {
using namespace msim;
namespace itch=msim::itch;
struct Selection {
  std::vector<std::tuple<std::string,std::string,std::uint16_t>> eligible;
  bool done=false;
  void on(const itch::StockDirectory& m) {
    if (m.authenticity=='P' && (m.market_category=='Q'||m.market_category=='G'||m.market_category=='S')) {
      Sha256 hash; hash.update(reinterpret_cast<const unsigned char*>(m.stock),8);
      eligible.emplace_back(hash.hex(),std::string(m.stock,8),m.locate);
    }
  }
  void on(const itch::SystemEvent& m) { if (m.code=='Q') done=true; }
  template<class M> void on(const M&) {}
  void on_other(const itch::Header&) {}
};
struct Row {
  std::uint64_t ref=0,new_ref=0,qty=0,match=0,paired=0,imbalance=0;
  std::uint32_t price=0,new_price=0,far=0,near=0,reference=0;
  char side='-',flag='-',code='-';
};
void fields(Row& r,const itch::AddOrder& m) { r.ref=m.ref;r.side=m.side;r.price=m.price;r.qty=m.shares; }
void fields(Row& r,const itch::OrderExecuted& m) { r.ref=m.ref;r.qty=m.shares;r.match=m.match;r.flag='Y'; }
void fields(Row& r,const itch::OrderExecutedWithPrice& m) { r.ref=m.ref;r.qty=m.shares;r.match=m.match;r.flag=m.printable?'Y':'N';r.reference=m.price; }
void fields(Row& r,const itch::OrderCancel& m) { r.ref=m.ref;r.qty=m.shares; }
void fields(Row& r,const itch::OrderDelete& m) { r.ref=m.ref; }
void fields(Row& r,const itch::OrderReplace& m) { r.ref=m.old_ref;r.new_ref=m.new_ref;r.qty=m.shares;r.new_price=m.price; }
void fields(Row& r,const itch::Trade& m) { r.qty=m.shares;r.match=m.match;r.price=m.price; }
void fields(Row& r,const itch::CrossTrade& m) { r.qty=m.shares;r.match=m.match;r.price=m.price;r.code=m.cross_type; }
void fields(Row& r,const itch::BrokenTrade& m) { r.match=m.match; }
void fields(Row& r,const itch::SystemEvent& m) { r.code=m.code; }
void fields(Row& r,const itch::TradingAction& m) { r.code=m.state; }
void fields(Row& r,const itch::OperationalHalt& m) { r.flag=m.market;r.code=m.action; }
void fields(Row& r,const itch::CircuitBreaker& m) { r.code=m.level; }
void fields(Row& r,const itch::Imbalance& m) { r.paired=m.paired;r.imbalance=m.imbalance;r.side=m.direction;r.far=m.far_price;r.near=m.near_price;r.reference=m.reference_price;r.code=m.cross_type;r.flag=m.variation==' '?'_':m.variation; }
template<class M> void fields(Row&,const M&) {}
struct Observer {
  FeedBook book;
  MarketHandler<FeedBook> market{book};
  std::set<std::uint16_t> panel;
  std::ofstream& out;
  std::uint64_t ordinal=0,rows=0;
  std::size_t offset=0;
  explicit Observer(std::ofstream& stream):out(stream) {}
  std::array<std::uint64_t,4> quote(std::uint16_t loc) const {
    auto b=book.best(loc,Side::BUY),a=book.best(loc,Side::SELL);
    return {b.value_or(0),a.value_or(0),b?book.level(loc,Side::BUY,*b).shares:0,
            a?book.level(loc,Side::SELL,*a).shares:0};
  }
  template<class M> void on(const M& m) {
    const bool selected=panel.count(m.locate) || m.type=='S' || m.type=='W';
    Row r; fields(r,m);
    std::array<std::uint64_t,4> pre{};
    FeedBook::LevelState level{};
    std::uint64_t own=0,ahead=0,ahead_qty=0,birth_ahead=0,birth_orders=0;
    bool active=false;
    char session='?',trading='?',oper='?';
    if (selected) {
      pre=quote(m.locate); active=market.state.trading(m.locate);
      session=market.state.session(); const auto& s=market.state.symbol(m.locate);
      trading=s.trading;oper=s.nasdaq_operation;
      if (m.type=='E' || m.type=='C' || m.type=='X' || m.type=='D' || m.type=='U') {
        const auto order=book.order(r.ref);
        if (order) {
          r.side=order->side==Side::BUY?'B':'S';r.price=order->price;own=order->shares;
          bool found=false;
          book.for_each(m.locate,order->side,[&](auto p,auto ref,auto qty) {
            if (ref==r.ref) found=true;
            if (p==r.price && !found) { ++ahead; ahead_qty+=qty; }
          });
        }
      }
      if (r.side=='B'||r.side=='S') level=book.level(m.locate,r.side=='B'?Side::BUY:Side::SELL,r.price);
      if (m.type=='D') r.qty=own;
    }
    market.set_context(offset,ordinal);market.on(m);
    if (!selected) return;
    const auto post=quote(m.locate);
    const auto after=(r.side=='B'||r.side=='S')?book.level(m.locate,r.side=='B'?Side::BUY:Side::SELL,r.price):FeedBook::LevelState{};
    if (m.type=='A'||m.type=='F'||m.type=='U') {
      const auto price=m.type=='U'?r.new_price:r.price;
      const auto born=book.level(m.locate,r.side=='B'?Side::BUY:Side::SELL,price);
      birth_ahead=born.shares-r.qty;birth_orders=born.orders-1;
    }
    out<<ordinal<<','<<m.timestamp<<','<<m.type<<','<<m.locate<<','<<r.ref<<','<<r.new_ref<<','<<r.side<<','<<r.price<<','<<r.new_price<<','<<r.qty<<','<<r.match<<','<<r.flag<<','<<r.code<<','<<r.paired<<','<<r.imbalance<<','<<r.far<<','<<r.near<<','<<r.reference;
    for (auto v:pre) out<<','<<v;
    out<<','<<level.shares<<','<<level.orders<<','<<own<<','<<ahead<<','<<ahead_qty;
    for (auto v:post) out<<','<<v;
    out<<','<<after.shares<<','<<birth_ahead<<','<<birth_orders<<','<<session<<','<<trading<<','<<oper<<','<<active<<','<<market.state.trading(m.locate)<<','<<market.state.symbol(m.locate).high_timestamp<<'\n';
    if (!out) throw std::runtime_error("observation row write failed");
    ++rows;
  }
  void on_other(const itch::Header& h) { on(h); }
};

// Presentation is a projection of the strict full-input replay, not a second
// matching policy. Its prefix snapshot must exist before the first clip event.
struct TapeObserver {
  FeedBook book;
  MarketHandler<FeedBook> market{book};
  std::ostream& initial;
  std::ostream& events;
  std::string stock;
  std::uint64_t start,end,ordinal=0,count=0,initial_count=0;
  std::uint16_t locate=0;
  std::size_t offset=0;
  bool started=false,ended=false;
  TapeObserver(std::ostream& i,std::ostream& e,std::string s,
               std::uint64_t first,std::uint64_t last)
      :initial(i),events(e),stock(std::move(s)),start(first),end(last) {}
  void snapshot() {
    for (Side side:{Side::BUY,Side::SELL})
      book.for_each(locate,side,[&](auto p,auto ref,auto qty) {
        initial<<ref<<','<<(side==Side::BUY?'B':'S')<<','<<p<<','<<qty<<'\n';
        ++initial_count;
      });
    if (!initial) throw std::runtime_error("tape initial state write failed");
  }
  template<class M> void identify(const M&) {}
  void identify(const itch::StockDirectory& m) {
    if (feed_stock(m.stock)==stock) locate=m.locate;
  }
  template<class M> void on(const M& m) {
    identify(m);
    const bool selected=locate && m.locate==locate;
    if (selected && !started && m.timestamp>=start) { snapshot();started=true; }
    if (selected && m.timestamp>=end) ended=true;
    const bool emit=selected && started && !ended;
    Row r;
    if (emit) {
      if (count>=200000) throw std::runtime_error("tape event bound exceeded");
      fields(r,m);
      if (m.type=='E'||m.type=='C'||m.type=='X'||m.type=='D'||m.type=='U') {
        const auto prior=book.order(r.ref);
        if (!prior) throw std::runtime_error("tape missing affected reference");
        r.side=prior->side==Side::BUY?'B':'S';r.price=prior->price;
        if (m.type=='D') r.qty=prior->shares;
      }
    }
    market.set_context(offset,ordinal);market.on(m);
    if (!emit) return;
    Digest digest;book.digest_into(digest,locate);
    events<<ordinal<<','<<offset<<','<<m.timestamp<<','<<m.type<<','<<r.ref<<','<<r.new_ref<<','<<r.side<<','<<r.price<<','<<r.new_price<<','<<r.qty<<','<<r.match<<','<<r.flag<<','<<r.reference<<','<<r.code<<','<<digest.value()<<'\n';
    if (!events) throw std::runtime_error("tape event write failed");
    ++count;
  }
  void on_other(const itch::Header& h) { on(h); }
};

int tape(int argc,char** argv) {
  if (argc!=7) throw std::invalid_argument("usage: itch_observe --tape INPUT OUTDIR SYMBOL START_NS END_NS");
  const std::string stock=argv[4];
  if (stock.empty() || stock.size()>8) throw std::invalid_argument("tape symbol must have 1..8 bytes");
  feed_ascii_text(stock);
  const auto start=feed_number(argv[5]),end=feed_number(argv[6]);
  if (start>=end || end>86'400'000'000'000ULL || end-start>60'000'000'000ULL)
    throw std::invalid_argument("tape interval must be positive and at most 60 seconds");
  InputFile guard(argv[2]);guard.reject_alias(argv[3]);
  FeedDirectory publication(argv[3]);
  std::ofstream initial(publication.path()/"initial.csv"),events(publication.path()/"events.csv");
  initial<<"ref,side,price,quantity\n";
  events<<"ordinal,offset,timestamp,kind,ref,new_ref,side,price,new_price,quantity,match,flag,execution_price,code,digest\n";
  TapeObserver handler(initial,events,stock,start,end);
  ItchFile reader(argv[2]);ItchFile::Frame frame{};
  while (reader.next(frame)) {
    handler.ordinal=frame.ordinal;handler.offset=frame.offset;
    if (itch::decode_checked(frame.data,frame.size,handler)!=itch::DecodeStatus::Decoded)
      throw std::runtime_error("tape decode failed at "+std::to_string(frame.ordinal));
  }
  handler.market.state.finish();reader.finish();
  if (!handler.started || !handler.count || !handler.ended)
    throw std::runtime_error("tape interval not bracketed by symbol events");
  if (!handler.book.check_invariants()) throw std::runtime_error("tape final book invariant failed");
  initial.close();events.close();
  if (!initial || !events) throw std::runtime_error("tape output close failed");
  std::ofstream summary(publication.path()/"summary.json");
  summary<<"{\"status\":\"SUCCESS\",\"schema\":1,\"stock\":"<<feed_json(stock)<<",\"locate\":"<<handler.locate<<",\"start_ns\":"<<start<<",\"end_ns\":"<<end<<",\"input_bytes\":"<<reader.input().size()<<",\"input_sha256\":"<<feed_json(reader.input().sha256())<<",\"messages\":"<<handler.ordinal<<",\"events\":"<<handler.count<<",\"initial_orders\":"<<handler.initial_count<<",\"capture_continuity\":\"UNVERIFIED\"}\n";
  summary.close();if (!summary) throw std::runtime_error("tape summary write failed");
  guard.validate_unchanged();
  std::cout<<"tape events="<<handler.count<<" initial orders="<<handler.initial_count<<'\n'<<std::flush;
  if (!std::cout) throw std::runtime_error("tape console write failed");
  publication.publish();std::cout<<"result: SUCCESS\n"<<std::flush;
  if (!std::cout) throw std::runtime_error("tape console write failed");
  publication.commit();return 0;
}
int run(int argc,char** argv) {
  if (argc>1 && std::string(argv[1])=="--tape") return tape(argc,argv);
  if (argc!=3) throw std::invalid_argument("usage: itch_observe INPUT OUTDIR");
  InputFile guard(argv[1]);guard.reject_alias(argv[2]);
  FeedDirectory publication(argv[2]);
  Selection select;
  {
    ItchFile reader(argv[1]);ItchFile::Frame frame{};
    while (!select.done && reader.next(frame)) {
      if (itch::decode_checked(frame.data,frame.size,select)!=itch::DecodeStatus::Decoded)
        throw std::runtime_error("panel selection decode failed");
    }
    reader.input().validate_unchanged();
  }
  if (!select.done) throw std::runtime_error("panel selection requires Start of Market Hours");
  std::sort(select.eligible.begin(),select.eligible.end());
  select.eligible.erase(std::unique(select.eligible.begin(),select.eligible.end()),select.eligible.end());
  if (select.eligible.size()>32) select.eligible.resize(32);
  std::ofstream panel(publication.path()/"panel.csv"),rows(publication.path()/"events.csv");
  panel<<"locate,stock,selection_sha256\n";
  rows<<"ordinal,timestamp,kind,locate,ref,new_ref,side,price,new_price,quantity,match,flag,code,paired,imbalance,far,near,reference,bid,ask,bid_qty,ask_qty,level_qty,level_orders,order_qty,ahead_orders,ahead_qty,post_bid,post_ask,post_bid_qty,post_ask_qty,post_level_qty,birth_ahead,birth_orders,session,trading,operational,active,post_active,high_timestamp\n";
  Observer handler(rows);
  for (const auto& [hash,name,loc]:select.eligible) {
    handler.panel.insert(loc); panel<<loc<<','<<feed_csv_field(feed_stock(name.data()))<<','<<hash<<'\n';
  }
  panel.close();if (!panel) throw std::runtime_error("panel write failed");
  ItchFile reader(argv[1]);ItchFile::Frame frame{};
  while (reader.next(frame)) {
    handler.ordinal=frame.ordinal;handler.offset=frame.offset;
    if (itch::decode_checked(frame.data,frame.size,handler)!=itch::DecodeStatus::Decoded)
      throw std::runtime_error("observation decode failed at "+std::to_string(frame.ordinal));
  }
  handler.market.state.finish();reader.finish();
  if (!handler.book.check_invariants()) throw std::runtime_error("final book invariant failed");
  rows.close();if (!rows) throw std::runtime_error("observation rows close failed");
  std::ofstream manifest(publication.path()/"summary.json");
  manifest<<"{\"status\":\"SUCCESS\",\"protocol\":\"ITCH-5.0\",\"terminal_event\":\"C\",\"capture_continuity\":\"UNVERIFIED\",\"input_bytes\":"<<reader.input().size()<<",\"input_sha256\":"<<feed_json(reader.input().sha256())<<",\"messages\":"<<handler.ordinal<<",\"rows\":"<<handler.rows<<",\"panel_size\":"<<handler.panel.size()<<",\"reference_births\":"<<handler.market.state.reference_births()<<"}\n";
  manifest.close();if (!manifest) throw std::runtime_error("manifest write failed");
  guard.validate_unchanged();
  std::cout<<"observations="<<handler.rows<<" panel="<<handler.panel.size()<<'\n'<<std::flush;
  if (!std::cout) throw std::runtime_error("console write failed");
  publication.publish();
  std::cout<<"result: SUCCESS\n"<<std::flush;
  if (!std::cout) throw std::runtime_error("console write failed");
  publication.commit();return 0;
}
}
int main(int argc,char** argv) {
  std::signal(SIGPIPE,SIG_IGN);std::signal(SIGXFSZ,SIG_IGN);
  try { return run(argc,argv); }
  catch (const std::exception& e) { std::fprintf(stderr,"itch_observe: %s\nresult: INVALID\n",e.what());return 2; }
  catch (...) { std::fputs("itch_observe: unknown failure\nresult: INVALID\n",stderr);return 2; }
}
