// ITCH 5.0 decoding. The golden messages are the first message of each type
// in Nasdaq's published sample day 01302019.NASDAQ_ITCH50, copied byte for
// byte, so the field offsets are checked against real data rather than
// against a second reading of the specification.

#include "msim/itch.hpp"
#include "msim/protocol_state.hpp"
#include "msim/sha256.hpp"

#include <cstring>
#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

#include "check.hpp"

namespace itch = msim::itch;

namespace {

std::vector<unsigned char> bytes(const char* hex) {
  std::vector<unsigned char> out;
  const auto nibble = [](char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
  };
  for (std::size_t i = 0; hex[i] && hex[i + 1]; i += 2)
    out.push_back(static_cast<unsigned char>(nibble(hex[i]) << 4 |
                                             nibble(hex[i + 1])));
  return out;
}

std::string stock(const char (&s)[8]) { return std::string(s, 8); }

// Keeps the last message of each type it is given.
struct Last {
  itch::AddOrder add{};
  itch::OrderExecuted exec{};
  itch::OrderExecutedWithPrice exec_price{};
  itch::OrderCancel cancel{};
  itch::OrderDelete del{};
  itch::OrderReplace replace{};
  itch::Trade trade{};
  itch::CrossTrade cross{};
  itch::BrokenTrade broken{};
  itch::SystemEvent system{};
  itch::StockDirectory directory{};
  itch::TradingAction action{};
  itch::OperationalHalt operational{};
  itch::Imbalance imbalance{};
  itch::CircuitBreaker circuit{};
  itch::Header other{};  // last unmodelled message
  int calls = 0;

