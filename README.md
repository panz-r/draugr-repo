# draugr

![Draugr Logo](draugr-ht-logo.png)
A lightweight C11 library providing hash table and hash table cache implementations.

## Technical Overview
- **Language**: C11 (ISO/IEC 9899:2011)
- **Build System**: CMake (minimum 3.10)
- **Components**:
  - `ht`: Core hash table implementation
  - `ht_cache`: Hash table-based cache
- **Testing**: Integrated CTest suite

## Build
```bash
cmake -B build && cmake --build build
```

## Run Tests
```bash
ctest --test-dir build
```

## Table Mechanics
Implements three techniques for O(x) expected operations at load factor 1−1/x:

- **Robin-Hood linear probing** — entries with larger probe distance displace entries with smaller probe distance, keeping probe distances balanced.
- **Graveyard hashing** — prophylactic tombstones placed at evenly-spaced "primitive" positions during rebuilds to break primary clustering (Bender, Kuszmaul, Kuszmaul, FOCS 2021).
- **Zombie hashing** — de-amortized tombstone cleanup via incremental interval rebuilds; one interval of c_b·x slots is rebuilt per insert (Chesetti, Shi, Phillips, Pandey, SIGMOD 2025).

SoA (Structure of Arrays) layout stores hash + probe distance packed into a single `uint64_t` per slot, with values in a parallel array. A spill lane handles edge-case hashes (lower 48 bits < 2).
