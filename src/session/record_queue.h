// A bounded single-producer, single-consumer queue of journal records.
//
// The PTY thread pushes a copy of each journal record (journal::Tap); the host's storage
// thread pops them. record() never allocates and never waits for room: when the queue is full
// the record is dropped and counted, and the consumer sees the hole as a jump in the stream
// offsets. So that nothing is dropped, the PTY thread calls wait_room() before it takes the
// terminal lock: it waits, bounded, until the next chunk of output fits.
//
// The bytes live in one ring mapped once for the queue's lifetime (os::LazyPages): a page costs
// memory only once a record has been written to it, and release() gives the pages back while the
// queue is empty, so an idle pane holds none of it. No reallocation, no heap. The ring's head and
// tail are atomics; a mutex that nobody else wants on the hot path keeps release() and record()
// apart (release() holds it only while the queue is empty).
//
// Entry layout in the ring (8-byte aligned): u32 len, u8 kind, 3 pad, u64 seq, u64 ts, payload.
// An entry never wraps: when it does not fit before the end of the ring, a wrap marker
// (len = UINT32_MAX) or too little room for a header tells the consumer to go to offset 0.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>

#include "session/journal.h"
#include "session/os.h"

namespace kt::session {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded by the alignas(64) counters, on purpose
#endif
class RecordQueue final : public journal::Tap {
public:
    static constexpr std::size_t kHeader = 24;

    // `capacity` is rounded up to a multiple of 8 and at least 2 x (kHeader + kMaxPayload).
    explicit RecordQueue(std::size_t capacity) {
        std::size_t c = (capacity + 7u) & ~std::size_t{7};
        const std::size_t min = 2 * (kHeader + journal::kMaxPayload + 8);
        if (c < min) c = min;
        mem_ = os::LazyPages(c);
        cap_ = c;
    }

    // Producer side.
    void record(journal::Kind kind, uint64_t seq, uint64_t unix_ms, std::span<const std::byte> payload) override {
        std::lock_guard lk(mu_);  // uncontended but for a release() of an empty queue
        const std::size_t need = kHeader + align8(payload.size());
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        const std::size_t cap = cap_;
        std::size_t pos = static_cast<std::size_t>(tail % cap);
        std::size_t skip = 0;
        if (pos + need > cap) skip = cap - pos;  // goes to the start; the rest of the ring is a gap
        if ((tail - head) + skip + need > cap) {
            dropped_records_.fetch_add(1, std::memory_order_relaxed);
            dropped_bytes_.fetch_add(payload.size(), std::memory_order_relaxed);
            return;
        }
        if (skip) {
            if (skip >= kHeader) put32(mem_.data() + pos, UINT32_MAX);
            pos = 0;
        }
        std::byte* p = mem_.data() + pos;
        put32(p, static_cast<uint32_t>(payload.size()));
        p[4] = static_cast<std::byte>(kind);
        p[5] = p[6] = p[7] = std::byte{0};
        put64(p + 8, seq);
        put64(p + 16, unix_ms);
        if (!payload.empty()) std::memcpy(p + kHeader, payload.data(), payload.size());
        tail_.store(tail + skip + need, std::memory_order_release);
    }

