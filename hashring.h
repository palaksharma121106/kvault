#pragma once
#include <map>
#include <string>
#include <cstdint>

class HashRing {
    std::map<uint64_t, std::string> ring;   // ring ki jagah -> server ka naam
    int vnodes;

public:
    explicit HashRing(int virtualNodes = 100) : vnodes(virtualNodes) {}

    // FNV-1a hash + ek finalizer. std::hash use nahi kiya kyunki wo alag
    // compilers/machines pe alag answer de sakta hai, aur hume har node pe same chahiye.
    static uint64_t hashKey(const std::string& s) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        // finalizer: milte-julte strings (key1, key2) ko ring pe failata hai
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    void addNode(const std::string& node) {
        for (int i = 0; i < vnodes; i++)
            ring[hashKey(node + "#" + std::to_string(i))] = node;
    }

    void removeNode(const std::string& node) {
        for (int i = 0; i < vnodes; i++)
            ring.erase(hashKey(node + "#" + std::to_string(i)));
    }

    // key ka malik: ring pe key ke hash se aage pehla server
    std::string getNode(const std::string& key) const {
        if (ring.empty()) return "";
        auto it = ring.lower_bound(hashKey(key));
        if (it == ring.end()) it = ring.begin();   // ring ghoom ke shuru pe aa gaya
        return it->second;
    }

    bool empty() const { return ring.empty(); }
};