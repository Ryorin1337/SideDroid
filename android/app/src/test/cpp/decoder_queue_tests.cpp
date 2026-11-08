#include <cstdlib>
#include <iostream>

#include "../../main/cpp/decoder_queue.h"

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void push(sd::DecoderQueue& q, uint32_t id, bool key, uint64_t at) {
    const uint8_t payload = static_cast<uint8_t>(id);
    q.push(sd::FrameHeader{id, id * 1000ull, key}, &payload, 1, at);
}
}  // namespace

int main() {
    sd::DecoderQueue q;
    q.configure(60);
    push(q, 1, false, 0);
    check(q.empty() && q.awaitingKey(), "P frames must wait for a keyframe");
    push(q, 2, true, 1000);
    push(q, 3, false, 2000);
    auto first = q.take(3000);
    auto second = q.take(3000);
    check(first && first->header.frameId == 2 && first->data[0] == 2, "keyframe must be copied and delivered first");
    check(second && second->header.frameId == 3, "healthy reference chain must preserve order");

    q.reset();
    check(q.drops() == 0, "surface and configuration resets must not report overload");
    const uint64_t beforeOverflow = q.generation();
    push(q, 4, true, 4000);
    push(q, 5, false, 5000);
    push(q, 6, false, 6000);
    push(q, 7, false, 7000);
    check(q.empty() && q.awaitingKey() && q.generation() != beforeOverflow,
          "overflow must discard the whole reference chain and invalidate in-flight work");
    check(q.drops() == 1, "overflow must report decoder overload");
    push(q, 8, false, 8000);
    check(q.empty(), "P frames after a dropped reference must stay discarded");
    push(q, 9, true, 9000);
    check(q.take(10000)->header.frameId == 9, "fresh keyframe must recover after overflow");

    q.reset();
    push(q, 10, true, 10000);
    push(q, 11, false, 11000);
    push(q, 12, false, 12000);
    push(q, 13, true, 13000);
    auto recovered = q.take(14000);
    check(recovered && recovered->header.frameId == 13 && q.empty(),
          "an arriving keyframe can replace an overflowing reference chain");

    q.reset();
    push(q, 14, true, 20000);
    push(q, 15, false, 21000);
    check(!q.take(20000 + q.maxAgeUs()) && q.awaitingKey(),
          "expired queued frames must be dropped at the dequeue boundary");
    push(q, 16, true, 100000);
    push(q, 17, false, 100000 + q.maxAgeUs());
    check(q.empty() && q.awaitingKey(), "enqueue must detect an expired reference chain too");

    push(q, 18, true, 200000);
    const uint64_t beforeConfig = q.generation();
    q.configure(120);
    check(q.empty() && q.awaitingKey() && q.generation() != beforeConfig,
          "reconfiguration must invalidate both queued and in-flight frames");
    check(q.maxAgeUs() == 16666, "120 Hz queue age should allow two frame intervals");
    q.configure(1);
    check(q.maxAgeUs() == 50000, "low frame rates must not permit an unbounded queue age");
    q.configure(1000);
    check(q.maxAgeUs() == 16000, "high frame rates need a practical scheduling floor");
    std::cout << "decoder queue tests passed\n";
}
