#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include <iostream>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
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
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <memory>
#include "OrderBook.h"
#include "BinarySerializer.h"
#include "TcpSender.h"
#include "MarketTypes.h"
#include <utility>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
// ============================================================================
// HFT-Level WebSocket Configuration and Performance Monitoring
// ============================================================================

struct HFTConfig {
    static constexpr int ORDERBOOK_DEPTH = 50;           // Maintain top N levels
    static constexpr int MAX_MESSAGE_QUEUE = 100000;     // Ring buffer size
    static constexpr int STATS_PRINT_INTERVAL_MS = 5000; // Print stats every 5 seconds
    static constexpr double PROCESSING_THRESHOLD_US = 1000.0; // Alert if message takes >1ms
};

// struct PerformanceMetrics {
//     std::atomic<uint64_t> messagesReceived{0};
//     std::atomic<uint64_t> messagesProcessed{0};
//     std::atomic<uint64_t> messagesDropped{0};
//     std::atomic<uint64_t> maxProcessingTimeUs{0};
//     std::atomic<uint64_t> minProcessingTimeUs{UINT64_MAX};
//     std::atomic<uint64_t> totalProcessingTimeUs{0};
//     std::atomic<double> lastSpreadBps{0.0};
//     std::atomic<double> currentMidPrice{0.0};
//     std::atomic<uint64_t> sequenceNumber{0};

//     double GetAverageLatencyUs() const {
//         uint64_t processed = messagesProcessed.load();
//         if (processed == 0) return 0.0;
//         return static_cast<double>(totalProcessingTimeUs.load()) / processed;
//     }

//     void Reset() {
//         messagesReceived = 0;
//         messagesProcessed = 0;
//         messagesDropped = 0;
//         maxProcessingTimeUs = 0;
//         minProcessingTimeUs = UINT64_MAX;
//         totalProcessingTimeUs = 0;
//     }
// };

// ============================================================================
// OrderBook Level Storage with Maps (HFT-Optimized)
// ============================================================================

// struct PriceLevelSnapshot {
//     std::map<Price, Quantity> bids;
//     std::map<Price, Quantity, std::greater<Price>> asks;
//     std::chrono::high_resolution_clock::time_point timestamp;
//     uint64_t sequenceNumber{0};
//     uint64_t updateId{0};
// };

// ============================================================================
// Message Queue for High-Frequency Processing
// ============================================================================

class MessageQueue {
public:
    struct Message {
        std::string data;
        std::chrono::high_resolution_clock::time_point receivedTime;
    };

    std::deque<Message> queue_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    const size_t maxSize_;

public:
    explicit MessageQueue(size_t maxSize = HFTConfig::MAX_MESSAGE_QUEUE)
        : maxSize_{maxSize} {}

    bool Push(const std::string &data) {
        auto now = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);

        if (queue_.size() >= maxSize_) {
            return false; // Queue full, message dropped
        }

        queue_.push_back({data, now});
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    bool Pop(Message &message) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            notEmpty_.wait(lock, [this] { return !queue_.empty(); });
        }

        if (queue_.empty()) return false;

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

    size_t Size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void Clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.clear();
    }
};
    
// ============================================================================
// WebSocket Streaming Handler
// ============================================================================

class BinanceWebSocketFeed {
private:
    std::string symbol_;
    std::string marketType_;
    MessageQueue messageQueue_;
    PerformanceMetrics metrics_;

    std::atomic<bool> isConnected_{false};
    std::atomic<bool> shouldStop_{false};
    std::thread wsThread_;

public:
    BinanceWebSocketFeed(const std::string& symbol, const std::string& marketType)
        : symbol_(symbol), marketType_(marketType) {}

    ~BinanceWebSocketFeed() {
        Stop();
    }

