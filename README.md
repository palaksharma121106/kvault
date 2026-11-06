# KVault
### A distributed in-memory key-value store, built in C++ from scratch

Inspired by Redis. Started as a single-process store with LRU eviction and TTL, then grew into a **3-node cluster** with a TCP server, consistent-hash sharding, replication and failover, all runnable with one `docker compose up`.

```
                    clients (nc, bench)
                            |
                      router  :6380
        (consistent hash ring, 500 virtual nodes per node)
              /             |              \
        node1:6379      node2:6379      node3:6379
         (KVault)        (KVault)        (KVault)
```

Every key lives on **2 nodes**: the primary (owner on the hash ring) and a replica (the next *different* node clockwise on the ring).

---

## Why I built this

I was working on a database project (RetailSphere) and kept running into the same problem: querying 100K rows every single time was slow. I started reading about how companies handle this at scale and kept seeing one name, **Redis**.

Instead of using it as a black box, I wanted to understand how it works inside. So I built the core idea myself, then kept going: network layer, concurrency, sharding, replication, failure handling. Not trying to replace Redis, just learning by building.

---

## Features

| Layer | What it does |
|---|---|
| Store | `unordered_map` + doubly linked list: O(1) get/set, LRU eviction at capacity |
| TTL | Per-key expiry using `std::chrono`; expired keys are removed on access (lazy) or with `CLEAN` |
| Persistence | `SAVE` / `LOAD` to a plain text file (`kvault.db`) |
| Server | Multi-threaded TCP server (POSIX sockets), one thread per client, mutex-protected store |
| Protocol | Newline-terminated text commands, one reply per command |
| Sharding | Consistent hashing with virtual nodes (FNV-1a + a mixing finalizer) |
| Replication | Replication factor 2: writes go to primary and replica |
| Failover | Router falls back to the replica if a node is down or does not have the key, and reconnects when the node returns |
| Deploy | Dockerfile + Docker Compose: 3 nodes + router |
| Benchmark | Custom load generator reporting throughput, p50 and p99 latency |

### Commands

| Command | Meaning | Through router |
|---|---|---|
| `SET key value` | store a value | yes |
| `GET key` | read a value | yes |
| `DEL key` | delete a key | yes |
| `EXISTS key` | 1 if present, else 0 | yes |
| `TTL key seconds` | expire the key after N seconds | yes |
| `TTLR key` | time remaining | yes |
| `KEYS`, `STATS`, `CLEAN`, `SAVE`, `LOAD`, `HELP` | node-local commands (multi-line replies) | no, talk to a node directly |

The router only supports commands with a one-line reply, because the protocol is line-based. Redis solves this with a length-prefixed protocol (RESP).

---

## How it works

**Store.** The hash map gives O(1) lookup. The linked list tracks recency: front is most recently used, back is least. At capacity, the back of the list is evicted. A `GET` moves the key to the front, which means even reads modify shared state.

**Concurrency.** Each client connection gets its own thread. One mutex guards the store, and it is held only around command execution, never around `recv()`, so a silent client cannot block everyone else. Because `GET` also reorders the LRU list, a read/write lock would not be safe here, so a single mutex is used.

**Consistent hashing.** Servers are placed on a hash ring (500 virtual points each in the router). A key belongs to the first server clockwise from the key's hash. Adding or removing a server only moves the keys next to it, instead of reshuffling almost everything like `hash(key) % N`.

**Replication.** For each key the router asks the ring for 2 distinct servers. Writes (`SET`, `DEL`, `TTL`) are sent to both and the client gets the first successful reply (synchronous replication). Reads go to the primary and fall back to the replica.

**Failover.** If a node is unreachable, the router skips it, reconnects on the next request, and for `GET`/`EXISTS` also tries the replica when the primary answers "not found" (a restarted node comes back empty).

---

## How to run

Requirements: a C++17 compiler (`g++` or `clang++`). Docker is only needed for the cluster.

### 1. Single process, interactive CLI

```bash
g++ -std=c++17 -O2 -pthread -o kvault kvault.cpp
./kvault
```

```
kvault> SET username palak
OK
kvault> GET username
palak
kvault> TTL username 30
```

### 2. One TCP server

```bash
./kvault server 6380
nc localhost 6380
```

### 3. Full cluster with Docker

```bash
docker compose up --build
```

This starts `node1`, `node2`, `node3` and the `router`. Talk to the router on port 6380:

```bash
printf "SET apple 1\nSET banana 2\nEXIT\n" | nc localhost 6380
printf "GET apple\nEXIT\n" | nc localhost 6380
```

