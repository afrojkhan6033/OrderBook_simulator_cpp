// ============================================================================
// LiveMarketDataWS.cpp  —  HFT WebSocket Market Data Engine  v8
//
// Base: document 19 (v3) — EVERY LINE preserved exactly unless marked v4.
//
// New in v4 — Signal Analytics Engine (SignalEngine.h):
//   P1  Multi-Level OFI  L1-L5 exp-decay weights, EWM α=0.3, O(1) per tick
//   P1  VPIN             volume-bucket adverse-selection (Easley et al. 2012)
//   P2  Momentum/MR      AR(1)+EWMA+z-score, 60-tick regime detection
//   P2  Iceberg Detector refill-based, 2s window, ≥3 refills = alert
//   P2  Quote Stuffing   msgs/|ΔQty| rolling 1s; >20 msgs + ratio>10 = alert
//   P3  Spoofing/Layer   large-order cancel-repeat, 60s window, ≥3 = alert
//
// New in v5 — Risk & PnL Engine (RiskEngine.h):
//   P1  SimulatedPositionTracker  OFI+composite signal → paper long/short
//       SlippageCalculator        microprice arrival vs exec; impl shortfall
//   P2  FillProbabilityModel      proportional-hazard queue model (100ms)
//       RollingRiskMetrics        100-trade Sharpe (per-trade + annualised)
//       MaxDrawdown               all-time peak-to-trough equity curve
//   P3  InventoryRiskHeatmap      DV01, liquidation cost, Kyle's λ impact
//   +   KalmanPriceFilter         2-state velocity-based direction signal
//   +   CompositeDirectionModel   5-factor weighted signal score [-1, +1]
//   +   HitRateTracker            EWM rolling signal accuracy (5-tick eval)
//
// New in v8 — Strategy Simulation & Backtesting Engine (StrategyEngine.h):
//   P1  MarketMakingSimulator    AS optimal quotes, fill simulation, inventory PnL
//   P1  AvellanedaStoikov        r=mid−q·γ·σ², δ*=γ·σ²+(2/γ)·ln(1+γ/κ) optimal MM
//   P2  LatencyArbSimulator      rolling tick buffer, N_US co-location edge (bps)
//   P2  SignalOrderFlowReplay    30s rolling replay, ASCII chart, simulated PnL
//   P3  CrossExchangeArbitrage   Bybit WebSocket feed, dual-exchange arb alert
//
// New in v7 — Statistical Regime & Market State Engine (RegimeEngine.h):
//   P1  YangZhangVolRegime       Rogers-Satchell tick-adapted vol, 4-tier regime
//   P2  SpreadRegimeTick         rolling 100-bar histogram, widening alert
//   P2  MidReturnACF             Lag-1 autocorrelation (Bartlett SE), regime flip
//   P2  HurstExponentRS          R/S rescaled range, H>0.55=trend/H<0.45=MR
//   P3  OnlineHMMFilter          2-state Bayesian HMM, online emission adaptation
//   +   RegimeAdjustedScore      5-factor gated composite → enhanced direction edge
//
// New in v6 — Book Dynamics & Order Flow Engine (BookDynamicsEngine.h):
//   P1  BookPressureHeatmap      2D time×price EWM intensity matrix (30×40)
//   P1  LevelLifetimeTracker     per-level birth/death timestamps + histogram
//   P2  DepthMigrationVelocity   OLS slope of bid/ask wall position (USD/s)
//   P2  OrderBookGradient        dQ/dP via finite differences, cliff detection
//   P3  HiddenLiquidityEstimator trade exceedance vs visible book (iceberg/dark)
//
// Integration points (all other code is verbatim document 19):
//   #include "SignalEngine.h"         (after MarketTypes.h)
//   SignalEngine signals;             (alongside MicrostructureEngine micro)
//   ComputeTotalQtyChange()           (new helper before main)
//   signals.OnTrade()                 (inside aggTrade path)
//   signals.OnDepthUpdate()           (inside depth path, after analytics)
//   PrintOrderbookDisplay(..., sig)   (new SignalResults parameter + panel)
//
// Fixes 1–10 from v2 are unchanged.
//
// New in v3 — Microstructure Analytics (MarketMicrostructure.h):
//   • Second WebSocket thread subscribes to @aggTrade stream
//     Both depth (@depth@100ms) and trade (@aggTrade) messages share the
//     single MessageQueue. Routing is by JSON field detection in the loop.
//   • MicrostructureEngine drives:
//       VWAP (60s)       — real trade price × qty, time-windowed
//       TWAP (60s)       — time-weighted average of real trade prices
//       Lee-Ready        — quote rule + tick rule, cross-checked vs Binance `m`
//       Kyle's λ         — rolling OLS Cov(Δmid,Q_s)/Var(Q_s), 200-trade window
//       Amihud ILLIQ     — |logReturn|/notional × 10^6, 200-trade window
//       Microprice (WMP) — (bid×askQ + ask×bidQ)/(bidQ+askQ), per book tick
//       Roll Spread      — 2√max(0,−Cov(ΔP_t,ΔP_{t-1})), 100-price window
//   • analytics.CopyFromMicro() merges results into MarketAnalytics every tick
//   • Extended terminal display with a dedicated Microstructure panel
//   • BinarySerializerMicro::SerializeMicrostructure() queued to TCP sender
//
// Data consistency guarantees:
//   • VWAP/TWAP use only real confirmed aggTrade ticks (not depth midprice)
//   • Kyle's λ uses Δmid ACROSS the depth update (before→after), not Δtrade
//   • Amihud uses log return of trade price / notional (price×qty), not qty alone
//   • Roll's spread receives both trade prices AND depth mid prices as inputs
//   • Microprice recomputed on every depth update from L1 quantities
//   • Lee-Ready validated against Binance `m` flag; `m` wins (more accurate)
//   • aggTrade messages NOT buffered (historical trades pre-snapshot unused)
//   • depth messages still buffered and drained exactly as in v2
//
// Fixes 1–10 from v2 are unchanged:
//   1. Binance U/u sync rule on first post-snapshot update
//   2. Explicit buffered-message drain before live loop
//   3. Queue cleared on every resync
//   4. Bid map std::greater enforced (MarketTypes.h)
//   5. Buffer size cap (MAX_BUFFERED_MESSAGES)
//   6. REST snapshot retry (3 attempts, 500ms apart)
//   7. Batch message processing (PopBatch)
//   8. OFI lastState written back after calculation
//   9. Network latency tracked from receivedTime
//  10. MarketAnalytics persistent (not recreated each tick)
// ============================================================================

#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

// ── Project headers — order matters ─────────────────────────────────────────
// MarketMicrostructure.h MUST precede MarketTypes.h because
// MarketTypes::CopyFromMicro uses MicrostructureResults (defined here).
#include "MarketMicrostructure.h"
#include "OrderBook.h"
#include "BinarySerializer.h"
#include "TcpSender.h"
#include "MarketTypes.h"
#include "SignalEngine.h"    // v4: P1/P2/P3 signal analytics
#include "RiskEngine.h"     // v5: Risk & PnL engine
#include "BookDynamicsEngine.h"  // v6: Book Dynamics & Order Flow
#include "RegimeEngine.h"        // v7: Statistical Regime & Market State
#include "StrategyEngine.h"      // v8: Strategy Simulation & Backtesting
#include "CrossExchangeFeed.h"   // v9: Standalone multi-exchange L1 feed (Bybit/OKX)

// ── System / third-party headers ─────────────────────────────────────────────
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/error/en.h>
#include <cmath>
#include <curl/curl.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <map>
#include <unordered_map>
#include <deque>
#include <atomic>
#include <mutex>
#include <shared_mutex>

#ifdef _WIN32
#include <windows.h>
#endif
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;

// ============================================================================
// ULTRA-FAST JSON PARSER using rapidjson (10x faster than nlohmann)
// ============================================================================
namespace FastJSON {
    struct DepthUpdate {
        uint64_t U = 0;
        uint64_t u = 0;
        struct PriceQty {
            double price = 0.0;
            double qty = 0.0;
        };
        std::vector<PriceQty> bids;
        std::vector<PriceQty> asks;
        bool hasBids = false;
        bool hasAsks = false;
    };

    inline DepthUpdate ParseDepthUpdate(const std::string &jsonStr) {
        rapidjson::Document d;
        d.Parse(jsonStr.c_str());

        DepthUpdate result;

        if (d.HasParseError()) {
            return result;
        }

        if (d.HasMember("U") && d["U"].IsUint64()) {
            result.U = d["U"].GetUint64();
        }
        if (d.HasMember("u") && d["u"].IsUint64()) {
            result.u = d["u"].GetUint64();
        }

        if (d.HasMember("b") && d["b"].IsArray()) {
            result.hasBids = true;
            for (auto& bid : d["b"].GetArray()) {
                if (bid.IsArray() && bid.Size() >= 2) {
                    DepthUpdate::PriceQty pq;
                    if (bid[0].IsString()) {
                        pq.price = std::stod(bid[0].GetString());
                    }
                    if (bid[1].IsString()) {
                        pq.qty = std::stod(bid[1].GetString());
                    }
                    result.bids.push_back(pq);
                }
            }
        }

        if (d.HasMember("a") && d["a"].IsArray()) {
            result.hasAsks = true;
            for (auto& ask : d["a"].GetArray()) {
                if (ask.IsArray() && ask.Size() >= 2) {
                    DepthUpdate::PriceQty pq;
                    if (ask[0].IsString()) {
                        pq.price = std::stod(ask[0].GetString());
                    }
                    if (ask[1].IsString()) {
                        pq.qty = std::stod(ask[1].GetString());
                    }
                    result.asks.push_back(pq);
                }
            }
        }

        return result;
    }

    // Check if this is an aggTrade message
    inline bool IsAggTrade(const std::string &jsonStr) {
        rapidjson::Document d;
        d.Parse(jsonStr.c_str());
        if (d.HasParseError()) return false;

        if (d.HasMember("e") && d["e"].IsString()) {
            std::string e = d["e"].GetString();
            return e == "aggTrade";
        }
        return false;
    }

    // Extract aggTrade fields
    struct AggTrade {
        double price = 0.0;
        double qty = 0.0;
        bool buyerMaker = false;
        int64_t tradeTime = 0;
    };

    inline AggTrade ParseAggTrade(const std::string &jsonStr) {
        rapidjson::Document d;
        d.Parse(jsonStr.c_str());

        AggTrade result;

        if (d.HasParseError()) return result;

        if (d.HasMember("p") && d["p"].IsString()) {
            result.price = std::stod(d["p"].GetString());
        }
        if (d.HasMember("q") && d["q"].IsString()) {
            result.qty = std::stod(d["q"].GetString());
        }
        if (d.HasMember("m") && d["m"].IsBool()) {
            result.buyerMaker = d["m"].GetBool();
        }
        if (d.HasMember("T") && d["T"].IsInt64()) {
            result.tradeTime = d["T"].GetInt64();
        }

        return result;
    }
}

// ============================================================================
// HFT Configuration  (v2 fields preserved; v3 adds microstructure window sizes)
// ============================================================================
struct HFTConfig {
    // ── Orderbook ─────────────────────────────────────────────────────────────
    static constexpr int    ORDERBOOK_DEPTH          = 50;
    static constexpr int    SNAPSHOT_DEPTH           = 1000;

    // ── Message queue ─────────────────────────────────────────────────────────
    static constexpr int    MAX_MESSAGE_QUEUE        = 100000;
    static constexpr int    MAX_BUFFERED_MESSAGES    = 50000;   // FIX 5
    static constexpr int    BATCH_PROCESS_SIZE       = 100;     // FIX 7

    // ── WebSocket ─────────────────────────────────────────────────────────────
    static constexpr int    WS_TIMEOUT_MS            = 30000;
    static constexpr int    WS_RECONNECT_DELAY_MS    = 2000;
    static constexpr int    WS_PING_INTERVAL_MS      = 15000;

    // ── Snapshot ──────────────────────────────────────────────────────────────
    static constexpr int    SNAPSHOT_REFRESH_SEC     = 60;
    static constexpr int    SNAPSHOT_TIMEOUT_MS      = 5000;
    static constexpr int    SNAPSHOT_RETRY_COUNT     = 3;       // FIX 6
    static constexpr int    SNAPSHOT_RETRY_DELAY_MS  = 500;     // FIX 6

