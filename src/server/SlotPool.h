#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace morpheus::server {

// The chat server's slots, shared by every run of morpheus_server (spec, section 2). Slot 0 is the upkeep slot: the
// calls of folding and condensing. The others go to conversations, the least recently used first: with more
// conversations than slots, the conversation used least recently loses its slot, and its next turn takes one again
// (its prompt is then read again instead of reused from the slot's cache). With a single slot, upkeep and every
// conversation share slot 0. Thread-safe.
class SlotPool {
public:
    static constexpr int UPKEEP_SLOT = 0;

private:
    // Who holds a conversation slot, and when it last used it.
    struct Seat {
        std::string key = {};      // The holding conversation's key; empty when the slot is free.
        std::uint64_t lastUse = 0; // _clock at its holder's last Acquire.
    };

    mutable std::mutex _mutex = {};
    int _slotCount = 0;
    std::vector<Seat> _seats = {}; // Indexed by slot. Seat 0 (upkeep) is never handed to a conversation when there are more.
    std::uint64_t _clock = 0;      // Counts Acquire calls: the order of use.

public:
    // Throws ContractError for a count below 1.
    explicit SlotPool(int slotCount);

    int SlotCount() const;

    // The slot of conversation `key` (its run's and its own id, e.g. "save_1/c2"), marked as used now: the slot it
    // holds, else a free conversation slot, else the slot of the conversation used least recently, which loses it.
    // With one slot: 0. Throws ContractError for an empty key.
    int Acquire(const std::string& key);

    // Frees conversation `key`'s slot, if it holds one.
    void Release(const std::string& key);

    // The key of the conversation holding `slot`; "" when it is free, and always for slot 0. Throws ContractError for
    // a slot out of range.
    std::string HolderOf(int slot) const;
};

} // namespace morpheus::server
