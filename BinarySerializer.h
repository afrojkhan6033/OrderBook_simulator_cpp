#pragma once
// =============================================================================
// BinarySerializer.h  —  Zero-copy binary output from your C++ OrderBook engine
//
// Wire format (little-endian, fixed-width, no dynamic allocation):
//
//  [FRAME HEADER]  8 bytes
//    uint8_t  magic[2]     = { 0x48, 0x46 }  // "HF"
//    uint8_t  version      = 1
//    uint8_t  msg_type     // 0=snapshot, 1=delta, 2=trade, 3=stats
//    uint32_t payload_len  // bytes that follow
//
//  [SNAPSHOT / DELTA payload]
//    uint64_t sequence_number
//    uint64_t event_time_us       // microseconds since epoch
//    uint32_t bid_count
//    uint32_t ask_count
//    PriceLevel bids[bid_count]
//    PriceLevel asks[ask_count]
//
//  [PriceLevel]  8 bytes
//    int32_t  price     // raw * 10000  (matches your existing encoding)
//    uint32_t quantity  // raw * 10000
//
//  [TRADE payload]
//    uint64_t sequence_number
//    uint64_t event_time_us
//    int32_t  price
//    uint32_t quantity
//    uint8_t  side        // 0=buy, 1=sell
//    uint8_t  _pad[3]
//
//  [STATS payload]  (sent every STATS_PRINT_INTERVAL_MS)
//    uint64_t messages_received
//    uint64_t messages_processed
//    uint64_t messages_dropped
//    uint64_t total_processing_time_us
//    uint64_t max_processing_time_us
//    uint64_t min_processing_time_us
//    double   last_spread_bps
//    double   current_mid_price
//    double   imbalance          // [-1, 1]
//    double   ofi                // order flow imbalance
//    uint64_t sequence_number
// =============================================================================
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <chrono>

// Reuse your existing types
#include "OrderBook.h"
#include "MarketTypes.h"  // for PriceLevelSnapshot, PerformanceMetrics, MarketAnalytics

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr uint8_t  MAGIC_0   = 0x48;  // 'H'
static constexpr uint8_t  MAGIC_1   = 0x46;  // 'F'
static constexpr uint8_t  VERSION   = 1;
static constexpr uint8_t  MSG_SNAPSHOT = 0;
static constexpr uint8_t  MSG_DELTA    = 1;
static constexpr uint8_t  MSG_TRADE    = 2;
static constexpr uint8_t  MSG_STATS    = 3;

// ── Wire structs (packed, no padding) ────────────────────────────────────────
#pragma pack(push, 1)

struct FrameHeader {
    uint8_t  magic[2];
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t payload_len;
};

struct WirePriceLevel {
    int32_t  price;     // raw * 10000
    uint32_t quantity;  // raw * 10000
};

struct SnapshotHeader {
    uint64_t sequence_number;
    uint64_t event_time_us;
    uint32_t bid_count;
    uint32_t ask_count;
};

struct TradePayload {
    uint64_t sequence_number;
    uint64_t event_time_us;
    int32_t  price;
    uint32_t quantity;
    uint8_t  side;       // 0=buy, 1=sell
    uint8_t  pad[3];
};

struct StatsPayload {
    uint64_t messages_received;
    uint64_t messages_processed;
    uint64_t messages_dropped;
    uint64_t total_processing_time_us;
    uint64_t max_processing_time_us;
    uint64_t min_processing_time_us;
    double   last_spread_bps;
    double   current_mid_price;
    double   imbalance;
    double   ofi;
    uint64_t sequence_number;
};

#pragma pack(pop)

