#include "Packet.h"
#include <cassert>
int main() {
    Packet packet;
    packet.clear();
    for (int i = 0; i < 20000; ++i) packet.put('a');
    assert(packet.error && packet.size == 16383);
    assert(packet.sentinel2 == static_cast<int>(0xF00DCAFE));
    assert(packet.buf[16383] == 0);
    packet.clear();
    packet.put_str("M20000000,4:01020304");
    packet.cursor2 = packet.buf;
    packet.take("M20000000,4:");
    unsigned char bytes[4] = {};
    assert(packet.take_blob(bytes, 4));
    for (int i = 0; i < 4; ++i) assert(bytes[i] == i + 1);
    packet.clear();
    packet.put_str("0");
    packet.cursor2 = packet.buf;
    assert(!packet.take_blob(bytes, 1));
    packet.clear();
    assert(!packet.skip(-1));
    packet.clear();
    packet.put_str("ffffffff");
    packet.cursor2 = packet.buf;
    assert(packet.take_hex() == 0xffffffffu);
}
