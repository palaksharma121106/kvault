# KVault 
### An in-memory key-value store -> built in C++ from scratch

---

## Why I built this

Was working on a database project (RetailSphere) and kept running into the same problem — querying 100K rows every single time was slow. Started reading about how companies like Walmart and Amazon actually handle this at scale and kept seeing one name — **Redis.**

Instead of just installing Redis and using it like a black box, I wanted to actually understand *how* it works internally. So I built my own version of the core idea in C++.

Not trying to replace Redis (lol). Just wanted to learn by building.

---

## What it does

You can store any key-value pair in memory and retrieve it instantly:

```
kvault> SET username palak
OK
kvault> GET username
palak
kvault> TTL username 30
Expires in 30s
kvault> GET username
(nil) [key expired]     ← after 30 seconds
```

Commands supported:
- `SET key value` — store something
- `GET key` — retrieve it
- `DEL key` — delete it
- `TTL key seconds` — auto-delete after N seconds
- `TTLR key` — check how much time is left
- `EXISTS key` — check if a key is there
- `KEYS` — list everything in the store
- `SAVE` — write everything to a file (kvault.db)
- `LOAD` — load back from that file
- `STATS` — see how full the store is

---

## How I built it

The two main data structures working together:

**Hash map** (`unordered_map`) —> gives O(1) lookup for any key. Think of it as the index.

**Doubly linked list** —> tracks which keys were accessed recently. Front = most recently used. Back = least recently used.

When the store hits capacity (50 keys by default), it removes whatever is at the back of the list the key nobody has touched in the longest time. This is the LRU (Least Recently Used) eviction policy.

TTL works using `std::chrono` —> when you set a TTL, I store the exact timestamp when that key should die. Every GET checks if the current time has passed that timestamp.

Persistence (SAVE/LOAD) just writes each key-value pair to a plain text file and reads it back.

---

## How to run

Requirements: g++ with C++17 support (comes with Xcode Command Line Tools on Mac)

```bash
# Compile
g++ -std=c++17 -O2 -o kvault kvault.cpp

# Run
./kvault
```

That's it. No dependencies, no install, just compile and go.

---

## What I learned

- How hash maps and linked lists can work together to solve a real problem
- Why C++ is used for systems like this you control exactly what goes in memory
- How Redis actually handles TTL expiry (lazy deletion — it checks on GET, not on a timer)
- How to think about O(1) operations when designing data structures

---

## What I'd add next

- TCP socket layer so multiple clients can connect simultaneously (how real Redis works)
- Thread safety with mutex locks
- A proper benchmark command to measure ops/second

---

Built by Palak Sharma — B.Tech CSE, Mody University