    // ── Timing ────────────────────────────────────────────────────────────────
    static constexpr int    STATS_PRINT_INTERVAL_MS  = 5000;
    static constexpr double PROCESSING_THRESHOLD_US  = 1000.0;
    static constexpr int    LATENCY_WINDOW_SIZE      = 1000;
    static constexpr int    ANALYTICS_INTERVAL_MS    = 500;
    static constexpr int    DISPLAY_INTERVAL_MS      = 1000;
    static constexpr int    SIGNAL_INTERVAL_MS       = 5000;
    static constexpr int    IDLE_SLEEP_US            = 50;
    static constexpr int    PROCESS_SLEEP_US         = 10;

    // ── OFI ───────────────────────────────────────────────────────────────────
    static constexpr int    OFI_LEVELS               = 5;
    static constexpr double OFI_ALPHA                = 0.3;
    static constexpr int    MAX_SEQUENCE_GAP         = 5;

    // ── Microstructure windows (v3) ───────────────────────────────────────────
    static constexpr int64_t VWAP_TWAP_WINDOW_US     = 60'000'000LL; // 60 s
    static constexpr int     KYLE_LAMBDA_WINDOW       = 200;           // trades
    static constexpr int     AMIHUD_WINDOW            = 200;           // trades
    static constexpr int     ROLL_SPREAD_WINDOW       = 100;           // prices
};

// ============================================================================
// REST helpers — UNCHANGED from v2
// ============================================================================
static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                            std::string *userp) {
    userp->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string FetchBinanceOrderbook(const std::string &symbol,
                                  const std::string &marketType,
                                  int limit = 1000) {
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    if (!curl) return readBuffer;

    std::string baseUrl;
    if      (marketType == "spot")    baseUrl = "https://api.binance.com/api/v3/depth";
    else if (marketType == "futures") baseUrl = "https://fapi.binance.com/fapi/v1/depth";
    else if (marketType == "coin")    baseUrl = "https://dapi.binance.com/dapi/v1/depth";
    else {
        std::cerr << "Invalid market type! Use: spot | futures | coin\n";
        curl_easy_cleanup(curl);
        return readBuffer;
    }

    std::string url = baseUrl + "?symbol=" + symbol +
                      "&limit=" + std::to_string(limit);

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &readBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Network latency optimizations
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY,   1L);              // Disable Nagle's algorithm
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);              // Enable TCP keepalive
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,  120L);            // Keepalive idle time
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);             // Keepalive interval
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,    65536L);          // Larger receive buffer

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        std::cerr << "curl failed: " << curl_easy_strerror(res) << "\n";

    curl_easy_cleanup(curl);
    return readBuffer;
}

// FIX 6: retry wrapper
std::string FetchBinanceOrderbookWithRetry(const std::string &symbol,
                                           const std::string &marketType,
                                           int limit = 1000) {
    std::string result;
    for (int attempt = 0; attempt < HFTConfig::SNAPSHOT_RETRY_COUNT; ++attempt) {
        result = FetchBinanceOrderbook(symbol, marketType, limit);
        if (!result.empty()) return result;
        std::cerr << "Snapshot fetch attempt " << (attempt + 1)
                  << " failed. Retrying...\n";
        std::this_thread::sleep_for(
            std::chrono::milliseconds(HFTConfig::SNAPSHOT_RETRY_DELAY_MS));
    }
    return result;
}

// ============================================================================
// MessageQueue — v2 with optional lock-free backend (performance optimization)
// ============================================================================

// Enable lock-free queue (requires boost-lockfree - already installed)
#define USE_LOCK_FREE_QUEUE 1

#ifdef USE_LOCK_FREE_QUEUE
    #include "LockFreeQueue.h"
    #include <string>
    #include <chrono>
    #include <thread>
    #include <iostream>

    struct MessageQueueMessage {
        std::string data;
        std::chrono::high_resolution_clock::time_point receivedTime;
    };

    class MessageQueue {
    public:
        using Message = MessageQueueMessage;

    private:
        LockFreeQueue<MessageQueueMessage> queue_;
        std::atomic<bool> shutdown_{false};

    public:
        explicit MessageQueue(size_t maxSize = HFTConfig::MAX_MESSAGE_QUEUE)
            : queue_(maxSize)
        {
            std::cerr << "[DEBUG] MessageQueue constructed, maxSize=" << maxSize << std::endl;
        }

        bool Push(const std::string &data) {
            return queue_.Push(data);
        }

        // Blocking Pop - spins with yield until message available or shutdown
        bool Pop(Message &message) {
            while (!shutdown_.load(std::memory_order_acquire)) {
                if (queue_.Pop(message)) {
                    return true;
                }
                // Yield briefly to avoid busy-wait spinning
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            return false;
        }

        bool PopWithTimeout(Message &message, int timeoutMs) {
            auto deadline = std::chrono::high_resolution_clock::now() +
                           std::chrono::milliseconds(timeoutMs);
            while (std::chrono::high_resolution_clock::now() < deadline) {
                if (queue_.Pop(message)) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            return false;
        }

        bool TryPop(Message &message) {
            return queue_.TryPop(message);
        }

        size_t PopBatch(std::vector<Message> &messages, size_t maxBatch) {
            return queue_.PopBatch(messages, maxBatch);
        }

        size_t Size() const { return queue_.Size(); }
        size_t Capacity() const { return queue_.Capacity(); }
        size_t DroppedMessages() const { return queue_.DroppedMessages(); }
        size_t HighWaterMark() const { return queue_.HighWaterMark(); }

        void Clear() { queue_.Clear(); }

        void Shutdown() {
            shutdown_ = true;
            queue_.Shutdown();
        }
    };
#else
class MessageQueue {
public:
    struct Message {
        std::string data;
        std::chrono::high_resolution_clock::time_point receivedTime;
    };

private:
    std::deque<Message>      queue_;
    mutable std::mutex       mutex_;
    std::condition_variable  notEmpty_;
    const size_t             maxSize_;
    std::atomic<size_t>      droppedMessages_{0};
    std::atomic<size_t>      highWaterMark_{0};
    std::atomic<bool>        shutdown_{false};

public:
    explicit MessageQueue(size_t maxSize = HFTConfig::MAX_MESSAGE_QUEUE)
        : maxSize_(maxSize) {}

    bool Push(const std::string &data) {
        auto now = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.size() >= maxSize_) { droppedMessages_++; return false; }
        queue_.push_back({data, now});
        if (queue_.size() > highWaterMark_) highWaterMark_ = queue_.size();
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    bool Pop(Message &message) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (shutdown_) return false;
        message = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool PopWithTimeout(Message &message, int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!notEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                [this] { return !queue_.empty() || shutdown_; }))
            return false;
        if (shutdown_) return false;
        message = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool TryPop(Message &message) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        message = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // FIX 7
    size_t PopBatch(std::vector<Message> &messages, size_t maxBatch) {
        std::unique_lock<std::mutex> lock(mutex_);
        size_t count = std::min(maxBatch, queue_.size());
        for (size_t i = 0; i < count; ++i) {
            messages.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return count;
    }

    size_t Size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }
    size_t Capacity()         const { return maxSize_; }
    size_t DroppedMessages()  const { return droppedMessages_.load(); }
    size_t HighWaterMark()    const { return highWaterMark_.load(); }

    void Clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.clear();
    }
    void Shutdown() {
        shutdown_ = true;
        notEmpty_.notify_all();
    }
};
#endif

// ============================================================================
// BinanceWebSocketFeed — v2 depth stream + v3 aggTrade stream
//
// v2 (preserved):
//   depthThread_ — connects to @depth@100ms
//   buffering_   — buffers depth messages during snapshot sync
//   StopBuffering() — flushes depth buffer to messageQueue_
//
// v3 additions:
//   tradeThread_ — connects to @aggTrade (separate SSL connection)
//   Trade messages pushed to the SAME messageQueue_ as depth updates.
//   Trade messages are NOT buffered — historical pre-snapshot trades are
//   irrelevant for microstructure. During buffering_, trade messages are
//   discarded. After StopBuffering(), trades go directly to messageQueue_.
//
// Message discrimination in the main loop (by JSON field detection):
//   aggTrade: contains "e"="aggTrade", "p", "q", "m", "T"
//   depth:    contains "b" (array), "a" (array), "U", "u"
// ============================================================================
class BinanceWebSocketFeed {
private:
    std::string        symbol_;
    std::string        marketType_;
    MessageQueue       messageQueue_;
    PerformanceMetrics metrics_;

    std::atomic<bool>  isConnected_{false};
    std::atomic<bool>  shouldStop_{false};
    std::atomic<bool>  buffering_{true};

    std::thread        depthThread_;   // @depth@100ms
    std::thread        tradeThread_;   // @aggTrade  (v3)

    std::deque<std::string> bufferedMessages_;
    std::mutex              bufferMutex_;

    // ── Internal: build one WebSocket connection to a given target ────────────
    // Returns when shouldStop_ is set or on error (reconnect handled by caller).
    void RunWebSocketStream(const std::string &host,
                            const std::string &port,
                            const std::string &target,
                            bool isDepthStream)      // true=depth (buffered), false=trade
    {
        while (!shouldStop_) {
            try {
                net::io_context ioc;
                ssl::context    ctx{ssl::context::tlsv12_client};
                ctx.set_default_verify_paths();

                tcp::resolver resolver{ioc};
                websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

                auto const results = resolver.resolve(host, port);
                auto &socket = ws.next_layer().next_layer();

                // Network latency optimizations on the underlying TCP socket
                boost::system::error_code ec;
                socket.set_option(tcp::no_delay(true), ec);  // Disable Nagle's algorithm

                // Set receive buffer size (larger = less syscall overhead)
                socket.set_option(boost::asio::socket_base::receive_buffer_size(65536), ec);
                socket.set_option(boost::asio::socket_base::send_buffer_size(65536), ec);

                // Set low-level socket options for performance
                boost::asio::detail::socket_option::boolean<IPPROTO_TCP, TCP_NODELAY> nodelay(true);
                socket.set_option(nodelay, ec);

                net::connect(socket, results.begin(), results.end());
                ws.next_layer().handshake(ssl::stream_base::client);
                ws.handshake(host, target);

                if (isDepthStream) {
                    isConnected_ = true;
                    std::cout << "[WS-Depth] Connected: " << host << target << "\n";
                } else {
                    std::cout << "[WS-Trade] Connected: " << host << target << "\n";
                }

                beast::flat_buffer buffer;
                while (!shouldStop_) {
                    ws.read(buffer);
                    std::string message = beast::buffers_to_string(buffer.data());
                    buffer.consume(buffer.size());
                    metrics_.messagesReceived++;

                    if (isDepthStream) {
                        // Depth: buffered during snapshot sync (FIX 2, 5)
                        if (buffering_) {
                            std::lock_guard<std::mutex> lock(bufferMutex_);
                            if (bufferedMessages_.size() <
                                static_cast<size_t>(HFTConfig::MAX_BUFFERED_MESSAGES))
                                bufferedMessages_.push_back(message);
                        } else {
                            if (!messageQueue_.Push(message))
                                metrics_.messagesDropped++;
                        }
                    } else {
                        // Trade: NOT buffered — skip during snapshot sync.
                        // Historical pre-snapshot trades are not useful for VWAP
                        // warm-up at this stage; live trades start flowing immediately
                        // after StopBuffering().
                        if (!buffering_) {
                            if (!messageQueue_.Push(message))
                                metrics_.messagesDropped++;
                        }
                        // else: discard — pre-snapshot trades not needed
                    }
                }
            } catch (const std::exception &e) {
                std::cerr << (isDepthStream ? "[WS-Depth] " : "[WS-Trade] ")
                          << "Error: " << e.what() << " — reconnecting...\n";
                if (isDepthStream) isConnected_ = false;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(HFTConfig::WS_RECONNECT_DELAY_MS));
            }
        }
    }

