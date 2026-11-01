#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <thread>
#include <cctype>
#include <cstdlib>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include "hashring.h"
using namespace std;

const size_t REPLICAS = 2;   // har key ki kitni copies

struct NodeAddr { string host; int port; };

// host naam ("node1") ya IP ("127.0.0.1"), dono chalte hain
int connectTo(const string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) { close(fd); fd = -1; }
    freeaddrinfo(res);
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

bool askNode(int fd, string& buf, const string& line, string& reply) {
    return sendAll(fd, line + "\n") && readLine(fd, buf, reply);
}

bool looksMissing(const string& cmd, const string& reply) {
    if (cmd == "GET")    return reply == "(nil)";
    if (cmd == "EXISTS") return reply == "0";
    return false;
}

bool askNodeRetry(const string& name, map<string, int>& nodeFd, map<string, string>& nodeBuf,
                  const map<string, NodeAddr>& nodes, const string& line, string& reply) {
    int& fd = nodeFd[name];
    if (fd >= 0 && askNode(fd, nodeBuf[name], line, reply)) return true;

    if (fd >= 0) close(fd);
    nodeBuf[name].clear();
    const NodeAddr& a = nodes.at(name);
    fd = connectTo(a.host, a.port);
    if (fd < 0) return false;
    return askNode(fd, nodeBuf[name], line, reply);
}

void handleClient(int clientFd, const map<string, NodeAddr>& nodes, const HashRing& ring) {
    map<string, int> nodeFd;
    map<string, string> nodeBuf;
    for (auto& [name, addr] : nodes)
        nodeFd[name] = connectTo(addr.host, addr.port);   // -1 = node abhi down

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

        vector<string> owners = ring.getNodes(key, REPLICAS);
        bool isWrite = (cmd == "SET" || cmd == "DEL" || cmd == "TTL");
        string reply;

        if (isWrite) {
            bool gotReply = false;
            for (const string& n : owners) {
                string r;
                if (askNodeRetry(n, nodeFd, nodeBuf, nodes, line, r) && !gotReply) {
                    reply = r;
                    gotReply = true;
                }
            }
            if (!gotReply) reply = "(error) all replicas down";
        } else {
            bool answered = false;
            for (const string& n : owners) {
                string r;
                if (!askNodeRetry(n, nodeFd, nodeBuf, nodes, line, r)) continue;
                if (!answered) { reply = r; answered = true; }
                if (!looksMissing(cmd, r)) { reply = r; break; }
            }
            if (!answered) reply = "(error) all replicas down";
        }
        sendAll(clientFd, reply + "\n");
    }

    for (auto& [n, f] : nodeFd) if (f >= 0) close(f);
    close(clientFd);
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    // Nodes command line se aate hain: naam=host:port  (jaise node1=node1:6379)
    // Kuch na diya to purane local defaults.
    map<string, NodeAddr> nodes;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        size_t eq = a.find('='), colon = a.rfind(':');
        if (eq == string::npos || colon == string::npos || colon < eq) {
            cerr << "Usage: ./router name=host:port name=host:port ...\n";
            return 1;
        }
        nodes[a.substr(0, eq)] = NodeAddr{a.substr(eq + 1, colon - eq - 1),
                                          atoi(a.substr(colon + 1).c_str())};
    }
    if (nodes.empty()) {
        nodes["node1"] = NodeAddr{"127.0.0.1", 6381};
        nodes["node2"] = NodeAddr{"127.0.0.1", 6382};
        nodes["node3"] = NodeAddr{"127.0.0.1", 6383};
    }

    HashRing ring(500);
    for (auto& [name, addr] : nodes) ring.addNode(name);

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(6380);
    if (::bind(serverFd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(serverFd, 128) < 0) { perror("listen"); return 1; }
    cout << "Router listening on 6380 with " << nodes.size() << " nodes\n";

    while (true) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) { perror("accept"); continue; }
        thread(handleClient, clientFd, cref(nodes), cref(ring)).detach();
    }
}