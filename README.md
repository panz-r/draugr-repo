# draugr

![Draugr Logo](draugr-ht-logo.png)
A lightweight C11 library providing hash table implementations.

## Technical Overview
- **Language**: C11 (ISO/IEC 9899:2011)
- **Build System**: CMake (minimum 3.10)
- **Components**:
  - `ht`: Core hash table (robin-hood/graveyard/zombie, single-threaded)
  - `htc`: Concurrent cuckoo hash table (lock-free reads, epoch GC, resize)
  - `ht_cache`: Hash table-based cache
- **Testing**: Integrated CTest suite + CBMC formal verification (6 models)
- **Formal verification**: CBMC 6.9 bounded model checking (M1-M8, concurrency C1-C2)
  See `tests/cbmc_htc_extended.c` for full model fidelity matrix.

## Build
```bash
cmake -B build && cmake --build build
```

## Run Tests
```bash
ctest --test-dir build
```

## CBMC Formal Verification
```bash
bash tests/run_cbmc.sh    # runs all 6 CBMC models
```

## Single-threaded HT (ht)
Implements three techniques for O(x) expected operations at load factor 1−1/x:

- **Robin-Hood linear probing** — entries with larger probe distance displace entries with smaller probe distance, keeping probe distances balanced.
- **Graveyard hashing** — prophylactic tombstones placed at evenly-spaced "primitive" positions during rebuilds to break primary clustering (Bender, Kuszmaul, Kuszmaul, FOCS 2021).
- **Zombie hashing** — de-amortized tombstone cleanup via incremental interval rebuilds; one interval of c_b·x slots is rebuilt per insert (Chesetti, Shi, Phillips, Pandey, SIGMOD 2025).

SoA (Structure of Arrays) layout stores hash + probe distance packed into a single `uint64_t` per slot, with values in a parallel array. A spill lane handles edge-case hashes (lower 48 bits < 2).

## Concurrent Cuckoo HT (htc)
Concurrent bucketized cuckoo hash table with:
- Lock-free readers (no shard locks, no allocation in find)
- Per-shard spinlocks for writers (sorted-lock acquisition)
- BFS displacement with bounded budget
- Per-shard adaptive stash (max 32 entries)
- Epoch-based reclamation (64 thread slots)
- Seq-guarded bucket snapshots for reader consistency
- Front cache (128-entry thread-local, ABA-protected by table_id+generation)
- Seeded placement hashing (identity_hash vs placement_hash split)
- CBMC formal verification of core semantics (see tests/cbmc_htc_extended.c)
