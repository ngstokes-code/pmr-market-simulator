## High-Level System Architecture

```
┌──────────────────────────────┐
│        MarketSim CLI         │
│------------------------------│
│ Parses flags (--events, etc) │
│ Constructs SimConfig         │
│ → Simulator(cfg)             │
│ → sim.run()                  │
└──────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│                     Simulator Object                       │
│------------------------------------------------------------│
│  Holds global RNG (rng_)                                   │
│  Initializes one SymState per symbol                       │
│  Each SymState owns:                                       │
│   • ArenaBundle (PMR memory)                               │
│   • OrderBook (PMR containers)                             │
│   • midprice (double)                                      │
│                                                            │
│  Emits Events → IStorage (BinaryLogStorage or LMDBStorage) │
└────────────────────────────────────────────────────────────┘
```

## Memory & Arena Layout (Per Symbol)

### Overview
```
             ┌────────────────────────────────────────────┐
Symbol: AAPL │   SymState                                 │
             │--------------------------------------------│
             │ mid = 100.0                                │
             │ book = OrderBook(AAPL, &arena)             │
             │ mem = unique_ptr<ArenaBundle>              │
             └────────────────────────────────────────────┘
                                │
                                ▼
                ┌──────────────────────────────────────┐
                │ ArenaBundle                          │
                │--------------------------------------│
                │ buffer: std::vector<std::byte> (1MB) │
                │ counter: CountingResource            │
                │ arena: std::pmr::monotonic_buffer... │
                └──────────────────────────────────────┘
                                │
                                ▼
             ┌────────────────────────────────────────────┐
             │ std::pmr::monotonic_buffer_resource        │
             │--------------------------------------------│
             │ Allocates sequentially inside buffer[]     │
             │ → no frees, no fragmentation               │
             │ → all allocations local to this symbol     │
             │ Upstream: CountingResource → new_delete    │
             └────────────────────────────────────────────┘
```

### Arena Layout Sketch
```
  Arena memory (1MB contiguous buffer per symbol)
  - conceptual / macro picture
  - bird's-eye schematic of the arena, showing that everything for AAPL lives in one contiguous buffer while MSFT has another.
 ┌────────────────────────────────────────────────────────────┐
 │ +----------------------+ +----------------------+          │
 │ | Order nodes (bids)   | | Ask nodes           | ...       │
 │ +----------------------+ +----------------------+          │
 │ contiguous allocations → cache-friendly, no heap churn     │
 │ Freed only when arena destroyed (per-symbol lifetime)      │
 └────────────────────────────────────────────────────────────┘
```

### Memory Layout Sketch
```
The microscopic or byte-level view of what actually fills that 1 MiB slab.
|<--------------------------- 1 MiB -------------------------------->|
[ map node ][ map node ][ deque block ][ index node ][ deque block ]...
^             ^                     ^              ^
|             |                     |              └─ price/qty entries
|             |                     └─ queue of orders for one price
|             └─ tree node for bids/asks
└─ first allocation from the arena
```

## Order Book Memory Model

```
       ┌────────────────────────────────────────────┐
       │ OrderBook (PMR-backed)                     │
       │--------------------------------------------│
       │ bids_: std::pmr::map<double, deque<Order>> │
       │ asks_: std::pmr::map<double, deque<Order>> │
       │ index_: std::pmr::map<uint64_t, (side,px)> │
       │ symbol_ : "AAPL"                           │
       └────────────────────────────────────────────┘

        ↓ All three containers share the same arena ↓

        ┌──────────────────────────────────────────┐
        │ Monotonic buffer                         │
        │------------------------------------------│
        │ nodes for map tree (key=price, value=q)  │
        │ nodes for deque<Order> blocks            │
        │ nodes for index map                      │
        │ ... all bump-allocated sequentially      │
        └──────────────────────────────────────────┘
```

Key takeaway:
**Each symbol is a self-contained memory world — its own monotonic arena and allocator.**
-> Zero cross-symbol contention.
-> Zero heap fragmentation.
-> Everything lives/frees together
