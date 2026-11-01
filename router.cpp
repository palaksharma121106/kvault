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
const size_t REPLICAS = 2;   // har key ki kitni copies

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

// ek node ko command bhejo aur uska ek-line jawab lo. Fail ho to false.
bool askNode(int fd, string& buf, const string& line, string& reply) {
    return sendAll(fd, line + "\n") && readLine(fd, buf, reply);
}

// kya ye jawab "key mili nahi" jaisa hai? Tab replica bhi try karenge.
bool looksMissing(const string& cmd, const string& reply) {
    if (cmd == "GET")    return reply == "(nil)";
    if (cmd == "EXISTS") return reply == "0";
    return false;
}

// Node se baat karo. Fail ho to connection dobara jodke ek baar aur try karo.
// false = node sach me down hai.
bool askNodeRetry(const string& name, map<string, int>& nodeFd, map<string, string>& nodeBuf,
                  const map<string, int>& nodePorts, const string& line, string& reply) {
    int& fd = nodeFd[name];
    if (fd >= 0 && askNode(fd, nodeBuf[name], line, reply)) return true;

    if (fd >= 0) close(fd);                 // toota hua connection band karo
    nodeBuf[name].clear();
    fd = connectTo(nodePorts.at(name));     // naya connection try
    if (fd < 0) return false;               // connect hi nahi hua = node down
    return askNode(fd, nodeBuf[name], line, reply);
}

void handleClient(int clientFd, const map<string, int>& nodePorts, const HashRing& ring) {
    map<string, int> nodeFd;
    map<string, string> nodeBuf;
    for (auto& [name, port] : nodePorts)
        nodeFd[name] = connectTo(port);     // -1 matlab node abhi down hai, koi baat nahi

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
            // dono copies pe likho; jo node down hai usse skip, client ko pehla successful jawab
            bool gotReply = false;
            for (const string& n : owners) {
                string r;
                if (askNodeRetry(n, nodeFd, nodeBuf, nodePorts, line, r) && !gotReply) {
                    reply = r;
                    gotReply = true;
                }
            }
            if (!gotReply) reply = "(error) all replicas down";
        } else {
            // read: primary se, na mile/down ho to replica se
            bool answered = false;
            for (const string& n : owners) {
                string r;
                if (!askNodeRetry(n, nodeFd, nodeBuf, nodePorts, line, r)) continue;  // down, agla
                if (!answered) { reply = r; answered = true; }       // kam se kam ek jawab mila
                if (!looksMissing(cmd, r)) { reply = r; break; }     // asli jawab mil gaya
            }
            if (!answered) reply = "(error) all replicas down";
        }
        sendAll(clientFd, reply + "\n");
    }

    for (auto& [n, f] : nodeFd) if (f >= 0) close(f);
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