  void on(const itch::AddOrder& m) { add = m, ++calls; }
  void on(const itch::OrderExecuted& m) { exec = m, ++calls; }
  void on(const itch::OrderExecutedWithPrice& m) { exec_price = m, ++calls; }
  void on(const itch::OrderCancel& m) { cancel = m, ++calls; }
  void on(const itch::OrderDelete& m) { del = m, ++calls; }
  void on(const itch::OrderReplace& m) { replace = m, ++calls; }
  void on(const itch::Trade& m) { trade = m, ++calls; }
  void on(const itch::CrossTrade& m) { cross = m, ++calls; }
  void on(const itch::BrokenTrade& m) { broken = m, ++calls; }
  void on(const itch::SystemEvent& m) { system = m, ++calls; }
  void on(const itch::StockDirectory& m) { directory = m, ++calls; }
  void on(const itch::TradingAction& m) { action = m, ++calls; }
  void on(const itch::OperationalHalt& m) { operational = m, ++calls; }
  void on(const itch::Imbalance& m) { imbalance = m, ++calls; }
  void on(const itch::CircuitBreaker& m) { circuit = m, ++calls; }
  void on_other(const itch::Header& h) { other = h, ++calls; }
};

bool decode_hex(const char* hex, Last& h) {
  const auto m = bytes(hex);
  return itch::decode(m.data(), m.size(), h);
}

void test_real_messages() {
  Last h;
  REQUIRE(decode_hex("4101e400000d18c2eb224b000000000000232d5300000258"
                     "415247582020202000105dd8",
                     h));
  CHECK(h.add.type == 'A' && h.add.locate == 484);
  CHECK(h.add.timestamp == 14400000565835u);  // 04:00:00.000565835
  CHECK(h.add.ref == 9005 && h.add.side == 'S' && h.add.shares == 600);
  CHECK(stock(h.add.stock) == "ARGX    " && h.add.price == 1072600);
  CHECK(!h.add.attributed);

  REQUIRE(decode_hex("46220300000d18c449143a000000000000177453000000645a"
                     "565a5a54202020000249f04c45484d",
                     h));
  CHECK(h.add.type == 'F' && h.add.attributed && h.add.ref == 6004);
  CHECK(stock(h.add.stock) == "ZVZZT   " && h.add.price == 150000);

  REQUIRE(decode_hex("45217d00020d18c9b2926c00000000000000e00000005100000"
                     "0000000440d",
                     h));
  CHECK(h.exec.locate == 8573 && h.exec.ref == 224 && h.exec.shares == 81);
  CHECK(h.exec.match == 17421);

  REQUIRE(decode_hex("431a8500011c8c63dd59fd000000000053119700000064000000"
                     "000000b9e74e000ad764",
                     h));
  CHECK(h.exec_price.ref == 5443991 && h.exec_price.shares == 100);
  CHECK(h.exec_price.match == 47591 && !h.exec_price.printable);
  CHECK(h.exec_price.price == 710500);

  REQUIRE(decode_hex("58146c00000d18c6a4ce940000000000005b73000001f4", h));
  CHECK(h.cancel.locate == 5228 && h.cancel.ref == 23411);
  CHECK(h.cancel.shares == 500);

  REQUIRE(decode_hex("44207e00000d18c399a2d900000000000003b8", h));
  CHECK(h.del.locate == 8318 && h.del.ref == 952);

  REQUIRE(decode_hex("5517c000000d18c4e93eb8000000000000029300000000000038"
                     "e3000006400005c238",
                     h));
  CHECK(h.replace.old_ref == 659 && h.replace.new_ref == 14563);
  CHECK(h.replace.shares == 1600 && h.replace.price == 377400);

  REQUIRE(decode_hex("501f0700020d18cecdd8a3000000000000000042000000015547"
                     "415a202020200005f9b00000000000004411",
                     h));
  CHECK(h.trade.shares == 1 && stock(h.trade.stock) == "UGAZ    ");
  CHECK(h.trade.price == 391600 && h.trade.match == 17425);

  REQUIRE(decode_hex("510bf100011f1acee28ec20000000000000000465249202020"
                     "20200000000000000000000124ef4f",
                     h));
  CHECK(h.cross.locate == 3057 && h.cross.shares == 0);
  CHECK(stock(h.cross.stock) == "FRI     " && h.cross.cross_type == 'O');
  CHECK(h.cross.match == 74991);

  REQUIRE(decode_hex("53000000000a0a60aadb934f", h));
  CHECK(h.system.code == 'O' && h.system.timestamp == 11039687760787u);

  REQUIRE(decode_hex("52000100000a4a4cee559941202020202020204e2000000064"
                     "4e435a20504e20314e000000004e",
                     h));
  CHECK(stock(h.directory.stock) == "A       ");
  CHECK(h.directory.market_category == 'N' && h.directory.round_lot == 100);

  REQUIRE(decode_hex("48000100000a4a4d1ced644120202020202020542020202020",
                     h));
  CHECK(stock(h.action.stock) == "A       " && h.action.state == 'T');
}

void test_framing() {
  // NOII is decoded, independently of the continuous book.
  const auto sys = bytes("000c53000000000a0a60aadb934f");
  auto noii = bytes("00324900010000000000000001");
  noii.resize(52, 0);
  noii[2 + 27] = 'N'; noii[2 + 48] = 'O'; noii[2 + 49] = ' ';
  std::vector<unsigned char> feed(sys);
  feed.insert(feed.end(), noii.begin(), noii.end());

  Last h;
  itch::ParseStats s = itch::parse(feed.data(), feed.size(), h);
  CHECK(s.messages == 2 && s.other == 0 && s.bad_length == 0);
  CHECK(!s.truncated && s.bytes == feed.size());
  CHECK(h.imbalance.type == 'I' && h.imbalance.locate == 1 &&
        h.imbalance.timestamp == 1 && h.imbalance.direction == 'N');

  // A frame that claims more bytes than remain stops the walk.
  Last t;
  s = itch::parse(feed.data(), feed.size() - 1, t);
  CHECK(s.messages == 1 && s.truncated && s.bytes == sys.size());

  // Stop before a following valid message when a known type is malformed.
  std::vector<unsigned char> bad = {0x00, 0x12, 'D'};  // D is 19 bytes
  bad.resize(2 + 0x12, 0);
  bad.insert(bad.end(), sys.begin(), sys.end());
  Last b;
  s = itch::parse(bad.data(), bad.size(), b);
  CHECK(s.messages == 1 && s.bad_length == 1 && b.calls == 0);
  CHECK(s.bytes == 0 && s.error_offset == 0);
}

void test_invalid_fields_and_unknown_types() {
  using Status = itch::DecodeStatus;
  const auto original = bytes("4101e400000d18c2eb224b000000000000232d5300000258"
                              "415247582020202000105dd8");
  Last h;
  for (std::size_t n = 0; n < original.size(); ++n)
    CHECK(itch::decode_checked(original.data(), n, h) == Status::BadLength);
  // Every byte boundary of every supported payload, including intentionally
  // unmodelled types: a short payload must never reach any callback.
  for (char kind : std::string("SRHhIWAFECXDUPQBYLVKJN")) {
    const auto size = itch::expected_length(kind) ? itch::expected_length(kind) : itch::ignored_length(kind);
    std::vector<unsigned char> payload(size, 0); payload[0]=static_cast<unsigned char>(kind);
    for (std::size_t cut=0;cut<size;++cut)
      CHECK(itch::decode_checked(payload.data(),cut,h)==Status::BadLength);
    CHECK(h.calls==0);
  }
  auto bad = original;
  bad[19] = '?';
  CHECK(itch::decode_checked(bad.data(), bad.size(), h) == Status::BadField);
  bad.assign(36, 0); bad[0] = 'C'; bad[31] = '?';
  CHECK(itch::decode_checked(bad.data(), bad.size(), h) == Status::BadField);
  bad.assign(12, 0); bad[0] = 'S'; bad[11] = '?';
  CHECK(itch::decode_checked(bad.data(), bad.size(), h) == Status::BadField);
  bad.assign(25, 0); bad[0] = 'H'; bad[19] = '?';
  CHECK(itch::decode_checked(bad.data(), bad.size(), h) == Status::BadField);
  bad.assign(11, 0); bad[0] = 'Z';
  CHECK(itch::decode_checked(bad.data(), bad.size(), h) == Status::Unsupported);
  bad[0] = 'I';
  CHECK(itch::decode_checked(bad.data(), bad.size(), h) == Status::BadLength);
  CHECK(h.calls == 0);

  // Unknown types cannot be mistaken for harmless, ignored messages.
  bad.insert(bad.begin(), {0, 11}); bad[2] = 'Z';
  const auto stats = itch::parse(bad.data(), bad.size(), h);
  CHECK(stats.unsupported == 1 && stats.bytes == 0 && h.calls == 0);
}

template<class Hash> void test_hash_provider() {
  static_assert(std::is_nothrow_copy_constructible<Hash>::value);
  static_assert(std::is_nothrow_copy_assignable<Hash>::value);
  static_assert(noexcept(std::declval<Hash&>().update(nullptr, 0)));
  // hashlib.sha256(bytes((i * 131 + 17) & 255 for i in range(n))).hexdigest()
  // fixes binary vectors around SHA padding, block and caller-buffer edges.
  const std::pair<std::size_t, const char*> vectors[] = {
      {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {1, "4a64a107f0cb32536e5bce6c98c393db21cca7f4ea187ba8c4dca8b51d4ea80a"},
      {55, "59aaae80b8e7958f5757b4ac9274f1fd57ec0dc7aa8349319102316781b92006"},
      {56, "dca902d31487ffab357ce36cc5abc6947fbb69127ec04c468ba5687268c4bf7c"},
      {63, "b8e655e9e7ad413b96f0a371eb1d6db71dcca27f4eee35892517cbe4bfaa6f9a"},
      {64, "e5146be62accc56709594cb45c651b361df94f622cbb09b91ea3ca7a2060bd53"},
      {65, "4b6c6774f0cbcd776a90e187b9494e04ca880c0afe3ea1c5cad51c8bbcb41f74"},
      {119, "3244509f8746f6fb511ca8a72a292a4d7d12e456cce283c03f169dbbcb0d512e"},
      {120, "c5236e50172da3b69c903dcd3b3bb8ce855182f79364d1b59ed36367aa25e468"},
      {127, "b4f5398bf618d741f2ab0f6e2fcde06f4949c20d476b5f6e1d4bf8618bc8973a"},
      {128, "ed045c7c319e0c9286e1dea21c80bfd4a08eca43e02e13ee8a7a2c8b4b2baec9"},
      {129, "d6f2fca0407b3f98c9ce29ae92a6cdf489506450fdbb29172f6b18987bc685db"},
      {4095, "d8d5daa4611f92aff29d032c9876a120ce7a4a7d19e2c4986b8c3c1af6e85c1a"},
      {4096, "c741eee93580a334bae702208e5c0595d8e32eb07d562c52d56ca5f86e78f8a8"},
      {4097, "6b828a41dabc57049f86a8a1b0ceac3e4112258b236a527395c8116c99424ffd"},
      {4194321, "1b5b220ad85a8cd4d20de22e74a70e9f3214f6e845e29c769c5376d20c4dd6e6"}};
  for (const auto& [length, expected] : vectors) {
    std::vector<unsigned char> data(length);
    for (std::size_t i = 0; i < length; ++i)
      data[i] = static_cast<unsigned char>((i * 131 + 17) & 255);
    Hash whole; whole.update(data.data(), data.size());
    CHECK(whole.hex() == expected);
    const std::size_t chunks[] = {1, 7, 55, 64, 65, 4096, 4194304};
    for (auto chunk : chunks) {
      Hash incremental;
      for (std::size_t offset = 0; offset < data.size();) {
        const auto take = std::min(chunk, data.size() - offset);
        incremental.update(data.data() + offset, take); offset += take;
        incremental.update(nullptr, 0);
      }
      CHECK(incremental.hex() == expected);
      CHECK(incremental.hex() == expected);
      incremental.update(nullptr, 0);
      CHECK(incremental.hex() == expected);
    }
  }
  // Finalizing one value must not erase its source or a branched prefix.
  const auto* prefix = reinterpret_cast<const unsigned char*>("prefix-");
  Hash original; original.update(prefix, 7);
  const auto prefix_digest = original.hex();
  Hash left = original, right;
  right = original;
  const unsigned char a = 'a', b = 'b';
  left.update(&a, 1); right.update(&b, 1);
  CHECK(left.hex() == "636e5fb07655bb29fdb3641b6cc21e86f19fcb4c1ca8b07c0b2508d379827d5d");
  CHECK(right.hex() == "2e90e0ff14381faf365f42cbb39fa257bc307fcf9a79ee3815d3bac70d2a870e");
  CHECK(original.hex() == prefix_digest);
  original.update(&a, 1);
  CHECK(original.hex() == left.hex());
}

void test_protocol_state_and_identity_hash() {
  msim::Sha256 empty;
  CHECK(empty.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const unsigned char abc[] = {'a','b','c'};
  empty.update(abc, 1); empty.update(abc + 1, 2);
  CHECK(empty.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  msim::Sha256 million;
  const unsigned char a = 'a';
  for (unsigned n = 0; n < 1000000; ++n) million.update(&a, 1);
  CHECK(million.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  msim::FeedBook book({16, 8, msim::RefMap::Mode::Window, 3});
  msim::MarketHandler<msim::FeedBook> h(book);
  h.on(itch::SystemEvent{{'S',0,1},'O'});
  h.on(itch::SystemEvent{{'S',0,2},'S'});
  itch::StockDirectory directory{{'R',1,3},{'O','N','E',' ',' ',' ',' ',' '},'Q',100,'P'};
  h.on(directory);
  h.on(itch::TradingAction{{'H',1,4},{'O','N','E',' ',' ',' ',' ',' '},'T'});
  h.on(itch::SystemEvent{{'S',0,5},'Q'});
  CHECK(h.state.trading(1));
  h.on(itch::CircuitBreaker{{'W',0,6},'1'});
  CHECK(!h.state.trading(1));
  h.on(itch::TradingAction{{'H',1,7},{'O','N','E',' ',' ',' ',' ',' '},'T'});
  CHECK(h.state.trading(1));
  h.on(itch::AddOrder{{'A',1,8},1,'B',10,{'O','N','E',' ',' ',' ',' ',' '},100,false});
  const auto prior = book.digest();
  const auto messages = h.state.messages();
  bool rejected = false;
  try { h.on(itch::TradingAction{{'H',1,9},{'T','W','O',' ',' ',' ',' ',' '},'H'}); }
  catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected && h.failed() && book.digest() == prior &&
        h.state.messages() == messages && h.state.trading(1));
  try { h.on(itch::OrderDelete{{'D',1,10},1}); }
  catch (const std::runtime_error&) {}
  CHECK(book.digest() == prior);

  msim::ProtocolState state;
  for (char code : std::string("OSQMEC")) {
    itch::SystemEvent event{{'S',0,1},code};
    state.validate(event); state.commit(event);
  }
  CHECK(state.complete()); state.finish();
  rejected = false;
  try { state.validate(itch::SystemEvent{{'S',0,2},'O'}); }
  catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected);
  msim::ProtocolState wrong;
  rejected = false;
  try { wrong.validate(itch::SystemEvent{{'S',0,1},'M'}); }
  catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected && wrong.messages() == 0);

  // Complete-session births occur after system open and before system close.
  // A replacement rejected at close must retain its original live order.
  for (bool closed : {false,true}) for (char kind : std::string("AFU")) {
    msim::FeedBook bounded({4,4,msim::RefMap::Mode::Window,3});
    msim::MarketHandler<msim::FeedBook> protocol(bounded);
    protocol.on(itch::SystemEvent{{'S',0,1},'O'});protocol.on(directory);
    if (closed) {
      protocol.on(itch::SystemEvent{{'S',0,4},'S'});
      protocol.on(itch::AddOrder{{'A',1,5},1,'B',10,{'O','N','E',' ',' ',' ',' ',' '},100,false});
      for (char code : std::string("QME")) protocol.on(itch::SystemEvent{{'S',0,6},code});
    }
    const auto digest=bounded.digest(),count=protocol.state.messages(),births=protocol.state.reference_births();
    rejected=false;
    try {
      if (kind=='U') protocol.on(itch::OrderReplace{{'U',1,7},1,2,10,100});
      else protocol.on(itch::AddOrder{{kind,1,7},2,'B',10,{'O','N','E',' ',' ',' ',' ',' '},100,kind=='F'});
    } catch (const std::runtime_error& e) { rejected=std::string(e.what())=="order birth outside system hours"; }
    CHECK(rejected && protocol.failed() && bounded.digest()==digest && bounded.check_invariants());
    CHECK(protocol.state.messages()==count && protocol.state.reference_births()==births);
  }

  msim::FeedBook partial({4, 4, msim::RefMap::Mode::HashOnly, 3});
  msim::MarketHandler<msim::FeedBook> fragment(partial,
      msim::ProtocolState::Scope::BoundedFragment);
  fragment.on(itch::AddOrder{{'A',1,10},1,'B',10,
      {'O','N','E',' ',' ',' ',' ',' '},100,false});
  auto bad = directory; bad.stock[0] = 'X';
  rejected = false;
  try { fragment.on(bad); } catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected && partial.orders() == 1 && !fragment.state.symbol(1).directory);
}

}  // namespace

int main() {
  test_hash_provider<msim::PortableSha256>();
#if defined(__APPLE__) && !defined(MSIM_PORTABLE_SHA256)
  test_hash_provider<msim::Sha256>();
#endif
  test_protocol_state_and_identity_hash();
  test_real_messages();
  test_framing();
  test_invalid_fields_and_unknown_types();
  return check::finish("itch");
}