public:
    BinanceWebSocketFeed(const std::string &symbol, const std::string &marketType)
        : symbol_(symbol), marketType_(marketType) {}

    ~BinanceWebSocketFeed() { Stop(); }

    void Start() {
        shouldStop_ = false;

        std::string streamName = symbol_;
        std::transform(streamName.begin(), streamName.end(),
                       streamName.begin(), ::tolower);

        // ── Build stream parameters per market type ───────────────────────────
        std::string depthHost, tradeHost, port;
        std::string depthTarget, tradeTarget;

        if (marketType_ == "spot") {
            depthHost   = tradeHost = "stream.binance.com";
            port        = "9443";
            depthTarget = "/ws/" + streamName + "@depth@100ms";
            tradeTarget = "/ws/" + streamName + "@aggTrade";
        } else if (marketType_ == "futures") {
            depthHost   = tradeHost = "fstream.binance.com";
            port        = "443";
            depthTarget = "/ws/" + streamName + "@depth@100ms";
            tradeTarget = "/ws/" + streamName + "@aggTrade";
        } else {
            // coin-margined futures
            depthHost   = tradeHost = "dstream.binance.com";
            port        = "443";
            depthTarget = "/ws/" + streamName + "@depth@100ms";
            tradeTarget = "/ws/" + streamName + "@aggTrade";
        }

        // Launch depth thread
        depthThread_ = std::thread([this, depthHost, port, depthTarget]() {
            RunWebSocketStream(depthHost, port, depthTarget, true);
        });

        // Launch trade thread (v3)
        tradeThread_ = std::thread([this, tradeHost, port, tradeTarget]() {
            RunWebSocketStream(tradeHost, port, tradeTarget, false);
        });
    }

    void Stop() {
        shouldStop_ = true;
        if (depthThread_.joinable()) depthThread_.join();
        if (tradeThread_.joinable()) tradeThread_.join();
    }

    bool IsConnected() const { return isConnected_.load(); }

    MessageQueue       &GetMessageQueue()    { return messageQueue_; }
    PerformanceMetrics &GetMutableMetrics()  { return metrics_; }

    // FIX 2 + 5: flush buffered depth messages to queue
    void StopBuffering() {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        buffering_ = false;
        size_t count = bufferedMessages_.size();
        for (auto &msg : bufferedMessages_)
            messageQueue_.Push(msg);
        bufferedMessages_.clear();
        std::cout << "Flushed " << count << " buffered depth messages to queue.\n";
    }

    bool   IsBuffering()           const { return buffering_.load(); }
    size_t GetBufferedMessageCount()     {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return bufferedMessages_.size();
    }
};

// ============================================================================
// OFI State — UNCHANGED from v2  (FIX 8: persistent, written back after calc)
// ============================================================================
struct OFIState {
    std::vector<double> bidPrice;
    std::vector<double> askPrice;
    std::vector<double> bidQty;
    std::vector<double> askQty;
    bool                initialized = false;
    double              rollingOFI  = 0.0;
};

// ============================================================================
// OrderBookProcessor — UNCHANGED from v2
// ============================================================================
class OrderBookProcessor {
private:
    Orderbook          orderbook_;
    PriceLevelSnapshot currentSnapshot_;
    PriceLevelSnapshot previousSnapshot_;
    mutable std::shared_mutex snapshotMutex_;  // Read-write lock (multiple readers, single writer)
    bool               snapshotLoaded_     = false;
    bool               firstUpdateApplied_ = false;  // FIX 1

public:
    OrderBookProcessor() = default;

    void LoadSnapshot(const nlohmann::json &json) {
        std::unique_lock<std::shared_mutex> lock(snapshotMutex_);
        currentSnapshot_.bids.clear();
        currentSnapshot_.asks.clear();

        for (const auto &bid : json["bids"]) {
            Price    price = static_cast<Price>(
                std::stold(bid[0].get<std::string>()) * 10000);
            Quantity qty   = static_cast<Quantity>(
                std::stold(bid[1].get<std::string>()) * 10000);
            currentSnapshot_.bids[price] = qty;
        }
        for (const auto &ask : json["asks"]) {
            Price    price = static_cast<Price>(
                std::stold(ask[0].get<std::string>()) * 10000);
            Quantity qty   = static_cast<Quantity>(
                std::stold(ask[1].get<std::string>()) * 10000);
            currentSnapshot_.asks[price] = qty;
        }

        currentSnapshot_.sequenceNumber = json["lastUpdateId"];
        previousSnapshot_   = currentSnapshot_;
        snapshotLoaded_     = true;
        firstUpdateApplied_ = false;   // FIX 1

        std::cout << "Snapshot loaded. lastUpdateId: "
                  << currentSnapshot_.sequenceNumber << "\n";
    }

    bool ProcessDepthUpdate(const FastJSON::DepthUpdate &update,
                            PerformanceMetrics   &metrics) {
        if (!snapshotLoaded_) return false;
        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            uint64_t U = update.U;
            uint64_t u = update.u;

            std::unique_lock<std::shared_mutex> lock(snapshotMutex_);  // Exclusive write lock

            // Always drop stale updates (FIX 1 — part a)
            if (u <= currentSnapshot_.sequenceNumber)
                return true;

            // FIX 1: Binance first-event sync rule
            if (!firstUpdateApplied_) {
                if (!(U <= currentSnapshot_.sequenceNumber + 1 &&
                           currentSnapshot_.sequenceNumber + 1 <= u))
                    return true;   // not the bridging update yet — wait
                firstUpdateApplied_ = true;
            }

            // Apply bid updates (rapidjson parsed data - no std::map overhead)
            for (const auto &bid : update.bids) {
                Price    price = static_cast<Price>(bid.price * 10000);
                Quantity qty   = static_cast<Quantity>(bid.qty * 10000);
                if (qty == 0) currentSnapshot_.bids.erase(price);
                else          currentSnapshot_.bids[price] = qty;
            }
            // Apply ask updates
            for (const auto &ask : update.asks) {
                Price    price = static_cast<Price>(ask.price * 10000);
                Quantity qty   = static_cast<Quantity>(ask.qty * 10000);
                if (qty == 0) currentSnapshot_.asks.erase(price);
                else          currentSnapshot_.asks[price] = qty;
            }

            currentSnapshot_.sequenceNumber  = u;
            currentSnapshot_.timestamp       = std::chrono::high_resolution_clock::now();
            previousSnapshot_ = currentSnapshot_;

            // Performance tracking (FIX 9 partial)
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - startTime).count();
            metrics.totalProcessingTimeUs += latency;
            metrics.maxProcessingTimeUs    =
                std::max(metrics.maxProcessingTimeUs.load(), (uint64_t)latency);
            metrics.minProcessingTimeUs    =
                std::min(metrics.minProcessingTimeUs.load(), (uint64_t)latency);
            metrics.messagesProcessed++;
            return true;

        } catch (const std::exception &e) {
            std::cerr << "ProcessDepthUpdate error: " << e.what() << "\n";
            return false;
        }
    }

    PriceLevelSnapshot GetCurrentSnapshotCopy() const {
        std::shared_lock<std::shared_mutex> lock(snapshotMutex_);  // Shared read lock
        return currentSnapshot_;
    }
    // Zero-copy const access (caller must hold shared_lock separately)
    const PriceLevelSnapshot& GetCurrentSnapshotRefUnsafe() const {
        return currentSnapshot_;
    }
    bool   IsSnapshotLoaded() const { return snapshotLoaded_; }
    size_t GetBidCount()      const {
        std::shared_lock<std::shared_mutex> lock(snapshotMutex_);  // Shared read lock
        return currentSnapshot_.bids.size();
    }
    size_t GetAskCount()      const {
        std::shared_lock<std::shared_mutex> lock(snapshotMutex_);  // Shared read lock
        return currentSnapshot_.asks.size();
    }
    double GetBestBid() const {   // FIX 4
        std::shared_lock<std::shared_mutex> lock(snapshotMutex_);  // Shared read lock
        if (currentSnapshot_.bids.empty()) return 0.0;
        return currentSnapshot_.bids.begin()->first / 10000.0;
    }
    double GetBestAsk() const {   // FIX 4
        std::shared_lock<std::shared_mutex> lock(snapshotMutex_);  // Shared read lock
        if (currentSnapshot_.asks.empty()) return 0.0;
        return currentSnapshot_.asks.begin()->first / 10000.0;
    }
};

