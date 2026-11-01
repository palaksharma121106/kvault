#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using namespace std;
using Clock = chrono::steady_clock;

int connectTo(const string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

// ek command bhejo, aur ek poori line (\n tak) jawab padho
bool request(int fd, const string& cmd) {
    string msg = cmd + "\n";
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = send(fd, msg.c_str() + sent, msg.size() - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    string reply;
    char tmp[4096];
    while (reply.find('\n') == string::npos) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        reply.append(tmp, n);
    }
    return true;
}

// Har worker ek fake client hai. Apni latencies (microseconds) apne vector me rakhta hai.
void worker(int id, const string& host, int port, int ops,
            vector<double>& latencies, int& errors) {
    int fd = connectTo(host, port);
    if (fd < 0) { errors = ops; return; }
    latencies.reserve(ops);

    for (int i = 0; i < ops; i++) {
        string key = "key" + to_string(id * 1000 + (i / 2) % 500);
        string cmd = (i % 2 == 0) ? "SET " + key + " value" + to_string(i)
                                  : "GET " + key;

        auto t0 = Clock::now();
        bool ok = request(fd, cmd);
        auto t1 = Clock::now();

        if (!ok) { errors++; break; }
        latencies.push_back(chrono::duration<double, micro>(t1 - t0).count());
    }
    close(fd);
}

int main(int argc, char* argv[]) {
    int clients = (argc > 1) ? atoi(argv[1]) : 10;
    int ops     = (argc > 2) ? atoi(argv[2]) : 10000;
    string host = "127.0.0.1";
    int port    = (argc > 3) ? atoi(argv[3]) : 6380;

    vector<vector<double>> lat(clients);
    vector<int> errs(clients, 0);
    vector<thread> threads;

    auto start = Clock::now();
    for (int i = 0; i < clients; i++)
        threads.emplace_back(worker, i, cref(host), port, ops, ref(lat[i]), ref(errs[i]));
    for (auto& t : threads) t.join();
    double secs = chrono::duration<double>(Clock::now() - start).count();

    vector<double> all;
    for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
    int totalErrors = accumulate(errs.begin(), errs.end(), 0);

    if (all.empty()) { cout << "No successful requests. Server chal raha hai?\n"; return 1; }
    sort(all.begin(), all.end());
    auto pct = [&](double p) { return all[(size_t)(p * (all.size() - 1))]; };
    double avg = accumulate(all.begin(), all.end(), 0.0) / all.size();

    cout << "Clients: " << clients << "   Ops/client: " << ops << "\n";
    cout << "Total ok: " << all.size() << "   Errors: " << totalErrors << "\n";
    cout << "Time:     " << secs << " s\n";
    cout << "Throughput: " << (size_t)(all.size() / secs) << " ops/sec\n";
    cout << "Latency (us): avg " << avg << "  p50 " << pct(0.50)
         << "  p99 " << pct(0.99) << "  max " << all.back() << "\n";
    return 0;
}