    void Start() {
        shouldStop_ = false;

        wsThread_ = std::thread([this]() {

            try {
                std::string host;
                std::string port = "9443";
                std::string target;

                std::string streamName = symbol_;
                std::transform(streamName.begin(), streamName.end(),
                               streamName.begin(), ::tolower);

                if (marketType_ == "spot") {
                    host = "stream.binance.com";
                    target = "/ws/" + streamName + "@depth@100ms";
                } else if (marketType_ == "futures") {
                    host = "fstream.binance.com";
                    target = "/ws/" + streamName + "@depth@100ms";
                } else {
                    host = "dstream.binance.com";
                    target = "/ws/" + streamName + "@depth@100ms";
                }

                net::io_context ioc;
                ssl::context ctx{ssl::context::tlsv12_client};
                ctx.set_default_verify_paths();

                tcp::resolver resolver{ioc};
                websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

                auto const results = resolver.resolve(host, port);
                net::connect(ws.next_layer().next_layer(),
                             results.begin(), results.end());

                ws.next_layer().handshake(ssl::stream_base::client);
                ws.handshake(host, target);

                isConnected_ = true;
                std::cout << "Connected to " << host << std::endl;

                beast::flat_buffer buffer;

                while (!shouldStop_) {

                    ws.read(buffer);

                    std::string message =
                        beast::buffers_to_string(buffer.data());

                    buffer.consume(buffer.size());

                    metrics_.messagesReceived++;

                    if (!messageQueue_.Push(message)) {
                        metrics_.messagesDropped++;
                    }
                }

            } catch (const std::exception& e) {
                std::cerr << "WebSocket error: "
                          << e.what() << std::endl;
                isConnected_ = false;
            }
        });
    }

    void Stop() {
        shouldStop_ = true;
        if (wsThread_.joinable())
            wsThread_.join();
    }

    bool IsConnected() const { return isConnected_.load(); }

    MessageQueue& GetMessageQueue() { return messageQueue_; }

    PerformanceMetrics& GetMutableMetrics() { return metrics_; }
};

// ============================================================================
// Efficient OrderBook Snapshot Processor
// ============================================================================

class OrderBookProcessor {
private:
    Orderbook orderbook_;
    PriceLevelSnapshot currentSnapshot_;
    PriceLevelSnapshot previousSnapshot_;
    mutable std::mutex snapshotMutex_;

public:
    OrderBookProcessor() = default;

    bool ProcessDepthUpdate(const nlohmann::json &json, PerformanceMetrics &metrics) {
        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            // Parse message type
            bool isFinalFrame = json.contains("E"); // Event time
            if (!isFinalFrame) {
                return false;
            }

            // Extract sequence numbers
            uint64_t u = json["u"].get<uint64_t>(); // Final update ID
            uint64_t pu = json.contains("pu") ? json["pu"].get<uint64_t>() : 0;

            {
                std::unique_lock<std::mutex> lock(snapshotMutex_);

                // Check for gaps in sequence
                if (previousSnapshot_.sequenceNumber > 0 &&
                    pu != previousSnapshot_.sequenceNumber) {
                    std::cerr << "Sequence gap detected! Previous: "
                              << previousSnapshot_.sequenceNumber << ", Current: " << pu << "\n";
                }

                // Update bid prices (highest to lowest)
        
                for (const auto &bid : json["b"]) {
                    Price price = static_cast<Price>(std::stold(bid[0].get<std::string>()) * 10000);
                    Quantity quantity =
                        static_cast<Quantity>(std::stold(bid[1].get<std::string>()) * 10000);

                    if (quantity == 0)
                        currentSnapshot_.bids.erase(price);
                    else
                        currentSnapshot_.bids[price] = quantity;
                }

                // Update ask prices (lowest to highest)
                
                for (const auto &ask : json["a"]) {
                    Price price = static_cast<Price>(std::stold(ask[0].get<std::string>()) * 10000);
                    Quantity quantity =
                        static_cast<Quantity>(std::stold(ask[1].get<std::string>()) * 10000);

                    if (quantity == 0)
                        currentSnapshot_.asks.erase(price);
                    else
                        currentSnapshot_.asks[price] = quantity;
                }

                currentSnapshot_.timestamp = std::chrono::high_resolution_clock::now();
                currentSnapshot_.sequenceNumber = u;
                currentSnapshot_.updateId++;

                previousSnapshot_ = currentSnapshot_;
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            auto latency =
                std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

            metrics.totalProcessingTimeUs += latency.count();
            metrics.maxProcessingTimeUs =
                std::max(metrics.maxProcessingTimeUs.load(), (uint64_t)latency.count());
            metrics.minProcessingTimeUs =
                std::min(metrics.minProcessingTimeUs.load(), (uint64_t)latency.count());
            metrics.messagesProcessed++;

            return true;

        } catch (const std::exception &e) {
            std::cerr << "Error processing depth update: " << e.what() << "\n";
            return false;
        }
    }

