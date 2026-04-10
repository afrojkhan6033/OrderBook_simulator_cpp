#pragma once
// =============================================================================
// MarketTypes.h  v3  —  Shared wire types for the HFT OrderBook Engine
//
// Include order in every TU:
//   1. #include "MarketMicrostructure.h"   (defines MicrostructureResults)
//   2. #include "MarketTypes.h"            (uses MicrostructureResults)
//
// DO NOT include MarketMicrostructure.h inside this header — keep it clean.
// =============================================================================

#include "Constants.h"   // Price = int64_t, Quantity = int64_t

#include <vector>
#include <functional>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <limits>
#include <utility>

// Forward declaration — full definition in MarketMicrostructure.h
// Must be included BEFORE MarketTypes.h in the TU for CopyFromMicro to work.
struct MicrostructureResults;

// ============================================================================
// ULTRA-FAST: Sorted vector-based price level map
//
// Replaces std::map<Price, Quantity> with a sorted vector for:
//   - Cache-friendly iteration (contiguous memory)
//   - O(log n) binary search for lookups
//   - O(1) amortized insertion (insertion sort for small deltas)
//   - O(1) access to begin() (front of vector)
//
// For typical order books with <2000 price levels, this is 2-3x faster
// than std::map due to better cache locality.
// ============================================================================
template<typename ValueComparator = std::greater<Price>>
class FastPriceMap {
private:
    using KeyValuePair = std::pair<Price, Quantity>;
    std::vector<KeyValuePair> data_;
    ValueComparator comp_;  // Comparator (greater for bids, less for asks)

    // Binary search to find insertion point or existing key
    int FindKey(Price key) const {
        int low = 0;
        int high = static_cast<int>(data_.size()) - 1;

        while (low <= high) {
            int mid = (low + high) / 2;
            if (data_[mid].first == key) {
                return mid;
            } else if (comp_(data_[mid].first, key)) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        return -low - 1;  // Negative = insertion point
    }

public:
    FastPriceMap() = default;

    // operator[] - insert or update
    Quantity& operator[](Price key) {
        int idx = FindKey(key);
        if (idx >= 0) {
            return data_[idx].second;
        }
        int insertPos = -idx - 1;
        data_.insert(data_.begin() + insertPos, {key, 0});
        return data_[insertPos].second;
    }

    // Erase a key
    void erase(Price key) {
        int idx = FindKey(key);
        if (idx >= 0) {
            data_.erase(data_.begin() + idx);
        }
    }

    // Check if empty
    bool empty() const { return data_.empty(); }

    // Get size
    size_t size() const { return data_.size(); }

    // Begin/end iterators (compatible with std::map iteration)
    auto begin() const { return data_.begin(); }
    auto end() const { return data_.end(); }
    auto begin() { return data_.begin(); }
    auto end() { return data_.end(); }

    // Clear all
    void clear() { data_.clear(); }

    // Copy from another FastPriceMap
    FastPriceMap& operator=(const FastPriceMap& other) {
        data_ = other.data_;
        comp_ = other.comp_;
        return *this;
    }
};

// =============================================================================
// PriceLevelSnapshot
//   bids: FastPriceMap with std::greater (highest price first)
//   asks: FastPriceMap with std::less (lowest price first)
// =============================================================================
struct PriceLevelSnapshot {
    FastPriceMap<std::greater<Price>> bids;
    FastPriceMap<std::less<Price>> asks;
    uint64_t sequenceNumber = 0;
    std::chrono::high_resolution_clock::time_point timestamp;
};

// =============================================================================
// PerformanceMetrics  (all numeric fields are atomic for lock-free cross-thread reads)
//
// BinarySerializer::SerializeStats() calls .load() on:
//   messagesReceived, messagesProcessed, messagesDropped,
//   totalProcessingTimeUs, maxProcessingTimeUs, minProcessingTimeUs,
//   lastSpreadBps, currentMidPrice, sequenceNumber
// These MUST remain std::atomic.
// =============================================================================
struct PerformanceMetrics {
    std::atomic<uint64_t> messagesReceived{0};
    std::atomic<uint64_t> messagesProcessed{0};
    std::atomic<uint64_t> messagesDropped{0};
    std::atomic<uint64_t> totalProcessingTimeUs{0};
    std::atomic<uint64_t> maxProcessingTimeUs{0};
    std::atomic<uint64_t> minProcessingTimeUs{UINT64_MAX};
    std::atomic<double>   lastSpreadBps{0.0};
    std::atomic<double>   currentMidPrice{0.0};
    std::atomic<uint64_t> sequenceNumber{0};