Stop it with `docker compose down`.

### 4. Cluster without Docker (4 terminals)

```bash
./kvault server 6381
./kvault server 6382
./kvault server 6383
./router        # defaults to the three local nodes above
```

The router also accepts nodes explicitly: `./router node1=host:port node2=host:port ...`

---

## Benchmarks

Load generator: `./bench <clients> <ops_per_client> <port>`. Each client opens its own connection and sends a 50/50 mix of `SET` and `GET` over 500 keys. Latency is measured per request with a monotonic clock.

**Environment:** MacBook Air (Apple M1, 8 GB RAM), client and server on the same machine (localhost), so they compete for the same CPU. Each number is the median of 3 runs.

### Single node, native (no Docker)

| Clients | Throughput (ops/sec) | p50 (µs) | p99 (µs) |
|---|---|---|---|
| 1 | ___ | ___ | ___ |
| 10 | ___ | ___ | ___ |
| 50 | ___ | ___ | ___ |

### Docker: 1 node vs 3-node cluster (10 clients)

| Setup | Throughput (ops/sec) | p50 (µs) | p99 (µs) |
|---|---|---|---|
| 1 node (direct, port 6381) | ___ | ___ | ___ |
| 3 nodes via router (port 6380) | ___ | ___ | ___ |

**Reading the results**

- Latency grows roughly as `clients / throughput` once the server is saturated: extra clients just queue up.
- The 3-node cluster is not faster on one laptop, and that is expected: every request takes an extra hop through the router, every write is applied on two nodes, and all containers share the same CPU. Replication buys availability, and costs throughput.
- I have not profiled the cause of the throughput plateau, so I am not claiming it is the mutex. Candidates are system call overhead, thread switching and the load generator sharing the CPU.

---

## Tests I ran

**Consistent hashing vs modulo** (`ring_test.cpp`, 100,000 keys, 100 virtual nodes):

| Check | Result |
|---|---|
| Key distribution across 3 nodes | 31.8% / 39.6% / 28.6% |
| Keys that moved when a 4th node was added (ring) | ~22.9% (ideal 25%) |
| Keys that moved going from `% 3` to `% 4` (modulo) | ~75.0% |

More virtual nodes give a more even split, at the cost of more memory.

**Sharding and replication.** After inserting 6 keys through the router, querying each node directly showed 12 entries in total: every key on exactly 2 different nodes.

**Failover.** With the cluster running, one node was stopped (`docker compose stop node2`). All 6 keys were still readable through the router and new writes succeeded. After restarting the node, reads for its keys were still answered from the replica.

---

## Known limitations

These are real, and I ran into the first one while testing.

1. **No re-sync after restart.** The store is in memory, so a restarted node comes back empty and nothing refills it. If a second node fails before that, keys that lived only on those two nodes are lost. Replication factor 2 survives one failure, not two in a row.
2. **Writes during an outage are not replayed.** A node that was down misses writes made in the meantime.
3. **Best-effort replication.** If the replica write fails, the client still gets the primary's reply, so copies can diverge.
4. **The router is a single process.** It is a single point of failure and adds one network hop.
5. **Static membership.** Nodes are fixed at startup. The ring supports adding and removing nodes, but the router does not move data around when the set changes.
6. **Limited routed commands** because of the line-based protocol (see Commands).
7. **One global mutex per node**, so commands on a node run one at a time.

## What I'd build next

- Read repair or a re-sync step when a node rejoins
- A length-prefixed protocol so multi-line replies and binary values work through the router
- Asynchronous replication, and measuring the throughput/durability trade-off
- Dynamic membership with data rebalancing
- `epoll`/`kqueue` instead of a thread per client

## What I learned

- How a hash map and a linked list combine to give an O(1) LRU cache
- TCP is a byte stream, not messages: you have to frame commands yourself and handle partial reads
- Why locks should be as small as possible, and why even reads can mutate shared state
- Why consistent hashing beats `hash % N` when servers come and go
- Redis expires keys lazily on access and also actively in the background; KVault implements the lazy part (plus a manual `CLEAN`)
- How to benchmark honestly: percentiles instead of averages, repeated runs, and saying what the numbers do not show
- Replication trades throughput for availability, and testing failures showed me exactly where my design is weak

---

## Project layout

```
kvault.cpp           store + CLI + TCP server
router.cpp           sharding and replication router
hashring.h           consistent hash ring
bench.cpp            load generator
ring_test.cpp        hashing distribution and key-movement test
Dockerfile
docker-compose.yml
```

Built by Palak Sharma, B.Tech CSE, Mody University