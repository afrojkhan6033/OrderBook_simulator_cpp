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
#include "MarketMicrostructure.h"
#include "OrderBook.h"
#include "MarketTypes.h"  // for PriceLevelSnapshot, PerformanceMetrics, MarketAnalytics
#include "Signalengine.h"  // for SignalResults
#include "RiskEngine.h"    // for RiskResults
#include "BookDynamicsEngine.h"  // for BookDynamicsResults
#include "RegimeEngine.h"  // for RegimeResults
#include "StrategyEngine.h"  // for StrategyResults

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr uint8_t  MAGIC_0   = 0x48;  // 'H'
static constexpr uint8_t  MAGIC_1   = 0x46;  // 'F'
static constexpr uint8_t  VERSION   = 1;
static constexpr uint8_t  MSG_SNAPSHOT = 0;
static constexpr uint8_t  MSG_DELTA    = 1;
static constexpr uint8_t  MSG_TRADE             = 2;
static constexpr uint8_t  MSG_STATS             = 3;
static constexpr uint8_t  MSG_MICROSTRUCTURE    = 4;
static constexpr uint8_t  MSG_SIGNALS           = 5;
static constexpr uint8_t  MSG_RISK              = 6;
static constexpr uint8_t  MSG_BOOK_DYNAMICS     = 7;
static constexpr uint8_t  MSG_REGIME            = 8;
static constexpr uint8_t  MSG_STRATEGY          = 9;
static constexpr uint8_t  MSG_CROSS_EXCHANGE    = 10;

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
    char     symbol[16];
};

#pragma pack(pop)

// ── Wire payload structs (defined BEFORE serializer methods) ─────────────────
#pragma pack(push, 1)

// Microstructure payload (msg_type = 4)
struct MicrostructurePayload {
    double   vwap;              // 60s rolling VWAP (USD)
    double   twap;              // 60s rolling TWAP (USD)
    double   microprice;        // Stoikov WMP (USD)
    double   kyle_lambda;       // market impact λ (USD per unit signed flow)
    double   amihud_illiq;      // Amihud ILLIQ × 10^6
    double   roll_spread;       // Roll effective spread (USD)
    double   signed_flow;       // cumulative signed order flow
    int32_t  last_trade_side;   // +1=BUY / -1=SELL / 0=unknown
    double   last_trade_price;  // USD
    double   last_trade_qty;    // base asset units
    uint64_t trade_count;
};

// Signal engine payload (MSG_SIGNALS)
struct SignalPayload {
    double   ofiNormalized;    // [-1, +1]
    double   vpin;             // [0, 1]
    int32_t  vpinBuckets;
    double   momentumScore;
    double   ar1Coeff;
    double   zScore;
    int32_t  regime;           // 0=neutral / +1=trending / -1=mean-reverting
    int32_t  icebergDetected;  // boolean
    double   icebergPrice;
    int32_t  icebergRefills;
    int32_t  spoofingAlert;    // boolean
    double   spoofPrice;
    double   stuffingRatio;
};

// Risk engine payload (MSG_RISK)
struct RiskPayload {
    double   positionSize;
    double   entryPrice;
    double   positionNotional;
    double   totalPnl;
    double   realizedPnl;
    double   unrealizedPnl;
    double   winRate;
    double   fillProb100ms;
    double   queueDepthAtL1;
    double   tradeArrivalRate;
    double   sharpePerTrade;
    double   maxDrawdown;
    double   compositeScore;
    double   kalmanPrice;
    double   kalmanVelocity;
    double   hitRate;
    double   slipLast;
    double   slipAvg;
    double   slipImpl;
    double   slipArrival;
    double   dv01;
    double   liquidCost;
    double   mktImpact;
};

// Book dynamics payload (MSG_BOOK_DYNAMICS)
struct BookDynamicsPayload {
    float    heatmap[5 * 10];   // row-major: time(5) x price(10)
    double   heatmapMidPrice;
    double   heatmapBucketUSD;
    double   bidWallVelocity;
    double   askWallVelocity;
    double   bidGradientMean;
    double   askGradientMean;
    double   phantomRatio;
    uint64_t hiddenDetectCount;
    double   avgBidLifetimeMs;
    double   avgAskLifetimeMs;
    double   compressionRateUSD;
    int32_t  bidSl;
    int32_t  askSl;
    int32_t  bidLl;
    int32_t  askLl;
    double   bidGrad;
    double   askGrad;
    double   bidSteep;
    double   askSteep;
    int32_t  bidLifeHist[5];
    int32_t  askLifeHist[5];
};

