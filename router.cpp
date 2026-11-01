#include <iostream>
#include <string>
#include <map>
#include <sstream>
#include <thread>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "hashring.h"
using namespace std;

int connectTo(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

bool sendAll(int fd, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.c_str() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Ek poori line padhta hai (\n tak). Jo extra data aaye wo buffer me rehta hai.
bool readLine(int fd, string& buffer, string& line) {
    size_t pos;
    while ((pos = buffer.find('\n')) == string::npos) {
        char tmp[4096];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buffer.append(tmp, n);
    }
    line = buffer.substr(0, pos);
    buffer.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

void handleClient(int clientFd, const map<string, int>& nodePorts, const HashRing& ring) {
    // Is client ke liye har node se apna alag connection (isliye router me lock nahi chahiye)
    map<string, int> nodeFd;
    map<string, string> nodeBuf;
    for (auto& [name, port] : nodePorts) {
        int fd = connectTo(port);
        if (fd < 0) {
            sendAll(clientFd, "(error) cannot reach " + name + "\n");
            for (auto& [n, f] : nodeFd) close(f);
            close(clientFd);
            return;
        }
        nodeFd[name] = fd;
    }

    string buffer, line;
    while (readLine(clientFd, buffer, line)) {
        if (line.empty()) continue;

        istringstream ss(line);
        string cmd, key;
        ss >> cmd >> key;
        for (auto& c : cmd) c = toupper(c);

        if (cmd == "EXIT" || cmd == "QUIT") { sendAll(clientFd, "Goodbye!\n"); break; }

        bool keyed = (cmd == "SET" || cmd == "GET" || cmd == "DEL" ||
                      cmd == "EXISTS" || cmd == "TTL" || cmd == "TTLR");
        if (!keyed || key.empty()) {
            sendAll(clientFd, "(error) router supports: SET GET DEL EXISTS TTL TTLR <key> ...\n");
            continue;
        }

        string node = ring.getNode(key);     // ring batayegi kaun si node
        string reply;
        if (!sendAll(nodeFd[node], line + "\n") || !readLine(nodeFd[node], nodeBuf[node], reply)) {
            sendAll(clientFd, "(error) " + node + " is down\n");
            continue;                         // asli failure handling Step 8 me
        }
        sendAll(clientFd, reply + "\n");
    }

    for (auto& [n, f] : nodeFd) close(f);
    close(clientFd);
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    map<string, int> nodePorts = {{"node1", 6381}, {"node2", 6382}, {"node3", 6383}};
    HashRing ring(500);
    for (auto& [name, port] : nodePorts) ring.addNode(name);

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(6380);
    if (::bind(serverFd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(serverFd, 128) < 0) { perror("listen"); return 1; }
    cout << "Router listening on 6380 -> node1:6381 node2:6382 node3:6383\n";

    while (true) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) { perror("accept"); continue; }
        thread(handleClient, clientFd, cref(nodePorts), cref(ring)).detach();
    }
}