    // Producer side, before record(): the room `bytes` of output in records of at most `chunk`
    // bytes take in the worst case, with a wrap and one more record of another kind.
    std::size_t room_for(std::size_t bytes, std::size_t chunk) const {
        const std::size_t records = chunk ? (bytes + chunk - 1) / chunk : 1;
        const std::size_t need = align8(bytes) + records * (kHeader + 8) + 2 * (kHeader + journal::kMaxPayload + 8);
        return need < cap_ ? need : cap_;
    }
    std::size_t free_bytes() const {
        return cap_ - static_cast<std::size_t>(tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_acquire));
    }
    // Waits until room_for(bytes, chunk) is free, at most `limit`, or until abort_waits().
    // True when the room is there; a wait that ran out is counted as a stall.
    bool wait_room(std::size_t bytes, std::size_t chunk, std::chrono::milliseconds limit) {
        const std::size_t need = room_for(bytes, chunk);
        if (free_bytes() >= need) return true;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        waits_.fetch_add(1, std::memory_order_relaxed);
        for (int spin = 0;; ++spin) {
            if (aborted_.load(std::memory_order_acquire)) return free_bytes() >= need;
            if (free_bytes() >= need) return true;
            if (std::chrono::steady_clock::now() >= deadline) {
                stalls_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (spin < 64) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    // Ends every wait at once and makes later ones return without waiting (the consumer stops).
    void abort_waits() { aborted_.store(true, std::memory_order_release); }

    // Consumer side. Calls fn(kind, seq, ts, payload) for each queued record, at most
    // `max_bytes` of payload per call (at least one record); returns the number of records.
    template <class Fn>
    std::size_t drain(Fn&& fn, std::size_t max_bytes = SIZE_MAX) {
        std::size_t n = 0, bytes = 0;
        uint64_t head = head_.load(std::memory_order_relaxed);
        const std::size_t cap = cap_;
        for (;;) {
            const uint64_t tail = tail_.load(std::memory_order_acquire);
            if (head == tail || (n > 0 && bytes >= max_bytes)) break;
            std::size_t pos = static_cast<std::size_t>(head % cap);
            if (cap - pos < kHeader || get32(mem_.data() + pos) == UINT32_MAX) {
                head += cap - pos;  // wrap
                head_.store(head, std::memory_order_release);
                continue;
            }
            const std::byte* p = mem_.data() + pos;
            const uint32_t len = get32(p);
            const auto kind = static_cast<journal::Kind>(p[4]);
            fn(kind, get64(p + 8), get64(p + 16), std::span<const std::byte>(p + kHeader, len));
            head += kHeader + align8(len);
            head_.store(head, std::memory_order_release);
            ++n;
            bytes += len;
        }
        return n;
    }

    // Consumer side: when the queue is empty and at least `min_bytes` (and at least one record)
    // were queued since the last release, gives every page of the ring back to the OS; the next
    // records take fresh pages. Skips (false) rather than waits when the producer is in record().
    bool release(std::size_t min_bytes) {
        std::unique_lock lk(mu_, std::try_to_lock);
        if (!lk.owns_lock()) return false;
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        // Nothing written since the last release: the pages are gone already. Discarding them
        // again would only hold mu_ (DiscardVirtualMemory is slow) and starve record().
        if (tail == released_at_) return false;
        if (head_.load(std::memory_order_acquire) != tail || tail - released_at_ < min_bytes) return false;
        mem_.discard(0, mem_.size());  // nothing in it is needed: the consumer read all of it
        released_at_ = tail;
        releases_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
    uint64_t dropped_records() const { return dropped_records_.load(std::memory_order_relaxed); }
    uint64_t dropped_bytes() const { return dropped_bytes_.load(std::memory_order_relaxed); }
    std::size_t capacity() const { return cap_; }
    uint64_t releases() const { return releases_.load(std::memory_order_relaxed); }  // release() gave pages back
    const os::LazyPages& memory() const { return mem_; }
    uint64_t waits() const { return waits_.load(std::memory_order_relaxed); }    // wait_room() had to wait
    uint64_t stalls() const { return stalls_.load(std::memory_order_relaxed); }  // ... and ran out of time

private:
    static constexpr std::size_t align8(std::size_t n) { return (n + 7u) & ~std::size_t{7}; }
    static void put32(std::byte* p, uint32_t v) { std::memcpy(p, &v, 4); }
    static void put64(std::byte* p, uint64_t v) { std::memcpy(p, &v, 8); }
    static uint32_t get32(const std::byte* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static uint64_t get64(const std::byte* p) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    os::LazyPages mem_;
    std::size_t cap_ = 0;   // the ring: mem_ may be longer (whole pages)
    std::mutex mu_;         // record() against release()
    uint64_t released_at_ = 0;  // under mu_: the tail at the last release()
    alignas(64) std::atomic<uint64_t> tail_{0};  // written by the producer
    alignas(64) std::atomic<uint64_t> head_{0};  // written by the consumer
    std::atomic<uint64_t> dropped_records_{0}, dropped_bytes_{0};
    std::atomic<uint64_t> waits_{0}, stalls_{0}, releases_{0};
    std::atomic<bool> aborted_{false};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace kt::session
