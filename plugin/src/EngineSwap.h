#pragma once
/* EngineSwap — per-instance engine state for the singleton firmware engine
   (Phase 4 item 5; design in ARCHITECTURE.md "Instance model").

   The firmware keeps ALL of its state in writable globals (Phase 2 census:
   no heap, no TLS). This component finds those globals in the HOST BINARY's
   own symbol table at runtime, validates them against the committed census
   (EngineCensus.generated.h) — exact (name,size) multiset match, exact
   total bytes, and three anchor symbols whose ELF addresses must equal
   their true runtime addresses — and only then allows EngineHost instances
   to swap their state in/out under a global mutex.

   Fail-safe by construction: if anything does not match (stripped binary,
   non-ELF platform, census drift), available() is false and every caller
   falls back to the Phase 0-3 singleton behaviour, bit-for-bit. */
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class EngineHost;

class EngineSwap
{
public:
    struct Segment { uintptr_t address; size_t offset; size_t size; };

    static EngineSwap& instance();

    // Builds + validates the table on first call; false = stay singleton.
    bool available();
    const std::vector<Segment>& segments() const { return segs; }
    size_t totalBytes() const { return total; }

    void saveTo (std::vector<uint8_t>& blob) const;
    void loadFrom (const std::vector<uint8_t>& blob) const;

    // Pristine post-init snapshot: captured once by the first EngineHost to
    // prepare; every instance's initial state.
    void capturePristineIfNeeded();
    bool hasPristine() const { return ! pristineBlob.empty(); }
    const std::vector<uint8_t>& pristine() const { return pristineBlob; }

    std::mutex mutex;              // serialises activation (see EngineHost)
    EngineHost* owner = nullptr;   // whose state is live; guarded by mutex

private:
    EngineSwap() = default;
    bool buildTable();

    bool attempted = false;
    bool ok = false;
    std::vector<Segment> segs;
    size_t total = 0;
    std::vector<uint8_t> pristineBlob;
};