// ── BinarySerializer class ────────────────────────────────────────────────────
class BinarySerializer {
public:
    // Serialize a full book snapshot.
    // Returns a buffer ready to send over TCP/WebSocket.
    static std::vector<uint8_t> SerializeSnapshot(
        const PriceLevelSnapshot& snap,
        uint8_t msg_type = MSG_SNAPSHOT,
        int depth = 50)
    {
        // Clamp to requested depth
        int bidCount = std::min((int)snap.bids.size(), depth);
        int askCount = std::min((int)snap.asks.size(), depth);

        uint32_t payload_len =
            sizeof(SnapshotHeader) +
            static_cast<uint32_t>(bidCount + askCount) * sizeof(WirePriceLevel);

        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len + 64);

        // Header
        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = msg_type;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        // Snapshot header
        SnapshotHeader sh;
        sh.sequence_number = snap.sequenceNumber;
        sh.event_time_us   = us_now();
        sh.bid_count       = static_cast<uint32_t>(bidCount);
        sh.ask_count       = static_cast<uint32_t>(askCount);
        append(buf, sh);

        // Bids: snap.bids is std::map<Price, Quantity> sorted ascending by price key.
        // Best bid is the HIGHEST price → iterate in reverse.
        int i = 0;
        for (auto it = snap.bids.rbegin(); it != snap.bids.rend() && i < bidCount; ++it , i++) {
            WirePriceLevel pl{ it->first, it->second };
            append(buf, pl);
        }

        // Asks: snap.asks is std::map<Price, Quantity, std::greater<Price>> sorted DESC.
        // Best ask is the LOWEST price → iterate in reverse.
        i = 0;
        for (auto it = snap.asks.begin(); it != snap.asks.end() && i < askCount; ++it , i++) {
            WirePriceLevel pl{ it->first, it->second };
            append(buf, pl);
        }

        return buf;
    }

    // Serialize a single trade execution.
    static std::vector<uint8_t> SerializeTrade(
        int32_t  price,
        uint32_t quantity,
        bool     isBuy,
        uint64_t seqNum)
    {
        constexpr uint32_t payload_len = sizeof(TradePayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr{ {MAGIC_0, MAGIC_1}, VERSION, MSG_TRADE, payload_len };
        append(buf, hdr);

        TradePayload tp;
        tp.sequence_number    = seqNum;
        tp.event_time_us      = us_now();
        tp.price              = price;
        tp.quantity           = quantity;
        tp.side               = isBuy ? 0 : 1;
        tp.pad[0] = tp.pad[1] = tp.pad[2] = 0;
        append(buf, tp);

        return buf;
    }

    // Serialize a PerformanceMetrics + MarketAnalytics stats frame.
    static std::vector<uint8_t> SerializeStats(
        const PerformanceMetrics& metrics,
        const MarketAnalytics&    analytics)
    {
        constexpr uint32_t payload_len = sizeof(StatsPayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr{ {MAGIC_0, MAGIC_1}, VERSION, MSG_STATS, payload_len };
        append(buf, hdr);

        StatsPayload sp;
        sp.messages_received        = metrics.messagesReceived.load();
        sp.messages_processed       = metrics.messagesProcessed.load();
        sp.messages_dropped         = metrics.messagesDropped.load();
        sp.total_processing_time_us = metrics.totalProcessingTimeUs.load();
        sp.max_processing_time_us   = metrics.maxProcessingTimeUs.load();
        sp.min_processing_time_us   = metrics.minProcessingTimeUs.load();
        sp.last_spread_bps          = metrics.lastSpreadBps.load();
        sp.current_mid_price        = metrics.currentMidPrice.load();
        sp.imbalance                = analytics.imbalance;
        sp.ofi                      = analytics.ofi / 10000.0;  // normalise
        sp.sequence_number          = metrics.sequenceNumber.load();
        append(buf, sp);

        return buf;
    }

private:
    template<typename T>
    static void append(std::vector<uint8_t>& buf, const T& val) {
        size_t pos = buf.size();
        buf.resize(pos + sizeof(T));
        std::memcpy(buf.data() + pos, &val, sizeof(T));
    }

    static uint64_t us_now() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    }
};