// Regime engine payload (MSG_REGIME)
struct RegimePayload {
    double   realizedVolAnnualized;
    int32_t  volRegime;         // 0=LOW / 1=NORM / 2=HIGH / 3=EXTREME
    double   hurstExponent;
    int32_t  hurstRegime;
    double   hmmBullProb;
    double   hmmBearProb;
    double   autocorrLag1;
    double   regimeAdjustedScore;
    double   edgeScore;
    double   srTicks;
    double   srMean;
    double   srStd;
    double   srZ;
    double   midAcfZ;
    double   tickVol;
    int32_t  spreadHist[6];
};

// Strategy engine payload (MSG_STRATEGY)
struct StrategyPayload {
    double   asBid, asAsk, asReservation;
    double   asOptimalSpreadBps, asSkewBps, asSigmaBps;
    double   mmNetPnl, mmRealPnl, mmUnrealPnl;
    double   mmInventory, mmWinRate, mmFillRate;
    bool     mmInventoryAlert, mmQuotingGated;
    double   latEdgeBps, latCumEdgeUSD, latSharpe;
    int      latOpportunities;
    double   replaySimPnl, replayWinRate, replayMAE, replayMFE, replayMin, replayMax;
    double   exchBid1, exchAsk1, exchBid2, exchAsk2, exchMidBps, exchArbBps;
    int32_t  exchConn;
};

// Cross-Exchange Feed payload (MSG_CROSS_EXCHANGE)
struct CrossExchangePayload {
    double   bid;           // secondary exchange best bid
    double   ask;           // secondary exchange best ask
    double   mid;           // (bid+ask)/2
    double   spread;        // ask - bid (USD)
    double   spreadBps;     // spread in bps
    int32_t  connected;     // 1=connected, 0=disconnected
    int32_t  isSpot;        // 1=spot, 0=perp
    double   binanceMid;    // primary exchange mid for drift calc
    double   driftUSD;      // cfMid - binanceMid
    double   driftBps;      // drift in bps
    double   arbNetBps;     // cross-exchange arb opportunity
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

        // Bids: DESCENDING (std::greater<Price>) → .begin() = best (highest) bid.
        // Forward iteration gives best→worst bid order on the wire.
        int i = 0;
        for (auto it = snap.bids.begin(); it != snap.bids.end() && i < bidCount; ++it, i++) {
            WirePriceLevel pl{ static_cast<int32_t>(it->first),
                               static_cast<uint32_t>(it->second) };
            append(buf, pl);
        }

        // Asks: ASCENDING (std::less<Price>, default) → .begin() = best (lowest) ask.
        // Forward iteration gives best→worst ask order on the wire.
        i = 0;
        for (auto it = snap.asks.begin(); it != snap.asks.end() && i < askCount; ++it, i++) {
            WirePriceLevel pl{ static_cast<int32_t>(it->first),
                               static_cast<uint32_t>(it->second) };
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
        const MarketAnalytics&    analytics,
        const std::string&        symbol)
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
        std::memset(sp.symbol, 0, 16);
        std::strncpy(sp.symbol, symbol.c_str(), 15);
        append(buf, sp);