// ============================================================================
// FormatTimestamp — UNCHANGED from v2
// ============================================================================
static std::string FormatTimestamp(
    const std::chrono::system_clock::time_point &tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// ============================================================================
// PrintOrderbookDisplay — v2 core preserved, v3 microstructure panel appended
//
// IMPORTANT: analytics is non-const ref because analytics.ofi is set here
// (same as v2 fix 8).
// ============================================================================
void PrintOrderbookDisplay(const PriceLevelSnapshot    &snapshot,
                           const std::string           &symbol,
                           MarketAnalytics             &analytics,   // non-const: ofi written here
                           PerformanceMetrics          &metrics,
                           OFIState                    &ofiState,    // FIX 8
                           const MicrostructureResults &micro,       // v3
                           const SignalResults         &sig,         // v4: P1/P2/P3 signals
                           const RiskResults           &risk,        // v5: Risk & PnL
                           const BookDynamicsResults   &bdy,         // v6: Book Dynamics
                           const RegimeResults         &reg,         // v7: Statistical Regime
                           const StrategyResults       &strat,       // v8: Strategy Sim
                           const SecondExchangeFeed    &crossFeed,   // v9: CrossExchangeFeed
                           int                          levels = 20) {

    std::vector<std::pair<Price, Quantity>> bids(
        snapshot.bids.begin(), snapshot.bids.end());
    std::vector<std::pair<Price, Quantity>> asks(
        snapshot.asks.begin(), snapshot.asks.end());

    auto now = std::chrono::system_clock::now();
    std::cout << "\033[2J\033[H";

    // ── Header ────────────────────────────────────────────────────────────────
    std::cout << "========================================\n";
    std::cout << "  LIVE ORDERBOOK (WebSocket): " << symbol << "\n";
    std::cout << "  " << FormatTimestamp(now) << "\n";
    std::cout << "========================================\n\n";

    // ── Order book table (v2 — UNCHANGED) ────────────────────────────────────
    std::cout << std::setw(15) << "BID QTY" << " | "
              << std::setw(12) << "BID PRICE" << " | "
              << std::setw(12) << "ASK PRICE" << " | "
              << std::setw(15) << "ASK QTY" << "\n";
    std::cout << std::string(65, '-') << "\n";

    int maxLevels = std::min({levels, (int)bids.size(), (int)asks.size()});
    for (int i = 0; i < maxLevels; ++i) {
        std::cout << std::setw(15) << std::fixed << std::setprecision(4)
                  << bids[i].second / 10000.0 << " | "
                  << std::setw(12) << std::fixed << std::setprecision(2)
                  << bids[i].first  / 10000.0 << " | "
                  << std::setw(12) << std::fixed << std::setprecision(2)
                  << asks[i].first  / 10000.0 << " | "
                  << std::setw(15) << std::fixed << std::setprecision(4)
                  << asks[i].second / 10000.0 << "\n";
    }
    std::cout << "========================================\n";

    // ── Core analytics (v2 — UNCHANGED) ──────────────────────────────────────
    if (!bids.empty() && !asks.empty()) {
        double bestBid   = bids[0].first  / 10000.0;
        double bestAsk   = asks[0].first  / 10000.0;
        double spread    = bestAsk - bestBid;
        double midPrice  = (bestBid + bestAsk) / 2.0;
        double spreadBps = (spread / midPrice) * 10000.0;

        std::cout << "Mid Price : " << std::fixed << std::setprecision(4)
                  << midPrice << "\n";
        std::cout << "Spread    : " << std::fixed << std::setprecision(4)
                  << spread << "  (" << std::setprecision(2)
                  << spreadBps << " bps)\n";

        // FIX 8: multi-level OFI with persistent state
        int levelsToUse = std::min({HFTConfig::OFI_LEVELS,
                                    (int)bids.size(), (int)asks.size()});
        if (!ofiState.initialized) {
            ofiState.bidPrice.assign(levelsToUse, 0.0);
            ofiState.askPrice.assign(levelsToUse, 0.0);
            ofiState.bidQty  .assign(levelsToUse, 0.0);
            ofiState.askQty  .assign(levelsToUse, 0.0);
            ofiState.initialized = true;
        }

        double ofi = 0.0;
        const std::vector<double> weights = {1.0, 0.8, 0.6, 0.4, 0.2};
        if ((int)ofiState.bidPrice.size() >= levelsToUse) {
            for (int i = 0; i < levelsToUse; ++i) {
                double bid    = bids[i].first;
                double ask    = asks[i].first;
                double bidQty = bids[i].second;
                double askQty = asks[i].second;
                double levelOFI = 0.0;

                if      (bid > ofiState.bidPrice[i]) levelOFI += bidQty;
                else if (bid < ofiState.bidPrice[i]) levelOFI -= ofiState.bidQty[i];
                else                                 levelOFI += (bidQty - ofiState.bidQty[i]);

                if      (ask < ofiState.askPrice[i]) levelOFI += askQty;
                else if (ask > ofiState.askPrice[i]) levelOFI -= ofiState.askQty[i];
                else                                 levelOFI -= (askQty - ofiState.askQty[i]);

                ofi += weights[i] * levelOFI;
            }
        }

        // FIX 8: write back AFTER calculation
        for (int i = 0; i < levelsToUse; ++i) {
            ofiState.bidPrice[i] = bids[i].first;
            ofiState.askPrice[i] = asks[i].first;
            ofiState.bidQty[i]   = bids[i].second;
            ofiState.askQty[i]   = asks[i].second;
        }

        ofiState.rollingOFI =
            HFTConfig::OFI_ALPHA * ofi +
            (1.0 - HFTConfig::OFI_ALPHA) * ofiState.rollingOFI;

        analytics.ofi = ofiState.rollingOFI;   // FIX 8: for BinarySerializer

        double liquidity = 0.0;
        for (int i = 0; i < levelsToUse; ++i)
            liquidity += bids[i].second + asks[i].second;
        double normalizedSignal = ofiState.rollingOFI / (liquidity + 1e-9);

        static auto lastSignalTime = std::chrono::steady_clock::now();
        auto nowSteady = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                nowSteady - lastSignalTime).count() >= HFTConfig::SIGNAL_INTERVAL_MS) {
            lastSignalTime = nowSteady;
            std::cout << "Rolling OFI Signal : " << std::fixed
                      << std::setprecision(6) << normalizedSignal;
            if      (normalizedSignal >  0.2) std::cout << "  → NEXT 5s: UP   ↑\n";
            else if (normalizedSignal < -0.2) std::cout << "  → NEXT 5s: DOWN ↓\n";
            else                              std::cout << "  → NEXT 5s: SIDEWAYS\n";
        }
    }

    // ── Performance (v2 — UNCHANGED) ─────────────────────────────────────────
    std::cout << "Msgs Processed : " << metrics.messagesProcessed << "\n";
    std::cout << "Avg Latency    : " << std::fixed << std::setprecision(3)
              << metrics.GetAverageLatencyMicros() << " μs\n";
    std::cout << "Net Latency    : " << std::fixed << std::setprecision(3)
              << metrics.lastNetworkLatencyUs << " μs\n";   // FIX 9
    std::cout << "Bids/Asks      : "
              << snapshot.bids.size() << " / " << snapshot.asks.size() << "\n";

    // ═══════════════════════════════════════════════════════════════════════════
    // MICROSTRUCTURE PANEL (v3)
    // ═══════════════════════════════════════════════════════════════════════════
    std::cout << "════════════ MICROSTRUCTURE ANALYTICS ═════════════\n";
    std::cout << std::fixed;

    // ── VWAP / TWAP ───────────────────────────────────────────────────────────
    std::cout << "VWAP (60s) : " << std::setprecision(4) << micro.vwap;
    std::cout << "   TWAP (60s) : " << std::setprecision(4) << micro.twap;
    if (micro.vwap > 0.0 && !bids.empty() && !asks.empty()) {
        double midNow = (bids[0].first + asks[0].first) / 20000.0;
        double diff   = midNow - micro.vwap;
        std::cout << "   Mid-VWAP: " << (diff >= 0 ? "+" : "")
                  << std::setprecision(4) << diff
                  << (diff >= 0 ? " (↑ above VWAP)" : " (↓ below VWAP)");
    }
    std::cout << "\n";

    // ── Microprice / WMP ──────────────────────────────────────────────────────
    std::cout << "Microprice : " << std::setprecision(4) << micro.microprice;
    if (micro.microprice > 0.0 && !bids.empty() && !asks.empty()) {
        double plainMid = (bids[0].first + asks[0].first) / 20000.0;
        double diff     = micro.microprice - plainMid;
        std::cout << "   (vs mid: " << (diff >= 0 ? "+" : "")
                  << std::setprecision(5) << diff;
        if      (diff >  0.0001) std::cout << " → BUY pressure)";
        else if (diff < -0.0001) std::cout << " → SELL pressure)";
        else                     std::cout << " → Balanced)";
    }
    std::cout << "\n";

    // ── Last trade + Lee-Ready classification ─────────────────────────────────
    const char *sideStr =
        (micro.lastTradeSide > 0) ? "BUY " :
        (micro.lastTradeSide < 0) ? "SELL" : "??? ";
    std::cout << "Last Trade : " << sideStr
              << " @ " << std::setprecision(4) << micro.lastTradePrice
              << "   Qty: " << std::setprecision(4) << micro.lastTradeQty
              << "   Total trades: " << micro.tradeCount << "\n";
    std::cout << "Signed Flow: " << std::setprecision(2) << micro.signedFlow
              << (micro.signedFlow > 0.0 ? "  (Net BUY)"  : "  (Net SELL)") << "\n";

    // ── Kyle's Lambda ─────────────────────────────────────────────────────────
    // Display: λ × 10^5 so values in range ~0.1 to 100 (easier to read)
    std::cout << "Kyle's λ   : " << std::setprecision(8) << micro.kyleLambda
              << "  (USD per unit signed flow)\n";

    // ── Amihud Illiquidity ────────────────────────────────────────────────────
    std::cout << "Amihud ILLIQ: " << std::setprecision(4) << micro.amihudIlliq
              << " ×10⁻⁶";
    if      (micro.amihudIlliq < 0.5)  std::cout << "  [Very Liquid]\n";
    else if (micro.amihudIlliq < 2.0)  std::cout << "  [Liquid]\n";
    else if (micro.amihudIlliq < 10.0) std::cout << "  [Moderate]\n";
    else                               std::cout << "  [ILLIQUID — caution]\n";

    // ── Roll's Spread ─────────────────────────────────────────────────────────
    std::cout << "Roll Spread: " << std::setprecision(5) << micro.rollSpread
              << " USD";
    if (!bids.empty() && !asks.empty()) {
        double qSpread = (asks[0].first - bids[0].first) / 10000.0;
        if (qSpread > 0.0) {
            double pct = (micro.rollSpread / qSpread) * 100.0;
            std::cout << "  (" << std::setprecision(1) << pct
                      << "% of quoted spread)";
        }
    }
    std::cout << "\n";

    // ═══════════════════════════════════════════════════════════════════════
    // SIGNAL ENGINE PANEL (v4) — P1/P2/P3 HFT Signals
    // ═══════════════════════════════════════════════════════════════════════
    std::cout << "═══════════════ SIGNAL ENGINE (v4) ════════════════\n";
    std::cout << std::fixed;

    // ── P1: Multi-Level OFI (exponential decay, L1-L5) ───────────────────────
    std::cout << "OFI(L1-L5): " << std::setprecision(6) << sig.ofiNormalized;
    if      (sig.ofiNormalized >  0.20) std::cout << "  → STRONG BUY  ↑↑\n";
    else if (sig.ofiNormalized >  0.05) std::cout << "  → moderate buy ↑\n";
    else if (sig.ofiNormalized < -0.20) std::cout << "  → STRONG SELL ↓↓\n";
    else if (sig.ofiNormalized < -0.05) std::cout << "  → moderate sell ↓\n";
    else                                std::cout << "  → balanced\n";

    // ── P1: VPIN ──────────────────────────────────────────────────────────────
    std::cout << "VPIN       : " << std::setprecision(4) << sig.vpin
              << "  (" << sig.vpinBuckets << " buckets)";
    if      (sig.vpin > 0.65) std::cout << "  [HIGH TOXICITY — adverse selection!]\n";
    else if (sig.vpin > 0.40) std::cout << "  [Elevated — monitor]\n";
    else if (sig.vpin > 0.00) std::cout << "  [Normal]\n";
    else                      std::cout << "  [Warming up...]\n";

    // ── P2: Momentum / Mean-Reversion Regime ─────────────────────────────────
    {
        const char *regStr = (sig.regime == +1) ? "TRENDING (+1)  " :
                             (sig.regime == -1) ? "MEAN-REV (-1)  " : "NEUTRAL  ( 0)  ";
        std::cout << "MomMR      : " << regStr
                  << " AR1=" << std::setprecision(4) << sig.ar1Coeff
                  << " Mom=" << std::setprecision(6) << sig.momentumScore
                  << " z=" << std::setprecision(3) << sig.zScore << "\n";
    }

    // ── P2: Iceberg Order Detector ────────────────────────────────────────────
    if (sig.icebergDetected) {
        std::cout << "ICEBERG    : *** DETECTED ***  "
                  << (sig.icebergSide > 0 ? "BID" : "ASK")
                  << " wall @ " << std::setprecision(4) << sig.icebergPrice
                  << "  refills=" << sig.icebergRefills << "\n";
    } else {
        std::cout << "Iceberg    : None detected\n";
    }

    // ── P2: Quote Stuffing ────────────────────────────────────────────────────
    std::cout << "QuoteStuff : ratio=" << std::setprecision(2) << sig.stuffingRatio;
    if (sig.stuffingAlert)
        std::cout << "  *** STUFFING ALERT — possible manipulation ***\n";
    else
        std::cout << "  [normal]\n";

    // ── P3: Spoofing / Layering ───────────────────────────────────────────────
    if (sig.spoofingAlert) {
        std::cout << "SPOOFING   : *** ALERT ***  "
                  << (sig.spoofSide > 0 ? "BID" : "ASK")
                  << " spoof @ " << std::setprecision(4) << sig.spoofPrice
                  << "  cancels=" << sig.spoofCancels << "\n";
    } else {
        std::cout << "Spoofing   : None detected\n";
    }

    // ═══════════════════════════════════════════════════════════════════
    // RISK & PnL ENGINE PANEL (v5) — Advanced position + direction analytics
    // ═══════════════════════════════════════════════════════════════════════
    std::cout << "═══════════════ RISK & PnL ENGINE (v5) ════════════════\n";
    std::cout << std::fixed;

    // ── P1: Simulated Position & PnL ─────────────────────────────────────────
    {
        const char *posStr = (risk.signalDirection > 0) ? "LONG " :
                             (risk.signalDirection < 0) ? "SHORT" : "FLAT ";
        std::cout << "Position   : " << posStr
                  << "  size=" << std::setprecision(4) << risk.positionSize
                  << "  entry=" << std::setprecision(4) << risk.entryPrice
                  << "  notional=$" << std::setprecision(2) << risk.positionNotional
                  << "\n";
        std::cout << "PnL        : Total=$" << std::setprecision(4) << risk.totalPnl
                  << "  Real=$" << std::setprecision(4) << risk.realizedPnl
                  << "  Unreal=$" << std::setprecision(4) << risk.unrealizedPnl
                  << "  Trades=" << risk.totalTrades
                  << "  WinRate=" << std::setprecision(1) << risk.winRate * 100.0 << "%\n";
    }

    // ── P1: Slippage ─────────────────────────────────────────────────────────
    std::cout << "Slippage   : Last=" << std::setprecision(2) << risk.lastSlippageBps
              << " bps  Avg=" << std::setprecision(2) << risk.avgSlippageBps
              << " bps  ImplShortfall=" << std::setprecision(2) << risk.implShortfallBps
              << " bps\n";
    if (risk.lastExecPrice > 0.0) {
        std::cout << "             Exec=$" << std::setprecision(4) << risk.lastExecPrice
                  << "  Arrival=$" << std::setprecision(4) << risk.lastArrivalPrice
                  << "\n";
    }

    // ── P2: Fill Probability ──────────────────────────────────────────────────
    std::cout << "FillProb   : P(fill|100ms)=" << std::setprecision(3) << risk.fillProb100ms
              << "  queue=" << std::setprecision(4) << risk.queueDepthAtL1
              << " units  λ_trade=" << std::setprecision(2) << risk.tradeArrivalRate
              << "/s\n";

    // ── P2: Rolling Risk Metrics ──────────────────────────────────────────────
    std::cout << "Sharpe     : per-trade=" << std::setprecision(3) << risk.sharpePerTrade
              << "  annualised=" << std::setprecision(2) << risk.annualizedSharpe
              << "  peak=$" << std::setprecision(4) << risk.peakEquity << "\n";
    std::cout << "Drawdown   : max=$" << std::setprecision(4) << risk.maxDrawdown
              << "  current=$" << std::setprecision(4) << risk.currentDrawdown;
    if (risk.peakEquity > 0.001)
        std::cout << "  (" << std::setprecision(1)
                  << risk.currentDrawdown / risk.peakEquity * 100.0 << "% of peak)";
    std::cout << "\n";

    // ── P3: Inventory Risk Heatmap ────────────────────────────────────────────
    if (std::abs(risk.positionSize) > 1e-9) {
        std::cout << "DV01       : $" << std::setprecision(5) << risk.dv01
                  << " per bps  "
                  << InventoryRiskHeatmap::HeatBar(risk.heatLevel) << "\n";
        std::cout << "LiquidCost : $" << std::setprecision(5) << risk.liquidationCost
                  << "  MktImpact=$" << std::setprecision(5) << risk.marketImpact << "\n";
    } else {
        std::cout << "DV01       : flat — no inventory risk\n";
    }

    // ── Direction Accuracy: Kalman + Composite + Hit Rate ─────────────────────
    std::cout << "Composite  : score=" << std::setprecision(4) << risk.compositeScore;
    if      (risk.compositeScore >  CompositeDirectionModel::ENTRY_THRESH)
        std::cout << "  → LONG signal  ↑\n";
    else if (risk.compositeScore < -CompositeDirectionModel::ENTRY_THRESH)
        std::cout << "  → SHORT signal ↓\n";
    else
        std::cout << "  → no signal\n";

    std::cout << "Kalman     : price=" << std::setprecision(4) << risk.kalmanPrice
              << "  velocity=" << std::setprecision(6) << risk.kalmanVelocity
              << (risk.kalmanVelocity > 0 ? " (↑)" : " (↓)") << "\n";

    std::cout << "HitRate    : " << std::setprecision(1) << risk.hitRate * 100.0
              << "% accuracy  (" << risk.pendingSignals << " pending)\n";

    // ═══════════════════════════════════════════════════════════════════
    // BOOK DYNAMICS & ORDER FLOW PANEL (v6)
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "══════════ BOOK DYNAMICS & ORDER FLOW (v6) ═════════\n";
    std::cout << std::fixed;

    // ── P1: Book Pressure Heatmap ─────────────────────────────────────────
    {
        // ASCII intensity palette: space < ░ < ▒ < ▓ < █
        static const char* palette[] = {" ","░","▒","▓","█"};
        std::cout << "HeatMap    : [time→old | price: low→high around mid]\n";
        std::cout << "             mid=$" << std::setprecision(4)
                  << bdy.heatmapMidPrice
                  << "  bucket=$" << std::setprecision(2)
                  << bdy.heatmapBucketUSD << "\n";
        for (int r = 0; r < BookDynamicsResults::HM_ROWS; ++r) {
            std::cout << "             |";
            for (int c = 0; c < BookDynamicsResults::HM_COLS; ++c)
                std::cout << palette[std::min(4, (int)bdy.heatmap[r][c])];
            std::cout << "|\n";
        }
    }

    // ── P1: Level Lifetime ────────────────────────────────────────────────
    std::cout << "LvlLife    : "
              << " bid_avg=" << std::setprecision(1) << bdy.avgBidLifetimeMs << "ms"
              << " ask_avg=" << std::setprecision(1) << bdy.avgAskLifetimeMs << "ms"
              << "  shortLived(bid/ask)=" << bdy.shortLivedBid << "/" << bdy.shortLivedAsk
              << "  longLived=" << bdy.longLivedBid << "/" << bdy.longLivedAsk << "\n";
    std::cout << "  Hist(<100ms|1s|10s|60s|>60s): bid[";
    for (int i=0;i<5;++i) std::cout << bdy.bidLifeHist[i] << (i<4?"|":"");
    std::cout << "]  ask[";
    for (int i=0;i<5;++i) std::cout << bdy.askLifeHist[i] << (i<4?"|":"");
    std::cout << "]\n";

    // ── P2: Depth Migration Velocity ──────────────────────────────────────
    std::cout << "WallVel    : "
              << " bid=$" << std::setprecision(4) << bdy.bidWallPrice
              << "(qty=" << std::setprecision(2) << bdy.bidWallQty << ")"
              << " vel=" << std::setprecision(4) << bdy.bidWallVelocity << "$/s"
              << "  ask=$" << std::setprecision(4) << bdy.askWallPrice
              << "(qty=" << std::setprecision(2) << bdy.askWallQty << ")"
              << " vel=" << std::setprecision(4) << bdy.askWallVelocity << "$/s\n";
    if (bdy.compressionAlert)
        std::cout << "  *** WALL COMPRESSION: " << std::setprecision(4)
                  << bdy.compressionRateUSD << " $/s squeeze ***\n";

    // ── P2: Order Book Gradient ───────────────────────────────────────────
    std::cout << "OBGradient : "
              << " bid_dQ/dP=" << std::setprecision(1) << bdy.bidGradientMean
              << "(steep=" << std::setprecision(2) << bdy.bidSteepnessIdx << "x)"
              << " ask_dQ/dP=" << std::setprecision(1) << bdy.askGradientMean
              << "(steep=" << std::setprecision(2) << bdy.askSteepnessIdx << "x)";
    if (bdy.bidCliffLevel >= 0)
        std::cout << " BID-CLIFF@L" << bdy.bidCliffLevel;
    if (bdy.askCliffLevel >= 0)
        std::cout << " ASK-CLIFF@L" << bdy.askCliffLevel;
    if (bdy.thinBookAlert)
        std::cout << " [THIN BOOK]";
    std::cout << "\n";

    // ── P3: Hidden Liquidity ──────────────────────────────────────────────
    std::cout << "HiddenLiq  : "
              << " phantom=" << std::setprecision(3) << bdy.phantomRatio * 100.0 << "%"
              << " cumHidden=" << std::setprecision(2) << bdy.hiddenVolCumulative
              << " detects=" << bdy.hiddenDetectCount;
    if (bdy.hiddenAlert)
        std::cout << "  *** HIDDEN LIQ @ $" << std::setprecision(4)
                  << bdy.lastHiddenPrice
                  << " ratio=" << std::setprecision(2) << bdy.lastExceedanceRatio
                  << "x vol=" << std::setprecision(4) << bdy.lastExceedanceVol
                  << " ***";
    std::cout << "\n";

    // ═══════════════════════════════════════════════════════════════════
    // STATISTICAL REGIME & MARKET STATE PANEL (v7)
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "════════════ STATISTICAL REGIME (v7) ═══════════════\n";
    std::cout << std::fixed;

    // ── P1: Volatility Regime ─────────────────────────────────────────────
    {
        static const char* volColors[] = {"[  LOW  ]","[ NORM  ]","[ HIGH  ]","[EXTREME]"};
        std::cout << "Vol(YZ)    : " << std::setprecision(1)
                  << reg.realizedVolAnnualized << "% ann"
                  << "  tick-vol=" << std::setprecision(6) << reg.realizedVolPerTick
                  << "  regime=" << volColors[std::min(3,reg.volRegime)] << "\n";
    }

    // ── P2: Spread Regime ─────────────────────────────────────────────────
    std::cout << "SpreadReg  : " << std::setprecision(2) << reg.spreadTicks
              << " tks  mean=" << std::setprecision(2) << reg.spreadTickMean
              << " std=" << std::setprecision(2) << reg.spreadTickStd
              << " z=" << std::setprecision(2) << reg.spreadZScore;
    if (reg.flashCrashPrecursor)
        std::cout << "  *** FLASH CRASH PRECURSOR ***";
    else if (reg.spreadWideningAlert)
        std::cout << "  [SPREAD WIDENING]";
    std::cout << "\n";
    std::cout << "  Hist(ticks 0|1|2|3|5|10+): [";
    for (int i=0;i<6;++i) std::cout << reg.spreadHist[i] << (i<5?"|":"");
    std::cout << "]\n";

    // ── P2: Autocorrelation of Mid Returns ────────────────────────────────
    {
        const char *acfReg = (reg.autocorrRegime==+1) ? "MOMENTUM  (+1)" :
                             (reg.autocorrRegime==-1) ? "MEAN-REV  (-1)" : "NEUTRAL    (0)";
        std::cout << "MidACF     : rho1=" << std::setprecision(4) << reg.autocorrLag1
                  << "  z=" << std::setprecision(2) << reg.autocorrZScore
                  << "  regime=" << acfReg << "\n";
    }

    // ── P2: Hurst Exponent ────────────────────────────────────────────────
    {
        const char *hurstReg = (reg.hurstRegime==+1) ? "TRENDING   " :
                               (reg.hurstRegime==-1) ? "MEAN-REVERT" : "RANDOM-WALK";
        std::cout << "Hurst(R/S) : H=" << std::setprecision(4) << reg.hurstExponent
                  << "  " << hurstReg
                  << (reg.hurstReliable ? "  [reliable]" : "  [warming up]") << "\n";
    }

    // ── P3: HMM State ─────────────────────────────────────────────────────
    {
        const char *hmmStr = (reg.hmmState==0) ? "BULL_MICRO" : "BEAR_MICRO";
        std::cout << "HMM State  : " << hmmStr
                  << "  P(bull)=" << std::setprecision(3) << reg.hmmBullProb
                  << "  P(bear)=" << std::setprecision(3) << reg.hmmBearProb;
        if (reg.hmmTransitionProb > 0.5) std::cout << "  [REGIME CHANGE]";
        if (reg.hmmConfident)             std::cout << "  [CONFIDENT]";
        std::cout << "\n";
    }

    // ── Direction Accuracy: Regime-Adjusted Score ─────────────────────────
    {
        const char *radStr = (reg.regimeAdjustedDir==+1) ? "LONG  ↑" :
                             (reg.regimeAdjustedDir==-1) ? "SHORT ↓" : "FLAT";
        std::cout << "RegScore   : adj=" << std::setprecision(4)
                  << reg.regimeAdjustedScore
                  << "  dir=" << radStr
                  << "  conf=" << std::setprecision(2) << reg.regimeConfidence
                  << "  edge=" << std::setprecision(3) << reg.edgeScore << "bps\n";
    }

    // ═══════════════════════════════════════════════════════════════════
    // STRATEGY SIMULATION & BACKTESTING PANEL (v8)
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "══════════ STRATEGY SIM & BACKTESTING (v8) ═════════\n";
    std::cout << std::fixed;

    // ── P1: Avellaneda-Stoikov MM Quotes (bps-normalised) ─────────────────
    if (strat.asQuotingActive) {
        std::cout << "AS-MM Quot : bid=$" << std::setprecision(4) << strat.asBid
                  << "  ask=$"    << std::setprecision(4) << strat.asAsk
                  << "  r=$"      << std::setprecision(4) << strat.asReservation
                  << "  δ*="      << std::setprecision(3) << strat.asOptimalSpreadBps << "bps"
                  << "($"         << std::setprecision(4) << strat.asOptimalSpreadUSD << ")"
                  << "  σ="       << std::setprecision(2) << strat.asSigmaBps << "bps/tk\n";
    } else {
        std::cout << "AS-MM Quot : *** GATED [" << strat.asGateReason << "] ***\n";
    }
    std::cout << "AS-Params  : γ=" << std::setprecision(4) << strat.asGamma
              << "  κ="            << std::setprecision(3) << strat.asKappa
              << "  skew="         << std::setprecision(3) << strat.asSkewBps << "bps"
              << "($"              << std::setprecision(5) << strat.asSkewUSD << ")"
              << (strat.asSkewBps > 0.1 ? " → pull ask" :
                  strat.asSkewBps < -0.1? " → pull bid" : "")
              << "  Kelly="        << std::setprecision(3) << strat.mmKellyUnit
              << "  P(fill)="      << std::setprecision(3) << strat.mmProbFill100ms << "\n";

    // ── P1: MM PnL & Inventory ────────────────────────────────────────────
    std::cout << "MM PnL     : net=$"    << std::setprecision(4) << strat.mmNetPnl
              << "  real=$"   << std::setprecision(4) << strat.mmRealizedPnl
              << "  unreal=$" << std::setprecision(4) << strat.mmUnrealizedPnl
              << "  spdCap=$" << std::setprecision(4) << strat.mmSpreadCapture
              << "  advSel=$" << std::setprecision(4) << strat.mmAdvSelection << "\n";
    std::cout << "MM Stats   : inv="     << std::setprecision(4) << strat.mmInventory
              << "  trips="   << strat.mmRoundTrips
              << "  WR="      << std::setprecision(1) << strat.mmWinRate*100.0 << "%"
              << "  fills="   << strat.mmFills
              << "  rate="    << std::setprecision(1) << strat.mmFillRate << "/min";
    if (strat.mmInventoryAlert) std::cout << "  *** INV ALERT ***";
    if (strat.mmQuotingGated)   std::cout << "  [GATED:" << strat.asGateReason << "]";
    std::cout << "\n";

    // ── P2: Latency Arbitrage ─────────────────────────────────────────────
    std::cout << "LatencyArb : sim=" << StrategyResults::LAT_SIM_US << "\xc2\xb5s"
              << "  edge="     << std::setprecision(3) << strat.latEdgeBps << "bps"
              << "  cum=$"     << std::setprecision(4) << strat.latCumEdgeUSD
              << "  Sharpe="   << std::setprecision(2) << strat.latSharpe
              << "  opps="     << strat.latOpportunities << "\n";

    // ── P2: Signal Replay ASCII Chart ─────────────────────────────────────
    std::cout << "Replay(30s): lo=$" << std::setprecision(2) << strat.replayMinPrice
              << " hi=$"    << std::setprecision(2) << strat.replayMaxPrice
              << " simPnL=$" << std::setprecision(4) << strat.replaySimPnl
              << " WR="     << std::setprecision(1) << strat.replayWinRate*100.0 << "%"
              << " MAE=$"   << std::setprecision(5) << strat.replayMAE
              << " MFE=$"   << std::setprecision(5) << strat.replayMFE << "\n";
    for (int rr2 = 0; rr2 < StrategyResults::CHART_ROWS; ++rr2)
        std::cout << "             |" << strat.replayChart[rr2] << "|\n";
    std::cout << "             ^=BUY v=SELL +=EXIT *=signal -=price\n";

    // ── P3: Cross-Exchange Arbitrage ──────────────────────────────────────
    std::cout << "XchgArb    : Binance bid=" << std::setprecision(4) << strat.exchBid1
              << " ask="    << std::setprecision(4) << strat.exchAsk1
              << "  "       << strat.exchName
              << (strat.exchConnected ? "[conn]" : "[disc]")
              << " bid="    << std::setprecision(4) << strat.exchBid2
              << " ask="    << std::setprecision(4) << strat.exchAsk2 << "\n";
    std::cout << "           midSpr="  << std::setprecision(2) << strat.exchMidSpreadBps
              << "bps  netArb=" << std::setprecision(2) << strat.exchArbBps << "bps"
              << "  cnt="    << strat.exchArbCount
              << "  cum="    << std::setprecision(1) << strat.exchCumArbBps << "bps";
    if (strat.exchArbAlert)
        std::cout << "  *** ARB! " << std::setprecision(2)
                  << strat.exchArbBps << "bps NET ***";
    std::cout << "\n";

    // ═══════════════════════════════════════════════════════════════════
    // CROSS-EXCHANGE FEED PANEL (v9 — CrossExchangeFeed.h)
    // ═══════════════════════════════════════════════════════════════════
    std::cout << "══════════ CROSS-EXCHANGE FEED (v9) ═══════════════\n";
    std::cout << std::fixed;
    {
        double cfBid  = crossFeed.GetBid();
        double cfAsk  = crossFeed.GetAsk();
        bool   cfConn = crossFeed.IsConnected();
        const std::string& cfName = crossFeed.GetExchangeName();
        bool   cfSpot = crossFeed.IsSpot();

        double cfMid       = (cfBid > 0 && cfAsk > 0) ? (cfBid + cfAsk) / 2.0 : 0.0;
        double cfSpread    = (cfBid > 0 && cfAsk > 0) ? (cfAsk - cfBid)       : 0.0;
        double cfSpreadBps = (cfMid > 0) ? (cfSpread / cfMid) * 10000.0       : 0.0;

        // Binance L1 from the snapshot for drift calculation
        double binBid = 0.0, binAsk = 0.0, binMid = 0.0;
        if (!bids.empty()) binBid = bids[0].first / 10000.0;
        if (!asks.empty()) binAsk = asks[0].first / 10000.0;
        if (binBid > 0 && binAsk > 0) binMid = (binBid + binAsk) / 2.0;

        double midDrift    = (binMid > 0 && cfMid > 0) ? (cfMid - binMid)              : 0.0;
        double midDriftBps = (binMid > 0)              ? (midDrift / binMid) * 10000.0  : 0.0;

        // Cross-exchange implied arb (buy low / sell high across exchanges)
        double arbBuy  = (cfBid > 0 && binAsk > 0) ? (cfBid - binAsk)              : 0.0;  // sell cf, buy bin
        double arbSell = (binBid > 0 && cfAsk > 0) ? (binBid - cfAsk)              : 0.0;  // sell bin, buy cf
        double bestArb    = std::max(arbBuy, arbSell);
        double bestArbBps = (binMid > 0) ? (bestArb / binMid) * 10000.0 : 0.0;

        std::cout << "Feed       : " << cfName
                  << (cfSpot ? " (SPOT)" : " (PERP)")
                  << "  " << (cfConn ? "\033[32m[CONNECTED]\033[0m" : "\033[31m[DISCONNECTED]\033[0m")
                  << "\n";
        std::cout << "L1 Quote   : bid=" << std::setprecision(4) << cfBid
                  << "  ask=" << std::setprecision(4) << cfAsk
                  << "  mid=" << std::setprecision(4) << cfMid
                  << "  spread=" << std::setprecision(4) << cfSpread
                  << " (" << std::setprecision(2) << cfSpreadBps << "bps)\n";
        std::cout << "Drift      : " << cfName << " mid vs Binance mid: "
                  << (midDrift >= 0 ? "+" : "")
                  << std::setprecision(5) << midDrift
                  << " (" << (midDriftBps >= 0 ? "+" : "")
                  << std::setprecision(2) << midDriftBps << "bps)";
        if (std::abs(midDriftBps) > 5.0)
            std::cout << "  *** LARGE DRIFT! ***";
        else if (std::abs(midDriftBps) > 2.0)
            std::cout << "  [drift]";
        std::cout << "\n";
        std::cout << "Arb Opp    : bestNet=" << std::setprecision(5) << bestArb
                  << " (" << std::setprecision(2) << bestArbBps << "bps)"
                  << (bestArbBps > 1.0 ? "  *** ACTIONABLE ARB! ***" : "")
                  << "\n";
    }

    std::cout << "========================================\n";
    std::cout << "Press Ctrl+C to exit...\n";
}