    const PriceLevelSnapshot &GetCurrentSnapshot() const {
        std::unique_lock<std::mutex> lock(snapshotMutex_);
        return currentSnapshot_;
    }

    PriceLevelSnapshot GetCurrentSnapshotCopy() const {
        std::unique_lock<std::mutex> lock(snapshotMutex_);
        return currentSnapshot_;
    }

    const Orderbook &GetOrderBook() const { return orderbook_; }

    size_t GetBidCount() const {
        std::unique_lock<std::mutex> lock(snapshotMutex_);
        return currentSnapshot_.bids.size();
    }

    size_t GetAskCount() const {
        std::unique_lock<std::mutex> lock(snapshotMutex_);
        return currentSnapshot_.asks.size();
    }
};

// ============================================================================
// Display and Analytics
// ============================================================================

// struct MarketAnalytics {
//     double bestBid{0.0};
//     double bestAsk{0.0};
//     double midPrice{0.0};
//     double spread{0.0};
//     double spreadBps{0.0};
//     Quantity bidQuantity{0};
//     Quantity askQuantity{0};
//     double imbalance{0.0};
//     double ofi{0.0}; // Order flow imbalance

//     void Calculate(const PriceLevelSnapshot &snapshot) {
//         if (snapshot.bids.empty() || snapshot.asks.empty()) {
//             return;
//         }

//         // Get best bid and ask
//         bestBid = snapshot.bids.begin()->first / 10000.0;
//         bestAsk = snapshot.asks.begin()->first / 10000.0;
//         midPrice = (bestBid + bestAsk) / 2.0;
//         spread = bestAsk - bestBid;
//         spreadBps = (spread / midPrice) * 10000.0;

//         // Get quantity at best prices
//         bidQuantity = snapshot.bids.begin()->second;
//         askQuantity = snapshot.asks.begin()->second;

//         // Calculate imbalance
//         double bidQty = bidQuantity / 10000.0;
//         double askQty = askQuantity / 10000.0;
//         imbalance = (bidQty - askQty) / (bidQty + askQty + 1e-9);

//         // Simple OFI calculation across top 5 levels
//         ofi = 0.0;
//         int levelCount = 0;
//         for (auto it = snapshot.bids.begin(); it != snapshot.bids.end() && levelCount < 5;
//              ++it, ++levelCount) {
//             ofi += it->second;
//         }
//         levelCount = 0;
//         for (auto it = snapshot.asks.begin(); it != snapshot.asks.end() && levelCount < 5;
//              ++it, ++levelCount) {
//             ofi -= it->second;
//         }
//     }
// };