        return buf;
    }

    // Serialize microstructure results
    static std::vector<uint8_t> SerializeMicrostructure(const MicrostructureResults& r) {
        constexpr uint32_t payload_len = sizeof(MicrostructurePayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_MICROSTRUCTURE;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        MicrostructurePayload mp;
        mp.vwap             = r.vwap;
        mp.twap             = r.twap;
        mp.microprice       = r.microprice;
        mp.kyle_lambda      = r.kyleLambda;
        mp.amihud_illiq     = r.amihudIlliq;
        mp.roll_spread      = r.rollSpread;
        mp.signed_flow      = r.signedFlow;
        mp.last_trade_side  = static_cast<int32_t>(r.lastTradeSide);
        mp.last_trade_price = r.lastTradePrice;
        mp.last_trade_qty   = r.lastTradeQty;
        mp.trade_count      = r.tradeCount;
        append(buf, mp);

        return buf;
    }

    // Serialize signal engine results
    static std::vector<uint8_t> SerializeSignals(const SignalResults& sig) {
        constexpr uint32_t payload_len = sizeof(SignalPayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_SIGNALS;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        SignalPayload sp;
        sp.ofiNormalized   = sig.ofiNormalized;
        sp.vpin            = sig.vpin;
        sp.vpinBuckets     = static_cast<int32_t>(sig.vpinBuckets);
        sp.momentumScore   = sig.momentumScore;
        sp.ar1Coeff        = sig.ar1Coeff;
        sp.zScore          = sig.zScore;
        sp.regime          = static_cast<int32_t>(sig.regime);
        sp.icebergDetected = sig.icebergDetected ? 1 : 0;
        sp.icebergPrice    = sig.icebergPrice;
        sp.icebergRefills  = static_cast<int32_t>(sig.icebergRefills);
        sp.spoofingAlert   = sig.spoofingAlert ? 1 : 0;
        sp.spoofPrice      = sig.spoofPrice;
        sp.stuffingRatio   = sig.stuffingRatio;
        append(buf, sp);

        return buf;
    }

    // Serialize risk engine results
    static std::vector<uint8_t> SerializeRisk(const RiskResults& risk) {
        constexpr uint32_t payload_len = sizeof(RiskPayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_RISK;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        RiskPayload rp;
        rp.positionSize     = risk.positionSize;
        rp.entryPrice       = risk.entryPrice;
        rp.positionNotional = risk.positionNotional;
        rp.totalPnl         = risk.totalPnl;
        rp.realizedPnl      = risk.realizedPnl;
        rp.unrealizedPnl    = risk.unrealizedPnl;
        rp.winRate          = risk.winRate;
        rp.fillProb100ms    = risk.fillProb100ms;
        rp.queueDepthAtL1   = risk.queueDepthAtL1;
        rp.tradeArrivalRate = risk.tradeArrivalRate;
        rp.sharpePerTrade   = risk.sharpePerTrade;
        rp.maxDrawdown      = risk.maxDrawdown;
        rp.compositeScore   = risk.compositeScore;
        rp.kalmanPrice      = risk.kalmanPrice;
        rp.kalmanVelocity   = risk.kalmanVelocity;
        rp.hitRate          = risk.hitRate;
        rp.slipLast         = risk.lastSlippageBps;
        rp.slipAvg          = risk.avgSlippageBps;
        rp.slipImpl         = risk.implShortfallBps;
        rp.slipArrival      = risk.lastArrivalPrice;
        rp.dv01             = risk.dv01;
        rp.liquidCost       = risk.liquidationCost;
        rp.mktImpact        = risk.marketImpact;
        append(buf, rp);

        return buf;
    }

    // Serialize book dynamics results
    static std::vector<uint8_t> SerializeBookDynamics(const BookDynamicsResults& bd) {
        constexpr uint32_t payload_len = sizeof(BookDynamicsPayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_BOOK_DYNAMICS;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        BookDynamicsPayload bp;
        std::memcpy(bp.heatmap, bd.heatmap, sizeof(float) * 5 * 10);
        bp.heatmapMidPrice    = bd.heatmapMidPrice;
        bp.heatmapBucketUSD   = bd.heatmapBucketUSD;
        bp.bidWallVelocity    = bd.bidWallVelocity;
        bp.askWallVelocity    = bd.askWallVelocity;
        bp.bidGradientMean    = bd.bidGradientMean;
        bp.askGradientMean    = bd.askGradientMean;
        bp.phantomRatio       = bd.phantomRatio;
        bp.hiddenDetectCount  = static_cast<uint64_t>(bd.hiddenDetectCount);
        bp.avgBidLifetimeMs   = bd.avgBidLifetimeMs;
        bp.avgAskLifetimeMs   = bd.avgAskLifetimeMs;
        bp.compressionRateUSD = bd.compressionRateUSD;
        bp.bidSl              = bd.shortLivedBid;
        bp.askSl              = bd.shortLivedAsk;
        bp.bidLl              = bd.longLivedBid;
        bp.askLl              = bd.longLivedAsk;
        bp.bidGrad            = 0; // fallback if missing
        bp.askGrad            = 0;
        bp.bidSteep           = 0;
        bp.askSteep           = 0;
        for(int i=0;i<5;i++) bp.bidLifeHist[i] = bd.bidLifeHist[i];
        for(int i=0;i<5;i++) bp.askLifeHist[i] = bd.askLifeHist[i];
        append(buf, bp);

        return buf;
    }

    // Serialize regime engine results
    static std::vector<uint8_t> SerializeRegime(const RegimeResults& reg) {
        constexpr uint32_t payload_len = sizeof(RegimePayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_REGIME;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        RegimePayload rp;
        rp.realizedVolAnnualized = reg.realizedVolAnnualized;
        rp.volRegime             = static_cast<int32_t>(reg.volRegime);
        rp.hurstExponent         = reg.hurstExponent;
        rp.hurstRegime           = static_cast<int32_t>(reg.hurstRegime);
        rp.hmmBullProb           = reg.hmmBullProb;
        rp.hmmBearProb           = reg.hmmBearProb;
        rp.autocorrLag1          = reg.autocorrLag1;
        rp.regimeAdjustedScore   = reg.regimeAdjustedScore;
        rp.edgeScore             = reg.edgeScore;
        rp.srTicks               = reg.spreadTicks;
        rp.srMean                = reg.spreadTickMean;
        rp.srStd                 = reg.spreadTickStd;
        rp.srZ                   = reg.spreadZScore;
        rp.midAcfZ               = reg.autocorrZScore;
        rp.tickVol               = reg.realizedVolPerTick;
        for(int i=0;i<6;i++) rp.spreadHist[i] = reg.spreadHist[i];
        append(buf, rp);

        return buf;
    }

    // Serialize strategy engine results
    static std::vector<uint8_t> SerializeStrategy(const StrategyResults& strat) {
        constexpr uint32_t payload_len = sizeof(StrategyPayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_STRATEGY;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        StrategyPayload sp;
        sp.asBid              = strat.asBid;
        sp.asAsk              = strat.asAsk;
        sp.asReservation      = strat.asReservation;
        sp.asOptimalSpreadBps = strat.asOptimalSpreadBps;
        sp.asSkewBps          = strat.asSkewBps;
        sp.asSigmaBps         = strat.asSigmaBps;
        sp.mmNetPnl           = strat.mmNetPnl;
        sp.mmRealPnl          = strat.mmRealizedPnl;
        sp.mmUnrealPnl        = strat.mmUnrealizedPnl;
        sp.mmInventory        = strat.mmInventory;
        sp.mmWinRate          = strat.mmWinRate;
        sp.mmFillRate         = strat.mmFillRate;
        sp.mmInventoryAlert   = strat.mmInventoryAlert ? 1 : 0;
        sp.mmQuotingGated     = strat.mmQuotingGated ? 1 : 0;
        sp.latEdgeBps         = strat.latEdgeBps;
        sp.latCumEdgeUSD      = strat.latCumEdgeUSD;
        sp.latSharpe          = strat.latSharpe;
        sp.latOpportunities   = strat.latOpportunities;
        sp.replaySimPnl       = strat.replaySimPnl;
        sp.replayWinRate      = strat.replayWinRate;
        sp.replayMAE          = strat.replayMAE;
        sp.replayMFE          = strat.replayMFE;
        sp.replayMin          = strat.replayMinPrice;
        sp.replayMax          = strat.replayMaxPrice;
        sp.exchBid1           = strat.exchBid1;
        sp.exchAsk1           = strat.exchAsk1;
        sp.exchBid2           = strat.exchBid2;
        sp.exchAsk2           = strat.exchAsk2;
        sp.exchMidBps         = strat.exchMidSpreadBps;
        sp.exchArbBps         = strat.exchArbBps;
        sp.exchConn           = strat.exchConnected ? 1 : 0;
        append(buf, sp);

        return buf;
    }

    // Serialize cross-exchange feed snapshot
    static std::vector<uint8_t> SerializeCrossExchange(
        double bid, double ask, bool connected, bool isSpot,
        double binanceMid)
    {
        constexpr uint32_t payload_len = sizeof(CrossExchangePayload);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(FrameHeader) + payload_len);

        FrameHeader hdr;
        hdr.magic[0]    = MAGIC_0;
        hdr.magic[1]    = MAGIC_1;
        hdr.version     = VERSION;
        hdr.msg_type    = MSG_CROSS_EXCHANGE;
        hdr.payload_len = payload_len;
        append(buf, hdr);

        CrossExchangePayload cp{};
        cp.bid        = bid;
        cp.ask        = ask;
        cp.mid        = (bid > 0 && ask > 0) ? (bid + ask) / 2.0 : 0.0;
        cp.spread     = (bid > 0 && ask > 0) ? (ask - bid) : 0.0;
        cp.spreadBps  = (cp.mid > 0) ? (cp.spread / cp.mid) * 10000.0 : 0.0;
        cp.connected  = connected ? 1 : 0;
        cp.isSpot     = isSpot ? 1 : 0;
        cp.binanceMid = binanceMid;
        cp.driftUSD   = (binanceMid > 0 && cp.mid > 0) ? (cp.mid - binanceMid) : 0.0;
        cp.driftBps   = (binanceMid > 0) ? (cp.driftUSD / binanceMid) * 10000.0 : 0.0;
        cp.arbNetBps  = 0.0;  // can be computed later
        append(buf, cp);

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

