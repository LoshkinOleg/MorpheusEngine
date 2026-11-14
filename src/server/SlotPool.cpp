#include "server/SlotPool.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "core/Errors.h"

namespace morpheus::server {

SlotPool::SlotPool(int slotCount)
    : _slotCount(slotCount)
    , _seats(static_cast<std::size_t>(std::max(slotCount, 0)))
{
    MORPHEUS_REQUIRE(slotCount >= 1, std::format("SlotPool: a server has at least one slot, not {}", slotCount));
}

int SlotPool::SlotCount() const
{
    return _slotCount;
}

int SlotPool::Acquire(const std::string& key)
{
    MORPHEUS_REQUIRE(!key.empty(), "SlotPool::Acquire: a conversation needs a key");
    std::scoped_lock lock(_mutex);
    // One slot: upkeep and every conversation share it.
    if (_slotCount == 1) {
        return UPKEEP_SLOT;
    }
    ++_clock;

    // The conversation's own slot, else a free one, else the one used least recently.
    int chosen = -1;
    for (int slot = 1; slot < _slotCount && chosen < 0; ++slot) {
        if (_seats[static_cast<std::size_t>(slot)].key == key) {
            chosen = slot;
        }
    }
    for (int slot = 1; slot < _slotCount && chosen < 0; ++slot) {
        if (_seats[static_cast<std::size_t>(slot)].key.empty()) {
            chosen = slot;
        }
    }
    if (chosen < 0) {
        chosen = 1;
        for (int slot = 2; slot < _slotCount; ++slot) {
            if (_seats[static_cast<std::size_t>(slot)].lastUse < _seats[static_cast<std::size_t>(chosen)].lastUse) {
                chosen = slot;
            }
        }
    }
    Seat& seat = _seats[static_cast<std::size_t>(chosen)];
    seat.key = key;
    seat.lastUse = _clock;
    return chosen;
}

void SlotPool::Release(const std::string& key)
{
    std::scoped_lock lock(_mutex);
    for (Seat& seat : _seats) {
        if (seat.key == key) {
            seat = Seat{};
        }
    }
}

std::string SlotPool::HolderOf(int slot) const
{
    MORPHEUS_REQUIRE(slot >= 0 && slot < _slotCount,
                     std::format("SlotPool::HolderOf: slot {} is not one of the {} slots", slot, _slotCount));
    std::scoped_lock lock(_mutex);
    return _seats[static_cast<std::size_t>(slot)].key;
}

} // namespace morpheus::server
