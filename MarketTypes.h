#pragma once
#include <map>
#include <chrono>
#include "OrderBook.h"

struct PriceLevelSnapshot {
    std::map<Price, Quantity> bids;
    std::map<Price, Quantity, std::greater<Price>> asks;
    std::chrono::high_resolution_clock::time_point timestamp;
    uint64_t sequenceNumber{0};
    uint64_t updateId{0};
};

struct PerformanceMetrics {
    std::atomic<uint64_t> messagesReceived{0};
    std::atomic<uint64_t> messagesProcessed{0};
    std::atomic<uint64_t> messagesDropped{0};
    std::atomic<uint64_t> maxProcessingTimeUs{0};
    std::atomic<uint64_t> minProcessingTimeUs{UINT64_MAX};
    std::atomic<uint64_t> totalProcessingTimeUs{0};
    std::atomic<double> lastSpreadBps{0.0};
    std::atomic<double> currentMidPrice{0.0};
    std::atomic<uint64_t> sequenceNumber{0};

    double GetAverageLatencyUs() const {
        uint64_t processed = messagesProcessed.load();
        if (processed == 0) return 0.0;
        return static_cast<double>(totalProcessingTimeUs.load()) / processed;
    }

    void Reset() {
        messagesReceived = 0;
        messagesProcessed = 0;
        messagesDropped = 0;
        maxProcessingTimeUs = 0;
        minProcessingTimeUs = UINT64_MAX;
        totalProcessingTimeUs = 0;
    }
};


struct MarketAnalytics {
    double bestBid{0.0};
    double bestAsk{0.0};
    double midPrice{0.0};
    double spread{0.0};
    double spreadBps{0.0};
    Quantity bidQuantity{0};
    Quantity askQuantity{0};
    double imbalance{0.0};
    double ofi{0.0}; // Order flow imbalance

    void Calculate(const PriceLevelSnapshot &snapshot) {
        if (snapshot.bids.empty() || snapshot.asks.empty()) {
            return;
        }

        // Get best bid and ask
        bestBid = snapshot.bids.begin()->first / 10000.0;
        bestAsk = snapshot.asks.begin()->first / 10000.0;
        midPrice = (bestBid + bestAsk) / 2.0;
        spread = bestAsk - bestBid;
        spreadBps = (spread / midPrice) * 10000.0;

        // Get quantity at best prices
        bidQuantity = snapshot.bids.begin()->second;
        askQuantity = snapshot.asks.begin()->second;

        // Calculate imbalance
        double bidQty = bidQuantity / 10000.0;
        double askQty = askQuantity / 10000.0;
        imbalance = (bidQty - askQty) / (bidQty + askQty + 1e-9);

        // Simple OFI calculation across top 5 levels
        ofi = 0.0;
        int levelCount = 0;
        for (auto it = snapshot.bids.begin(); it != snapshot.bids.end() && levelCount < 5;
             ++it, ++levelCount) {
            ofi += it->second;
        }
        levelCount = 0;
        for (auto it = snapshot.asks.begin(); it != snapshot.asks.end() && levelCount < 5;
             ++it, ++levelCount) {
            ofi -= it->second;
        }
    }
};