// REMOVED DUPLICATE - FastJSON namespace is now at top of file

// ============================================================================
// ProcessOneMessage — ULTRA-FAST VERSION using rapidjson
// Handles ONLY depth update messages. aggTrade messages will be routed
// separately in the main loop.
// ============================================================================
static bool ProcessOneMessage(const MessageQueue::Message &msg,
                              OrderBookProcessor          &processor,
                              PerformanceMetrics          &metrics) {
    // ULTRA-FAST: Use rapidjson parser (10x faster than nlohmann)
    auto update = FastJSON::ParseDepthUpdate(msg.data);

    if (!update.hasBids && !update.hasAsks)
        return true;   // not a depth update — skip (aggTrade lands here too,
                       // since aggTrade has no "b" or "a" array — safe)

    // FIX 9: network latency from message receive time
    auto networkLatency =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - msg.receivedTime)
        .count();
    metrics.lastNetworkLatencyUs = static_cast<double>(networkLatency);

    return processor.ProcessDepthUpdate(update, metrics);
}

// ============================================================================
// v4 helper: Σ|ΔQty| from a depth update JSON message (bid + ask arrays).
// Called BEFORE ProcessDepthUpdate so no extra parsing pass is needed.
// Input qtys are raw string values (pre ÷10000 encoding), so result is in
// the same scale; SignalEngine.OnDepthUpdate receives them divided by 10000.
// Used by QuoteStuffingDetector (rolling msg/qty ratio over 1 second).
// ============================================================================
static double ComputeTotalQtyChange(const nlohmann::json &j) {
    double total = 0.0;
    if (j.contains("b"))
        for (const auto &bid : j["b"])
            total += std::abs(std::stod(bid[1].get<std::string>()));
    if (j.contains("a"))
        for (const auto &ask : j["a"])
            total += std::abs(std::stod(ask[1].get<std::string>()));
    return total;   // raw units (divide by 10000 before passing to SignalEngine)
}