    // Plain doubles — only touched on the main processing thread
    double lastNetworkLatencyUs = 0.0;

    double GetAverageLatencyMicros() const {
        uint64_t p = messagesProcessed.load();
        return p > 0
            ? static_cast<double>(totalProcessingTimeUs.load()) / static_cast<double>(p)
            : 0.0;
    }
};

// =============================================================================
// MarketAnalytics  — instantiate ONCE, reuse every tick (persistent state)
//
// Calculate()      fills core book fields from snapshot
// CopyFromMicro()  fills microstructure fields from MicrostructureResults
// =============================================================================
struct MarketAnalytics {
    // ── Core book analytics (filled by Calculate) ─────────────────────────────
    double midPrice   = 0.0;
    double spreadBps  = 0.0;
    double imbalance  = 0.0;     // top-of-book qty imbalance in [-1, +1]
    double ofi        = 0.0;     // rolling multi-level OFI (set externally)
    double microprice = 0.0;     // Stoikov WMP (also filled by CopyFromMicro)

    // ── Microstructure analytics (filled by CopyFromMicro) ────────────────────
    double   vwap           = 0.0;
    double   twap           = 0.0;
    double   kyleLambda     = 0.0;
    double   amihudIlliq    = 0.0;
    double   rollSpread     = 0.0;
    double   signedFlow     = 0.0;
    int      lastTradeSide  = 0;   // +1=BUY / -1=SELL / 0=unknown
    double   lastTradePrice = 0.0;
    double   lastTradeQty   = 0.0;
    uint64_t tradeCount     = 0;

    // ── Core calculation: call every tick after GetCurrentSnapshotCopy() ───────
    void Calculate(const PriceLevelSnapshot &snap) {
        if (snap.bids.empty() || snap.asks.empty()) return;

        double bestBid = static_cast<double>(snap.bids.begin()->first)  / 10000.0;
        double bestAsk = static_cast<double>(snap.asks.begin()->first)  / 10000.0;
        double spread  = bestAsk - bestBid;

        midPrice  = (bestBid + bestAsk) / 2.0;
        spreadBps = midPrice > 0.0 ? (spread / midPrice) * 10000.0 : 0.0;

        double bidQ   = static_cast<double>(snap.bids.begin()->second) / 10000.0;
        double askQ   = static_cast<double>(snap.asks.begin()->second) / 10000.0;
        double totalQ = bidQ + askQ;
        imbalance  = (bidQ - askQ) / (totalQ + 1e-15);

        // Microprice from book top (also refreshed by CopyFromMicro)
        if (totalQ > 1e-15)
            microprice = (bestBid * askQ + bestAsk * bidQ) / totalQ;
    }

    // ── Copy microstructure results in — call after MicrostructureEngine ───────
    // MicrostructureResults must be fully defined when this is called.
    // Enforced by include order: MarketMicrostructure.h before MarketTypes.h.
    void CopyFromMicro(const MicrostructureResults &r) {
        microprice     = r.microprice;
        vwap           = r.vwap;
        twap           = r.twap;
        kyleLambda     = r.kyleLambda;
        amihudIlliq    = r.amihudIlliq;
        rollSpread     = r.rollSpread;
        signedFlow     = r.signedFlow;
        lastTradeSide  = r.lastTradeSide;
        lastTradePrice = r.lastTradePrice;
        lastTradeQty   = r.lastTradeQty;
        tradeCount     = r.tradeCount;
    }
};