#include <iostream>
#include <unordered_map>
#include <list>
#include <string>
#include <fstream>
#include <chrono>
#include <sstream>

using namespace std;
using namespace chrono;

// ── Each key-value pair lives in a Node ─────────────────────────────────────
struct Node {
    string key, value;
    Node(string k, string v) : key(k), value(v) {}
};

// ── KVault: the main store ───────────────────────────────────────────────────
class KVault {
private:
    int capacity;
    list<Node> lruList;                               // doubly linked list
    unordered_map<string, list<Node>::iterator> cache;// key → position in list
    unordered_map<string, long long> expiry;          // key → expiry timestamp

    long long now() {
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }

    bool isExpired(const string& key) {
        if (!expiry.count(key)) return false;
        return now() > expiry[key];
    }

    // When store is full, remove the LEAST recently used key (back of list)
    void evict() {
        auto last = lruList.back();
        cache.erase(last.key);
        expiry.erase(last.key);
        lruList.pop_back();
        cout << "  [LRU evicted]: " << last.key << "\n";
    }

    /// move accessed key to front (LRU update)
    void touch(list<Node>::iterator it) {
        lruList.splice(lruList.begin(), lruList, it);
    }

public:
    KVault(int cap = 50) : capacity(cap) {}

    // SET key value ──────────────────────────────────────────────────────────
    string set(const string& key, const string& value) {
        if (cache.count(key)) {
            cache[key]->value = value;
            touch(cache[key]);
            expiry.erase(key);
        } else {
            if ((int)cache.size() >= capacity) evict();
            lruList.push_front(Node(key, value));
            cache[key] = lruList.begin();
        }
        return "OK";
    }

    // GET key ────────────────────────────────────────────────────────────────
    string get(const string& key) {
        if (!cache.count(key)) return "(nil)";
        if (isExpired(key)) { del(key); return "(nil) [key expired]"; }
        touch(cache[key]);
        return cache[key]->value;
    }

    // DEL key(when capacity is reached) ────────────────────────────────────────────────────────────────
    string del(const string& key) {
        if (!cache.count(key)) return "0";
        lruList.erase(cache[key]);
        cache.erase(key);
        expiry.erase(key);
        return "1";
    }

    // TTL key seconds ────────────────────────────────────────────────────────
    string setTTL(const string& key, int seconds) {
        if (!cache.count(key)) return "(error) key not found";
        expiry[key] = now() + (long long)seconds * 1000;
        return "Expires in " + to_string(seconds) + "s";
    }

    // TTLR key — how much time left ──────────────────────────────────────────
    string ttlRemaining(const string& key) {
        if (!cache.count(key)) return "(nil)";
        if (!expiry.count(key)) return "(no expiry set)";
        long long rem = expiry[key] - now();
        if (rem <= 0) { del(key); return "(nil) [just expired]"; }
        return to_string(rem / 1000) + "s remaining";
    }

    // EXISTS key ─────────────────────────────────────────────────────────────
    string exists(const string& key) {
        if (!cache.count(key) || isExpired(key)) return "0";
        return "1";
    }

    // KEYS — list everything ─────────────────────────────────────────────────
    string keys() {
        if (cache.empty()) return "(empty store)";
        string result = "";
        int i = 1;
        for (auto& node : lruList) {
            if (!isExpired(node.key))
                result += "  " + to_string(i++) + ") " + node.key
                        + "  →  " + node.value + "\n";
        }
        return result.empty() ? "(all keys expired)" : result;
    }

    // SAVE — write to disk (persistence) ─────────────────────────────────────
    string save(const string& file = "kvault.db") {
        ofstream f(file);
        if (!f.is_open()) return "(error) cannot open file";
        int count = 0;
        for (auto& node : lruList) {
            if (!isExpired(node.key)) {
                f << node.key << " | " << node.value;
                if (expiry.count(node.key)) f << " " << expiry[node.key];
                f << "\n";
                count++;
            }
        }
        return "Saved " + to_string(count) + " keys → " + file;
    }

    // LOAD — read from disk ───────────────────────────────────────────────────
    string load(const string& file = "kvault.db") {
        ifstream f(file);
        if (!f.is_open()) return "(error) kvault.db not found. Run SAVE first.";
        string line; int count = 0;
        while (getline(f, line)) {
            istringstream ss(line);
            string key, value; long long exp = 0;
            ss >> key >> value;
            set(key, value);
            if (ss >> exp) expiry[key] = exp;
            count++;
        }
        return "Loaded " + to_string(count) + " keys ← " + file;
    }

    // STATS ──────────────────────────────────────────────────────────────────
    void stats() {
        cout << "\n  ┌─ KVault Stats ─────────────────┐\n";
        cout << "  │  Keys stored   : " << cache.size() << "/" << capacity << "\n";
        cout << "  │  Keys with TTL : " << expiry.size() << "\n";
        cout << "  └────────────────────────────────┘\n\n";
    }
};

// ── CLI ──────────────────────────────────────────────────────────────────────
void help() {
    cout << "\n"
    "  SET  <key> <value>    Store a key-value pair\n"
    "  GET  <key>            Retrieve a value\n"
    "  DEL  <key>            Delete a key\n"
    "  TTL  <key> <sec>      Set expiry timer\n"
    "  TTLR <key>            Check time remaining\n"
    "  EXISTS <key>          Check if key exists (1/0)\n"
    "  KEYS                  List all keys\n"
    "  SAVE                  Persist to kvault.db\n"
    "  LOAD                  Load from kvault.db\n"
    "  STATS                 Show store stats\n"
    "  HELP                  This menu\n"
    "  EXIT                  Quit\n\n";
}

int main() {
    KVault store(50);
    cout << "\n"
    "  ╔═══════════════════════════════╗\n"
    "  ║   KVault  v1.0                ║\n"
    "  ║   In-Memory Key-Value Store   ║\n"
    "  ║   Inspired by Redis           ║\n"
    "  ╚═══════════════════════════════╝\n";
    help();

    string line;
    while (true) {
        cout << "kvault> ";
        if (!getline(cin, line)) break;
        if (line.empty()) continue;

        istringstream ss(line);
        string cmd; ss >> cmd;
        for (auto& c : cmd) c = std::toupper(c);

        if (cmd == "EXIT" || cmd == "QUIT") { cout << "Goodbye!\n"; break; }
        else if (cmd == "HELP")   { help(); }
        else if (cmd == "STATS")  { store.stats(); }
        else if (cmd == "KEYS")   { cout << store.keys(); }
        else if (cmd == "SAVE")   { cout << store.save() << "\n"; }
        else if (cmd == "LOAD")   { cout << store.load() << "\n"; }
        else if (cmd == "GET")    { string k; ss >> k; cout << store.get(k) << "\n"; }
        else if (cmd == "DEL")    { string k; ss >> k; cout << store.del(k) << "\n"; }
        else if (cmd == "EXISTS") { string k; ss >> k; cout << store.exists(k) << "\n"; }
        else if (cmd == "SET") {
            string k, v; ss >> k; getline(ss, v);
            if (!v.empty() && v[0]==' ') v = v.substr(1);
            cout << (k.empty()||v.empty() ? "(error) SET <key> <value>" : store.set(k,v)) << "\n";
        }
        else if (cmd == "TTL") {
            string k; int sec; ss >> k >> sec;
            cout << store.setTTL(k, sec) << "\n";
        }
        else if (cmd == "TTLR") {
            string k; ss >> k; cout << store.ttlRemaining(k) << "\n";
        }
        else { cout << "(error) Unknown command. Type HELP\n"; }
    }
    return 0;
}