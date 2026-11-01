#include <iostream>
#include <unordered_map>
#include <list>
#include <string>
#include <fstream>
#include <chrono>
#include <sstream>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <thread>
#include <mutex>

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
        cerr << "  [LRU evicted]: " << last.key << "\n";
    }

    /// move accessed key to front (LRU update)
    void touch(list<Node>::iterator it) {
        lruList.splice(lruList.begin(), lruList, it);
    }

public:
    KVault(int cap = 50) : capacity(cap) {}

    // CLEAN — remove all expired keys, returns how many were removed ─────────
    int cleanExpired() {
        int removed = 0;
        for (auto it = lruList.begin(); it != lruList.end(); ) {
            string k = it->key;
            ++it;                       // move on BEFORE deleting this node
            if (isExpired(k)) { del(k); removed++; }
        }
        return removed;
    }

    // SET key value ──────────────────────────────────────────────────────────
    string set(const string& key, const string& value) {
        if (cache.count(key)) {
            cache[key]->value = value;
            touch(cache[key]);
            expiry.erase(key);
        } else {
            if ((int)cache.size() >= capacity) {
                cleanExpired();                          // free dead keys first
                if ((int)cache.size() >= capacity) evict(); // still full → LRU
            }
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

    // DEL key ────────────────────────────────────────────────────────────────
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
    // File format, one key per line:  key | value | expiryTimestamp (0 = none)
    string save(const string& file = "kvault.db") {
        ofstream f(file);
        if (!f.is_open()) return "(error) cannot open file";
        int count = 0;
        for (auto& node : lruList) {
            if (!isExpired(node.key)) {
                f << node.key << " | " << node.value << " | "
                  << (expiry.count(node.key) ? expiry[node.key] : 0) << "\n";
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
            size_t a = line.find(" | ");    // first separator  (after key)
            size_t b = line.rfind(" | ");   // last separator   (before expiry)
            if (a == string::npos || a == b) continue;   // bad line → skip

            string key   = line.substr(0, a);
            string value = line.substr(a + 3, b - (a + 3));
            long long exp = 0;
            try { exp = stoll(line.substr(b + 3)); }
            catch (...) { continue; }       // bad expiry → skip line

            set(key, value);
            if (exp > 0) expiry[key] = exp;
            count++;
        }
        return "Loaded " + to_string(count) + " keys ← " + file;
    }

        // STATS ──────────────────────────────────────────────────────────────────
    string stats() {
        string s = "  ┌─ KVault Stats ─────────────────┐\n";
        s += "  │  Keys stored   : " + to_string(cache.size()) + "/" + to_string(capacity) + "\n";
        s += "  │  Keys with TTL : " + to_string(expiry.size()) + "\n";
        s += "  └────────────────────────────────┘";
        return s;
    }

   
};

// ── CLI ──────────────────────────────────────────────────────────────────────
string helpText() {
    return "\n"
    "  SET  <key> <value>    Store a key-value pair\n"
    "  GET  <key>            Retrieve a value\n"
    "  DEL  <key>            Delete a key\n"
    "  TTL  <key> <sec>      Set expiry timer\n"
    "  TTLR <key>            Check time remaining\n"
    "  EXISTS <key>          Check if key exists (1/0)\n"
    "  KEYS                  List all keys\n"
    "  CLEAN                 Remove all expired keys\n"
    "  SAVE                  Persist to kvault.db\n"
    "  LOAD                  Load from kvault.db\n"
    "  STATS                 Show store stats\n"
    "  HELP                  This menu\n"
    "  EXIT                  Quit\n";
}

// keys() ke end me extra newline hota hai, usko hata dete hain
string trimNewline(string s) {
    while (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

// Ek command line leta hai, store pe chalata hai, jawab STRING me return karta hai.
// quit = true ho jaye to caller ko pata chal jaata hai ki connection/loop band karna hai.
string execute(KVault& store, const string& line, bool& quit) {
    istringstream ss(line);
    string cmd; ss >> cmd;
    for (auto& c : cmd) c = std::toupper(c);

    if (cmd == "EXIT" || cmd == "QUIT") { quit = true; return "Goodbye!"; }
    if (cmd == "HELP")   return trimNewline(helpText());
    if (cmd == "STATS")  return store.stats();
    if (cmd == "KEYS")   return trimNewline(store.keys());
    if (cmd == "CLEAN")  return "Removed " + to_string(store.cleanExpired()) + " expired keys";
    if (cmd == "SAVE")   return store.save();
    if (cmd == "LOAD")   return store.load();
    if (cmd == "GET")    { string k; ss >> k; return store.get(k); }
    if (cmd == "DEL")    { string k; ss >> k; return store.del(k); }
    if (cmd == "EXISTS") { string k; ss >> k; return store.exists(k); }
    if (cmd == "TTLR")   { string k; ss >> k; return store.ttlRemaining(k); }
    if (cmd == "SET") {
        string k, v; ss >> k; getline(ss, v);
        if (!v.empty() && v[0] == ' ') v = v.substr(1);
        if (k.empty() || v.empty()) return "(error) SET <key> <value>";
        return store.set(k, v);
    }
    if (cmd == "TTL") {
        string k; int sec = 0; ss >> k >> sec;
        if (k.empty() || sec <= 0) return "(error) TTL <key> <seconds>";
        return store.setTTL(k, sec);
    }
    return "(error) Unknown command. Type HELP";
}

// ── TCP SERVER ───────────────────────────────────────────────────────────────

bool sendAll(int fd, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.c_str() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Ek client ko handle karta hai. Har client ke liye alag thread me chalta hai.
void handleClient(int clientFd, KVault& store, mutex& storeMutex) {
    string buffer;
    char tmp[1024];
    bool quit = false;

    while (!quit) {
        ssize_t n = recv(clientFd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buffer.append(tmp, n);

        size_t pos;
        while (!quit && (pos = buffer.find('\n')) != string::npos) {
            string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            string reply;
            {
                lock_guard<mutex> lock(storeMutex);   // yahan se lock
                reply = execute(store, line, quit);
            }                                         // yahan lock khul gaya
            if (!sendAll(clientFd, reply + "\n")) quit = true;
        }
    }
    close(clientFd);
    cout << "Client disconnected\n";
}

void runServer(KVault& store, int port) {
    signal(SIGPIPE, SIG_IGN);   // client beech me kat jaye to server crash na ho

    // 1. socket banao
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) { perror("socket"); return; }

    // 2. restart karne pe "Address already in use" na aaye
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. address bharo
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // 4. bind + listen
    if (::bind(serverFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(serverFd); return;
    }
    if (listen(serverFd, 10) < 0) {
        perror("listen"); close(serverFd); return;
    }
    cout << "KVault server listening on port " << port << "\n";

    // 5. clients ka intezaar (ek time pe ek client)
       mutex storeMutex;   // sab clients ke liye ek hi lock

    while (true) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) { perror("accept"); continue; }
        cout << "Client connected\n";

        thread(handleClient, clientFd, ref(store), ref(storeMutex)).detach();
    }
        
}

int main(int argc, char* argv[]) {
    KVault store(50);

    if (argc > 1 && string(argv[1]) == "server") {
        runServer(store, 6380);
        return 0;
    }
    cout << "\n"
    "  ╔═══════════════════════════════╗\n"
    "  ║   KVault  v1.1                ║\n"
    "  ║   In-Memory Key-Value Store   ║\n"
    "  ║   Inspired by Redis           ║\n"
    "  ╚═══════════════════════════════╝\n";
    cout << helpText();

    string line;
    while (true) {
        cout << "kvault> ";
        if (!getline(cin, line)) break;
        if (line.empty()) continue;

        bool quit = false;
        string reply = execute(store, line, quit);
        cout << reply << "\n";
        if (quit) break;
    }
    return 0;
}