void PrintOrderbookDisplay(const PriceLevelSnapshot &snapshot, const std::string &symbol,
                           const MarketAnalytics &analytics, const PerformanceMetrics &metrics,
                           int displayLevels = 20) {
    std::cout << "\033[2J\033[1;1H"; // Clear screen

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%H:%M:%S");

    std::cout << "═══════════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  📊 LIVE ORDERBOOK: " << symbol << " │ " << ss.str() << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════════════\n\n";

    // Bidirectional orderbook display
    std::cout << std::setw(20) << "BID QTY" << " │ " << std::setw(15) << "BID PRICE" << " │ "
              << std::setw(15) << "ASK PRICE" << " │ " << std::setw(20) << "ASK QTY"
              << "\n";
    std::cout << std::string(80, '─') << "\n";

    // Convert to vectors for easier iteration
    std::vector<std::pair<Price, Quantity>> bidVec(snapshot.bids.begin(), snapshot.bids.end());
    std::vector<std::pair<Price, Quantity>> askVec(snapshot.asks.begin(), snapshot.asks.end());

    int maxDisplay = std::min({displayLevels, (int)bidVec.size(), (int)askVec.size()});

    for (int i = 0; i < maxDisplay; ++i) {
        // Bid side
        if (i < bidVec.size()) {
            std::cout << std::setw(20) << std::fixed << std::setprecision(2)
                      << bidVec[i].second / 10000.0 << " │ " << std::setw(15) << std::fixed
                      << std::setprecision(4) << bidVec[i].first / 10000.0 << " │ ";
        } else {
            std::cout << std::setw(20) << "─" << " │ " << std::setw(15) << "─" << " │ ";
        }

        // Ask side
        if (i < askVec.size()) {
            std::cout << std::setw(15) << std::fixed << std::setprecision(4)
                      << askVec[i].first / 10000.0 << " │ " << std::setw(20) << std::fixed
                      << std::setprecision(2) << askVec[i].second / 10000.0 << "\n";
        } else {
            std::cout << std::setw(15) << "─" << " │ " << std::setw(20) << "─" << "\n";
        }
    }

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "📈 MARKET ANALYTICS\n";
    std::cout << "───────────────────────────────────────────────────────────────────────────────────\n";
    std::cout << "Best Bid: " << std::fixed << std::setprecision(4) << analytics.bestBid
              << " │ Best Ask: " << analytics.bestAsk << "\n";
    std::cout << "Mid Price: " << analytics.midPrice << " │ Spread: " << analytics.spread
              << " (" << analytics.spreadBps << " bps)\n";
    std::cout << "Bid Qty: " << std::fixed << std::setprecision(2) << analytics.bidQuantity / 10000.0
              << " │ Ask Qty: " << analytics.askQuantity / 10000.0 << "\n";
    std::cout << "Imbalance: " << std::fixed << std::setprecision(4) << (analytics.imbalance * 100)
              << "% │ OFI: " << std::fixed << std::setprecision(0) << analytics.ofi / 10000.0
              << "\n";

    std::cout << "\n";
    std::cout << "⚡ PERFORMANCE METRICS\n";
    std::cout << "───────────────────────────────────────────────────────────────────────────────────\n";
    std::cout << "Messages Received: " << metrics.messagesReceived.load()
              << " │ Processed: " << metrics.messagesProcessed.load()
              << " │ Dropped: " << metrics.messagesDropped.load() << "\n";
    std::cout << "Avg Latency: " << std::fixed << std::setprecision(3) << metrics.GetAverageLatencyUs()
              << " μs │ Max: " << metrics.maxProcessingTimeUs.load()
              << " μs │ Min: " << metrics.minProcessingTimeUs.load() << " μs\n";
    std::cout << "Bid Levels: " << snapshot.bids.size() << " │ Ask Levels: " << snapshot.asks.size()
              << " │ Seq: " << metrics.sequenceNumber.load() << "\n";

    std::cout << "═══════════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "Press Ctrl+C to exit...\n";
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char *argv[]) {
    
    // Parse command-line arguments
    std::string symbol = "SOLUSDT";
    std::string marketType = "spot";
    int displayLevels = 20;
    int refreshIntervalMs = 1000;

    if (argc > 1) symbol = argv[1];
    if (argc > 2) marketType = argv[2];
    if (argc > 3) displayLevels = std::stoi(argv[3]);
    if (argc > 4) refreshIntervalMs = std::stoi(argv[4]);

    // Normalize inputs
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
    std::transform(marketType.begin(), marketType.end(), marketType.begin(), ::tolower);

    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           BINANCE LIVE ORDERBOOK - HFT WEBSOCKET FEED                        ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════════════╝\n\n";

    std::cout << "Configuration:\n";
    std::cout << "  Symbol: " << symbol << "\n";
    std::cout << "  Market Type: " << marketType << " (spot | futures | coin)\n";
    std::cout << "  Display Levels: " << displayLevels << "\n";
    std::cout << "  Refresh Interval: " << refreshIntervalMs << " ms\n";
    std::cout << "  Orderbook Depth: " << HFTConfig::ORDERBOOK_DEPTH << " levels\n";
    std::cout << "\nUsage:\n";
    std::cout << "  ./LiveMarketData [SYMBOL] [MARKET_TYPE] [LEVELS] [REFRESH_MS]\n";
    std::cout << "Examples:\n";
    std::cout << "  ./LiveMarketData BTCUSDT spot 20 1000\n";
    std::cout << "  ./LiveMarketData ETHUSDT futures 50 500\n";
    std::cout << "  ./LiveMarketData BTCUSD_PERP coin 30 100\n\n";

    std::cout << "Connecting to Binance WebSocket...\n\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    try {
        // Initialize components
        BinanceWebSocketFeed wsFeeds(symbol, marketType);
        OrderBookProcessor processor;
        PerformanceMetrics &metrics = wsFeeds.GetMutableMetrics();
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            std::cerr << "Winsock initialization failed\n";
            return 1;
        }
        TcpSender sender("127.0.0.1", 9001);
        if (!sender.Connect()) {
            std::cerr << "Failed to connect to gateway\n";
            return 1;
        }

        // Start WebSocket connection
        wsFeeds.Start();

        // Wait for initial connection
        int waitCount = 0;
        while (!wsFeeds.IsConnected() && waitCount < 30) {
            std::cout << "Waiting for WebSocket connection... (" << (waitCount + 1) << "/30)\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            waitCount++;
        }

        if (!wsFeeds.IsConnected()) {
            std::cerr << "Failed to establish WebSocket connection\n";
            
            return 1;
        }

        std::cout << "✅ WebSocket connected! Processing live data...\n\n";

        // Main processing loop
        auto lastStatsPrintTime = std::chrono::steady_clock::now();
        MessageQueue::Message message;

        while (true){
            // BLOCK until message available
            wsFeeds.GetMessageQueue().Pop(message);

            try {
                auto jsonData = nlohmann::json::parse(message.data);

                if (jsonData.contains("b") && jsonData.contains("a") && processor.ProcessDepthUpdate(jsonData, metrics)) {

                    metrics.sequenceNumber = jsonData["u"].get<uint64_t>();

                    PriceLevelSnapshot snapshot = processor.GetCurrentSnapshotCopy();

                    MarketAnalytics analytics;
                    analytics.Calculate(snapshot);

                    metrics.lastSpreadBps = analytics.spreadBps;
                    metrics.currentMidPrice = analytics.midPrice;

                    auto now = std::chrono::steady_clock::now();
                    auto timeSinceLast =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - lastStatsPrintTime);

                    if (timeSinceLast.count() >= refreshIntervalMs) {
                        PrintOrderbookDisplay(snapshot, symbol, analytics, metrics,displayLevels);
                        sender.SendSnapshot(snapshot, metrics, analytics);
                        lastStatsPrintTime = now;
                    }
                }

            } catch (const std::exception &e) {
                std::cerr << "Processing error: " << e.what() << "\n";
            }
        }

    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        
        return 1;
    }

    WSACleanup();
    return 0;
}

