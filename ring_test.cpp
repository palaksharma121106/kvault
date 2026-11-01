#include <iostream>
#include <map>
#include <vector>
#include "hashring.h"
using namespace std;

int main() {
    const int N = 100000;
    HashRing ring(500);
    ring.addNode("node1");
    ring.addNode("node2");
    ring.addNode("node3");

    // Test 1: keys 3 servers me kitni barabar baant rahi hain?
    map<string, int> count;
    vector<string> before(N);
    for (int i = 0; i < N; i++) {
        before[i] = ring.getNode("key" + to_string(i));
        count[before[i]]++;
    }
    cout << "Distribution (3 nodes, " << N << " keys):\n";
    for (auto& [node, c] : count)
        cout << "  " << node << ": " << c << "  (" << c * 100.0 / N << "%)\n";

    // Test 2: 4th server add karo, kitni keys hili?
        ring.addNode("node4");
    vector<string> withNode4(N);
    int moved = 0;
    for (int i = 0; i < N; i++) {
        withNode4[i] = ring.getNode("key" + to_string(i));
        if (withNode4[i] != before[i]) moved++;
    }
    cout << "\nConsistent hashing: node4 add karne pe "
         << moved * 100.0 / N << "% keys hili (ideal ~25%)\n";

    // Test 3: purana tareeka (hash % servers) ke saath compare
    int movedMod = 0;
    for (int i = 0; i < N; i++) {
        uint64_t h = HashRing::hashKey("key" + to_string(i));
        if (h % 3 != h % 4) movedMod++;
    }
    cout << "Modulo (% 3 se % 4): " << movedMod * 100.0 / N << "% keys hili\n";

    // Test 4: node2 hata do, kya sirf node2 ki keys hili?
        // Test 4: node2 hata do, kya sirf node2 ki keys hili?
    ring.removeNode("node2");
    int rightMoves = 0, wrongMoves = 0;
    for (int i = 0; i < N; i++) {
        string now = ring.getNode("key" + to_string(i));
        if (withNode4[i] == "node2") rightMoves++;        // node2 ki keys ko hilna hi tha
        else if (now != withNode4[i]) wrongMoves++;       // kisi aur ki key hili, ye galat hai
    }
    cout << "\nnode2 hata diya: " << rightMoves << " keys naye owner pe gayi\n";
    cout << "Doosre nodes ki keys jo hili (0 honi chahiye): " << wrongMoves << "\n";
}