// ============================================================================
// main() — structure IDENTICAL to v2; microstructure logic surgically inserted
// ============================================================================
int main(int argc, char *argv[]) {
    std::cout << "[INIT] Starting HFT Engine..." << std::endl;

    std::string symbol           = "SOLUSDT";
    std::string marketType       = "spot";
    int         displayLevels    = 20;
    int         refreshIntervalMs = 1000;

    if (argc > 1) symbol            = argv[1];
    if (argc > 2) marketType        = argv[2];
    if (argc > 3) displayLevels     = std::stoi(argv[3]);
    if (argc > 4) refreshIntervalMs = std::stoi(argv[4]);

    std::cout << "[INIT] Symbol: " << symbol << " Market: " << marketType << std::endl;

    std::transform(symbol.begin(),     symbol.end(),     symbol.begin(),     ::toupper);
    std::transform(marketType.begin(), marketType.end(), marketType.begin(), ::tolower);

    std::cout << "========================================\n";
    std::cout << "  Binance WebSocket Market Data Engine v3 (Lock-Free)\n";
    std::cout << "  Symbol: " << symbol << "  Market: " << marketType << "\n";
    std::cout << "  Levels: " << displayLevels << "\n";
    std::cout << "========================================\n";

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Winsock init failed\n";
        return 1;
    }
    std::cout << "[INIT] Winsock OK" << std::endl;

    // Set CPU affinity to core 0 (reduces cache misses from CPU migration)
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 0x0001);  // Pin to CPU 0
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    std::cout << "[INIT] CPU pinned to core 0, REALTIME priority" << std::endl;
#endif

    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::cout << "[INIT] CURL OK" << std::endl;

    try {
        BinanceWebSocketFeed wsFeeds(symbol, marketType);
        OrderBookProcessor   processor;
        OFIState             ofiState;                // FIX 8: persistent
        MicrostructureEngine micro;                   // v3: all 6 algorithms
        SignalEngine         signals;                 // v4: P1/P2/P3 signals
        RiskEngine           risk;                    // v5: Risk & PnL engine
        BookDynamicsEngine   bookDyn;                 // v6: Book Dynamics
        RegimeEngine         regime;                  // v7: Statistical Regime
        StrategyEngine       strat;                   // v8: Strategy Simulation
        SecondExchangeFeed   crossFeed;                // v9: Standalone CrossExchangeFeed
        PerformanceMetrics  &metrics = wsFeeds.GetMutableMetrics();

        // v8: start second exchange feed (Bybit) for cross-exchange arb
        strat.GetSecondFeed().Start(symbol);

        // v9: start standalone CrossExchangeFeed (Bybit spot by default)
        {
            ExchangeFeedParams cfParams;
            cfParams.exchange = "Bybit";
            cfParams.symbol   = symbol;
            cfParams.mktType  = ExchMarketType::SPOT;
            crossFeed.Start(cfParams);
            std::cout << "[INIT] CrossExchangeFeed started: "
                      << cfParams.exchange << " " << symbol
                      << " (" << (cfParams.mktType == ExchMarketType::SPOT ? "SPOT" : "PERP") << ")\n";
        }

        // v3: track last mid price for Kyle's lambda Δmid calculation
        double lastMid = 0.0;

        TcpSender sender("127.0.0.1", 9001);
        if (!sender.Connect())
            std::cerr << "TCP gateway connection failed — continuing without sender\n";

        // ====================================================================
        // STEP 1 — Start WebSocket (depth buffering ON; trade stream live but
        //          discarding during buffering)
        // ====================================================================
        wsFeeds.Start();
        std::cout << "Waiting for WebSocket connection (depth stream)...\n";
        while (!wsFeeds.IsConnected())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "WebSocket connected. Buffering depth updates...\n";

        // ====================================================================
        // STEP 2 — Fetch REST snapshot with retry  (FIX 6)
        // ====================================================================
        std::string snapshotJson =
            FetchBinanceOrderbookWithRetry(symbol, marketType,
                                           HFTConfig::SNAPSHOT_DEPTH);
        if (snapshotJson.empty()) {
            std::cerr << "Fatal: could not fetch snapshot after "
                      << HFTConfig::SNAPSHOT_RETRY_COUNT << " attempts\n";
            return 1;
        }
        auto snapshotJsonParsed = nlohmann::json::parse(snapshotJson);
        processor.LoadSnapshot(snapshotJsonParsed);

        // ====================================================================
        // STEP 3 — Stop buffering: flush buffer → queue atomically
        // ====================================================================
        wsFeeds.StopBuffering();

        // ====================================================================
        // STEP 4 — Drain buffered messages FIRST  (FIX 2)
        //
        // After StopBuffering(), the queue contains:
        //   - Depth messages that arrived during the REST fetch (buffered)
        //   - Possibly NO trade messages (they were discarded during buffering)
        //
        // ProcessOneMessage() already handles aggTrade safely: aggTrade has no
        // "b" array, so it falls through with return true (skip silently).
        // ====================================================================
        std::cout << "Draining buffered messages...\n";
        {
            MessageQueue::Message bufferedMsg;
            int applied = 0, skipped = 0;
            while (wsFeeds.GetMessageQueue().TryPop(bufferedMsg)) {
                bool ok = ProcessOneMessage(bufferedMsg, processor, metrics);
                if (!ok) {
                    // FIX 3: gap inside buffer — reload snapshot
                    std::cout << "Gap in buffer. Reloading snapshot...\n";
                    wsFeeds.GetMessageQueue().Clear();
                    snapshotJson =
                        FetchBinanceOrderbookWithRetry(symbol, marketType,
                                                       HFTConfig::SNAPSHOT_DEPTH);
                    if (snapshotJson.empty()) {
                        std::cerr << "Fatal: snapshot retry failed during buffer drain\n";
                        return 1;
                    }
                    processor.LoadSnapshot(nlohmann::json::parse(snapshotJson));
                    skipped++;
                    break;
                }
                applied++;
            }
            std::cout << "Buffer drain complete. Applied=" << applied
                      << " GapResyncs=" << skipped << "\n";
        }

        std::cout << "Snapshot sync complete. Starting live processing...\n";

        // ====================================================================
        // STEP 5 — Live processing loop
        // ====================================================================
        auto lastDisplayTime = std::chrono::steady_clock::now();
        auto lastStatsTime   = std::chrono::steady_clock::now();
        MarketAnalytics analytics;   // FIX 10: persistent

        std::vector<MessageQueue::Message> batch;
        batch.reserve(HFTConfig::BATCH_PROCESS_SIZE);

        while (true) {
            // FIX 7: Batch pop
            size_t count = wsFeeds.GetMessageQueue().PopBatch(
                batch, HFTConfig::BATCH_PROCESS_SIZE);

            if (count == 0) {
                // Ultra-low latency: use yield instead of sleep
                // Sleep adds 50μs latency, yield is ~1-2μs
                for (int i = 0; i < 5; ++i) {
                    if (wsFeeds.GetMessageQueue().PopBatch(batch, 1) > 0) {
                        // Got message, break to processing
                        count = 1;
                        goto process_batch;  // Break out of for loop, skip sleep
                    }
                    std::this_thread::yield();
                }
                // Still no message after yields, sleep briefly
                std::this_thread::sleep_for(std::chrono::microseconds(5));
                continue;
            }
            process_batch:

            for (auto &msg : batch) {
                try {
                    // ULTRA-FAST: Use rapidjson instead of nlohmann (10x faster parsing)
                    auto rapidAgg = FastJSON::ParseAggTrade(msg.data);
                    auto rapidDepth = FastJSON::ParseDepthUpdate(msg.data);

                    // FIX 9: network latency from message receive time
                    auto netLat = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - msg.receivedTime)
                        .count();
                    metrics.lastNetworkLatencyUs = static_cast<double>(netLat);

                    // Check if this is an aggTrade first (fastest path)
                    bool isAggTrade = (rapidAgg.price > 0.0 && rapidAgg.qty > 0.0);

                    if (isAggTrade) {
                        // ── v3: Process aggTrade → MicrostructureEngine ───────
                        double tradePrice = rapidAgg.price;
                        double tradeQty   = rapidAgg.qty;
                        bool   buyerMM    = rapidAgg.buyerMaker;
                        int64_t tradeUs   = rapidAgg.tradeTime * 1000LL;

                        TradeSide side = buyerMM ? TradeSide::SELL : TradeSide::BUY;

                        double midBefore = lastMid;
                        double midAfter  = lastMid;

                        micro.OnTrade(tradePrice, tradeQty, side,
                                      tradeUs, midBefore, midAfter);

                        signals.OnTrade(tradePrice, tradeQty, side, tradeUs, lastMid);

                        risk.OnTrade(tradePrice, tradeQty,
                                     static_cast<int>(side));

                        bookDyn.OnTrade(tradePrice, tradeQty, buyerMM);

                        // v8: build StrategyContext then call OnTrade
                        {
                            StrategyContext sctx;
                            sctx.mid                  = lastMid;
                            sctx.microprice           = analytics.microprice;
                            sctx.spreadUSD            = 0.01;  // approx during trade
                            sctx.sigmaLogReturnPerTick= regime.GetResults().realizedVolPerTick;
                            sctx.tradeRatePerSec      = risk.GetResults().tradeArrivalRate;
                            sctx.avgTradeSizeUnits    = tradeQty;
                            sctx.ofiNorm              = signals.GetResults().ofiNormalized;
                            sctx.vpin                 = signals.GetResults().vpin;
                            sctx.regimeAdjScore       = regime.GetResults().regimeAdjustedScore;
                            sctx.regimeAdjDir         = regime.GetResults().regimeAdjustedDir;
                            sctx.kalmanVelocity       = risk.GetResults().kalmanVelocity;
                            sctx.hitRate              = risk.GetResults().hitRate;
                            sctx.edgeScore            = regime.GetResults().edgeScore;
                            sctx.hmmState             = regime.GetResults().hmmState;
                            sctx.hmmConfident         = regime.GetResults().hmmConfident;
                            sctx.volRegime            = regime.GetResults().volRegime;
                            sctx.flashCrashPrecursor  = regime.GetResults().flashCrashPrecursor;
                            sctx.spreadWideningAlert  = regime.GetResults().spreadWideningAlert;
                            strat.OnTrade(tradePrice, tradeQty, buyerMM, sctx, tradeUs);
                        }

                        // Send trade to UI via TCP gateway
                        sender.SendTrade(
                            static_cast<int32_t>(tradePrice * 10000),
                            static_cast<uint32_t>(tradeQty * 10000),
                            !buyerMM,   // buyerMaker=true → taker sold → isBuy=false
                            metrics.sequenceNumber.load());

                        continue;  // do NOT process as depth
                    }

                    // ── depth update path (rapdjson) ──────────────────────────
                    if (!rapidDepth.hasBids && !rapidDepth.hasAsks)
                        continue;   // unknown message type — skip

                    // v4: compute Σ|ΔQty| BEFORE applying update (for QuoteStuffing)
                    // Divide by 10000: rapidjson already parsed the values
                    double totalQtyChange = 0.0;
                    for (const auto &bid : rapidDepth.bids)
                        totalQtyChange += std::abs(bid.qty);
                    for (const auto &ask : rapidDepth.asks)
                        totalQtyChange += std::abs(ask.qty);
                    totalQtyChange /= 10000.0;

                    // Capture mid BEFORE applying this depth update (for Kyle's λ)
                    double midBefore = (processor.GetBestBid() + processor.GetBestAsk()) / 2.0;

                    if (!processor.ProcessDepthUpdate(rapidDepth, metrics)) {
                        // FIX 3: sequence gap
                        std::cout << "Sequence gap in live stream. Resyncing...\n";
                        wsFeeds.GetMessageQueue().Clear();
                        batch.clear();
                        snapshotJson = FetchBinanceOrderbookWithRetry(
                            symbol, marketType, HFTConfig::SNAPSHOT_DEPTH);
                        if (snapshotJson.empty()) {
                            std::cerr << "Snapshot retry failed — waiting 2s\n";
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            break;
                        }
                        processor.LoadSnapshot(nlohmann::json::parse(snapshotJson));
                        break;   // restart batch with clean state
                    }

                    // ── ULTRA-LOW LATENCY PATH: Minimal per-message processing ──
                    // ZERO COPY: Don't copy 2000+ levels on every message!
                    // Instead, just get the top-of-book (L1) we actually need
                    double bestBid = processor.GetBestBid();
                    double bestAsk = processor.GetBestAsk();
                    lastMid = (bestBid + bestAsk) / 2.0;

                    metrics.lastSpreadBps = (bestAsk > bestBid) ?
                        ((bestAsk - bestBid) / lastMid) * 10000.0 : 0.0;
                    metrics.currentMidPrice = lastMid;

                    auto now = std::chrono::steady_clock::now();

                    // ── Display on timer ───────────────────────────────────────
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - lastDisplayTime).count() >= refreshIntervalMs) {

                        // Copy snapshot ONCE for display (not on every message!)
                        PriceLevelSnapshot snap = processor.GetCurrentSnapshotCopy();

                        // Run full analytics ONCE per second (not per message)
                        // This is the latency optimization: defer expensive analytics
                        analytics.Calculate(snap);  // fills midPrice, spreadBps, imbalance

                        // v3: notify microstructure engine of updated book top
                        if (!snap.bids.empty() && !snap.asks.empty()) {
                            double bidQ = static_cast<double>(snap.bids.begin()->second) / 10000.0;
                            double askQ = static_cast<double>(snap.asks.begin()->second) / 10000.0;
                            micro.OnBookUpdate(
                                analytics.midPrice - analytics.spreadBps / 20000.0,
                                analytics.midPrice + analytics.spreadBps / 20000.0,
                                bidQ, askQ);
                            double exactBid = snap.bids.begin()->first / 10000.0;
                            double exactAsk = snap.asks.begin()->first / 10000.0;
                            micro.OnBookUpdate(exactBid, exactAsk, bidQ, askQ);
                        }

                        // v3: copy microstructure results into analytics
                        analytics.CopyFromMicro(micro.GetResults());

                        // v4-v8: Update all signal/risk/strategy engines once per second
                        TopOfBook tob = ExtractTopOfBook(snap);
                        int64_t nowUs = MicrostructureEngine::CurrentUsEpoch();
                        signals.OnDepthUpdate(tob, 0.0, nowUs);  // No qty change for display

                        if (!snap.bids.empty() && !snap.asks.empty()) {
                            double bidQ1 = snap.bids.begin()->second / 10000.0;
                            double askQ1 = snap.asks.begin()->second / 10000.0;
                            double bid1  = snap.bids.begin()->first  / 10000.0;
                            double ask1  = snap.asks.begin()->first  / 10000.0;
                            double spreadUSD = ask1 - bid1;
                            risk.OnDepthUpdate(
                                signals.GetResults(),
                                analytics.midPrice,
                                analytics.microprice,
                                spreadUSD,
                                bid1, ask1,
                                bidQ1, askQ1,
                                analytics.imbalance,
                                micro.GetResults().kyleLambda,
                                nowUs);

                            bookDyn.OnDepthUpdate(snap, nowUs);

                            regime.OnDepthUpdate(
                                analytics.midPrice,
                                spreadUSD,
                                0.01,
                                signals.GetResults(),
                                risk.GetResults().compositeScore,
                                signals.GetResults().vpin,
                                risk.GetResults().hitRate,
                                nowUs);

                            StrategyContext sctx8;
                            sctx8.mid                   = analytics.midPrice;
                            sctx8.microprice            = analytics.microprice;
                            sctx8.spreadUSD             = spreadUSD;
                            sctx8.bid1                  = bid1;
                            sctx8.ask1                  = ask1;
                            sctx8.bidQ1                 = bidQ1;
                            sctx8.askQ1                 = askQ1;
                            sctx8.sigmaLogReturnPerTick = regime.GetResults().realizedVolPerTick;
                            sctx8.tradeRatePerSec       = risk.GetResults().tradeArrivalRate;
                            sctx8.kyleLambda            = micro.GetResults().kyleLambda;
                            sctx8.avgTradeSizeUnits     = 0.10;
                            sctx8.ofiNorm               = signals.GetResults().ofiNormalized;
                            sctx8.vpin                  = signals.GetResults().vpin;
                            sctx8.momentumScore         = signals.GetResults().momentumScore;
                            sctx8.momentumRegime        = signals.GetResults().regime;
                            sctx8.compositeScore        = risk.GetResults().compositeScore;
                            sctx8.regimeAdjScore        = regime.GetResults().regimeAdjustedScore;
                            sctx8.regimeAdjDir          = regime.GetResults().regimeAdjustedDir;
                            sctx8.kalmanVelocity        = risk.GetResults().kalmanVelocity;
                            sctx8.hitRate               = risk.GetResults().hitRate;
                            sctx8.edgeScore             = regime.GetResults().edgeScore;
                            sctx8.realizedVolPerTick    = regime.GetResults().realizedVolPerTick;
                            sctx8.realizedVolAnnualized = regime.GetResults().realizedVolAnnualized;
                            sctx8.volRegime             = regime.GetResults().volRegime;
                            sctx8.hmmState              = regime.GetResults().hmmState;
                            sctx8.hmmConfident          = regime.GetResults().hmmConfident;
                            sctx8.hmmBullProb           = regime.GetResults().hmmBullProb;
                            sctx8.hurstExponent         = regime.GetResults().hurstExponent;
                            sctx8.hurstRegime           = regime.GetResults().hurstRegime;
                            sctx8.autocorrLag1          = regime.GetResults().autocorrLag1;
                            sctx8.flashCrashPrecursor   = regime.GetResults().flashCrashPrecursor;
                            sctx8.spreadWideningAlert   = regime.GetResults().spreadWideningAlert;
                            strat.OnDepthUpdate(sctx8, nowUs);
                        }

                        metrics.lastSpreadBps   = analytics.spreadBps;
                        metrics.currentMidPrice = analytics.midPrice;

                        // FIX 8: ofiState written inside PrintOrderbookDisplay
                        // v3: pass micro.GetResults() for microstructure panel
                        PrintOrderbookDisplay(snap, symbol, analytics,
                                              metrics, ofiState,
                                              micro.GetResults(),
                                              signals.GetResults(),   // v4
                                              risk.GetResults(),      // v5
                                              bookDyn.GetResults(),   // v6
                                              regime.GetResults(),    // v7
                                              strat.GetResults(),     // v8
                                              crossFeed,              // v9
                                              displayLevels);

                        sender.SendSnapshot(snap, metrics, analytics);

                        // v3: send dedicated microstructure TCP frame
                        sender.SendMicrostructure(micro.GetResults());

                        // v4-v8: send all engine results to web UI via TCP
                        sender.SendSignals(signals.GetResults());
                        sender.SendRisk(risk.GetResults());
                        sender.SendBookDynamics(bookDyn.GetResults());
                        sender.SendRegime(regime.GetResults());
                        sender.SendStrategy(strat.GetResults());

                        // v9: send standalone cross-exchange feed to UI
                        {
                            double cfBid = crossFeed.GetBid();
                            double cfAsk = crossFeed.GetAsk();
                            bool   cfConn = crossFeed.IsConnected();
                            bool   cfSpot = crossFeed.IsSpot();
                            double binMid = analytics.midPrice;
                            sender.SendCrossExchange(cfBid, cfAsk, cfConn, cfSpot, binMid);
                        }

                        lastDisplayTime = now;
                    }

                    // ── Stats on timer ─────────────────────────────────────────
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - lastStatsTime).count() >=
                        HFTConfig::STATS_PRINT_INTERVAL_MS) {

                        {
                            const SignalResults &sr = signals.GetResults();
                            std::cout << "[Stats] Queue="
                                      << wsFeeds.GetMessageQueue().Size()
                                      << " Dropped="
                                      << wsFeeds.GetMessageQueue().DroppedMessages()
                                      << " HWM="
                                      << wsFeeds.GetMessageQueue().HighWaterMark()
                                      << " MsgsProcessed="
                                      << metrics.messagesProcessed
                                      << " Trades="
                                      << micro.GetResults().tradeCount
                                      << " VWAP="
                                      << std::fixed << std::setprecision(4)
                                      << micro.GetResults().vwap
                                      << " λ="
                                      << std::setprecision(8)
                                      << micro.GetResults().kyleLambda
                                      << " VPIN="
                                      << std::setprecision(4) << sr.vpin
                                      << " OFI="
                                      << std::setprecision(4) << sr.ofiNormalized
                                      << " Regime="
                                      << (sr.regime == +1 ? "TREND" :
                                          sr.regime == -1 ? "MR"    : "NEUT")
                                      << (sr.stuffingAlert  ? " [STUFFING!]" : "")
                                      << (sr.spoofingAlert  ? " [SPOOF!]"    : "")
                                      << (sr.icebergDetected? " [ICE!]"      : "")
                                      << "\n";
                            // v5: risk stats
                            const RiskResults &rr = risk.GetResults();
                            std::cout << "[Risk]  Pos=" << std::setprecision(4)
                                      << rr.positionSize
                                      << " PnL=$" << std::setprecision(4) << rr.totalPnl
                                      << " Sharpe=" << std::setprecision(3) << rr.sharpePerTrade
                                      << " MaxDD=$" << std::setprecision(4) << rr.maxDrawdown
                                      << " DV01=$" << std::setprecision(5) << rr.dv01
                                      << " HitRate=" << std::setprecision(1)
                                      << rr.hitRate * 100.0 << "%"
                                      << " Composite=" << std::setprecision(4)
                                      << rr.compositeScore
                                      << " KalmanVel=" << std::setprecision(6)
                                      << rr.kalmanVelocity
                                      << "\n";
                            // v6: book dynamics stats
                            const BookDynamicsResults &bdr = bookDyn.GetResults();
                            std::cout << "[BookDyn] WallVel(b/a)="
                                      << std::setprecision(3) << bdr.bidWallVelocity
                                      << "/" << std::setprecision(3) << bdr.askWallVelocity
                                      << " Gradient(b/a)="
                                      << std::setprecision(1) << bdr.bidGradientMean
                                      << "/" << std::setprecision(1) << bdr.askGradientMean
                                      << " Phantom=" << std::setprecision(2)
                                      << bdr.phantomRatio * 100.0 << "%"
                                      << " HiddenDetects=" << bdr.hiddenDetectCount
                                      << " AvgLife(b/a)="
                                      << std::setprecision(0) << bdr.avgBidLifetimeMs
                                      << "/" << std::setprecision(0) << bdr.avgAskLifetimeMs
                                      << "ms"
                                      << (bdr.compressionAlert ? " [COMPRESS!]" : "")
                                      << (bdr.thinBookAlert    ? " [THIN!]"     : "")
                                      << (bdr.hiddenAlert      ? " [HIDDEN!]"   : "")
                                      << "\n";
                            // v7: regime stats
                            const RegimeResults &rge = regime.GetResults();
                            static const char* vls[] = {"LOW","NORM","HIGH","XTRM"};
                            static const char* hrs[] = {"MR","RW","TREND"};
                            std::cout << "[Regime] Vol="
                                      << std::setprecision(1) << rge.realizedVolAnnualized
                                      << "%" << vls[std::min(3,rge.volRegime)]
                                      << " H=" << std::setprecision(3) << rge.hurstExponent
                                      << "(" << hrs[rge.hurstRegime+1] << ")"
                                      << " ACF=" << std::setprecision(3) << rge.autocorrLag1
                                      << " HMM=" << (rge.hmmState==0?"BULL":"BEAR")
                                      << " RegAdj=" << std::setprecision(3)
                                      << rge.regimeAdjustedScore
                                      << " Edge=" << std::setprecision(2)
                                      << rge.edgeScore << "bps"
                                      << (rge.flashCrashPrecursor ? " [FLASH!]" : "")
                                      << (rge.spreadWideningAlert ? " [WIDE!]"  : "")
                                      << "\n";
                            // v8: strategy stats
                            const StrategyResults &sr8 = strat.GetResults();
                            std::cout << "[Strat]  MMnet=$"
                                      << std::setprecision(4) << sr8.mmNetPnl
                                      << " SpCap=$" << std::setprecision(4) << sr8.mmSpreadCapture
                                      << " AdvSel=$" << std::setprecision(4) << sr8.mmAdvSelection
                                      << " WR=" << std::setprecision(1) << sr8.mmWinRate*100.0 << "%"
                                      << " Inv=" << std::setprecision(3) << sr8.mmInventory
                                      << " δ*=$" << std::setprecision(4) << sr8.asOptimalSpreadUSD
                                      << " (" << sr8.asOptimalSpreadBps << "bps)"
                                      << " Kelly=" << std::setprecision(3) << sr8.mmKellyUnit
                                      << " LatShp=" << std::setprecision(2) << sr8.latSharpe
                                      << " ArbBps=" << std::setprecision(2) << sr8.exchArbBps
                                      << (sr8.mmInventoryAlert ? " [INV!]"  : "")
                                      << (sr8.mmQuotingGated   ? " [GATED]" : "")
                                      << (sr8.exchArbAlert     ? " [ARB!]"  : "")
                                      << "\n";
                            // ──────────────────────────────────────────────────
                            // v9: CrossExchangeFeed (standalone multi-exchange)
                            // ──────────────────────────────────────────────────
                            {
                                double cfBid  = crossFeed.GetBid();
                                double cfAsk  = crossFeed.GetAsk();
                                bool   cfConn = crossFeed.IsConnected();
                                const std::string& cfName = crossFeed.GetExchangeName();
                                bool   cfSpot = crossFeed.IsSpot();

                                double cfMid    = (cfBid > 0 && cfAsk > 0) ? (cfBid + cfAsk) / 2.0 : 0.0;
                                double cfSpread = (cfBid > 0 && cfAsk > 0) ? (cfAsk - cfBid)       : 0.0;
                                double cfSpreadBps = (cfMid > 0) ? (cfSpread / cfMid) * 10000.0     : 0.0;

                                // Compute cross-exchange mid drift vs Binance
                                double binMid = 0.0;
                                {
                                    double bb = processor.GetBestBid();
                                    double ba = processor.GetBestAsk();
                                    if (bb > 0 && ba > 0) binMid = (bb + ba) / 2.0;
                                }
                                double midDrift    = (binMid > 0 && cfMid > 0) ? (cfMid - binMid)            : 0.0;
                                double midDriftBps = (binMid > 0)              ? (midDrift / binMid) * 10000.0 : 0.0;

                                std::cout << "[XchgFeed] " << cfName
                                          << (cfSpot ? "(spot)" : "(perp)")
                                          << (cfConn ? " [CONNECTED]" : " [DISCONNECTED]")
                                          << "  bid=" << std::setprecision(4) << cfBid
                                          << "  ask=" << std::setprecision(4) << cfAsk
                                          << "  mid=" << std::setprecision(4) << cfMid
                                          << "  spread=" << std::setprecision(4) << cfSpread
                                          << "(" << std::setprecision(2) << cfSpreadBps << "bps)"
                                          << "\n";
                                std::cout << "           Drift vs Binance: "
                                          << (midDrift >= 0 ? "+" : "")
                                          << std::setprecision(5) << midDrift
                                          << " (" << (midDriftBps >= 0 ? "+" : "")
                                          << std::setprecision(2) << midDriftBps << "bps)"
                                          << (std::abs(midDriftBps) > 3.0 ? "  *** DRIFT! ***" : "")
                                          << "\n";
                            }
                        }
                        lastStatsTime = now;
                    }

                } catch (const std::exception &e) {
                    std::cerr << "Msg processing error: " << e.what() << "\n";
                }
            }

            batch.clear();
        }

    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        curl_global_cleanup();
        WSACleanup();
        return 1;
    }

    curl_global_cleanup();
    WSACleanup();
    return 0;
}

