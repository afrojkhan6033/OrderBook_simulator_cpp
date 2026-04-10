// =============================================================================
// TcpSender.h  —  Async TCP push from C++ engine → Node.js gateway
//
// DROP-IN INTEGRATION for your existing LiveMarketData.cpp main loop.
//
// Usage inside main():
//
//   TcpSender sender("127.0.0.1", 9001);   // must match gateway BINARY_PORT
//   sender.Connect();
//
//   // Inside the processing loop (after ProcessDepthUpdate):
//   auto snap = processor.GetCurrentSnapshotCopy();
//   sender.SendSnapshot(snap, metrics, analytics);
//
// The sender runs its own write thread so it never blocks the market data loop.
// =============================================================================

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <vector>
#include <cstdint>
#include <iostream>
#include <cstring>
#include "BinarySerializer.h"
#include "OrderBook.h"
#include <condition_variable>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  #define INVALID_SOCK INVALID_SOCKET
  #define CLOSE_SOCK(s) closesocket(s)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  #define INVALID_SOCK (-1)
  #define CLOSE_SOCK(s) ::close(s)
#endif


class TcpSender {
public:
    TcpSender(std::string host, uint16_t port, size_t maxQueue = 50000)
        : host_(std::move(host)), port_(port), maxQueue_(maxQueue) {}

    ~TcpSender() { Disconnect(); }

    // Connect (non-blocking on failure — will not crash your market data loop).
    bool Connect() {

    #ifdef _WIN32
        static bool wsaInitialized = false;
        if (!wsaInitialized) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
                std::cerr << "[TcpSender] WSAStartup failed\n";
                return false;
            }
            wsaInitialized = true;
        }
    #endif

        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCK) return false;

        // Disable Nagle's algorithm (critical for low latency)
        int flag = 1;
    #ifdef _WIN32
        ::setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
    #else
        ::setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    #endif

        // Increase send buffer (helps during burst updates)
        int bufsize = 4 * 1024 * 1024;
    #ifdef _WIN32
        ::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (char*)&bufsize, sizeof(bufsize));
    #else
        ::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    #endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port_);

        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            std::cerr << "[TcpSender] Invalid address\n";
            return false;
        }

        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            std::cerr << "[TcpSender] Cannot connect to " << host_ << ":" << port_ << "\n";
            CLOSE_SOCK(sock_);
            sock_ = INVALID_SOCK;
            return false;
        }

        connected_ = true;
        writerThread_ = std::thread(&TcpSender::WriterLoop, this);

        std::cout << "[TcpSender] Connected to " << host_ << ":" << port_ << "\n";
        return true;
    }

    void Disconnect() {
        connected_.store(false);
        cv_.notify_all();
        if (writerThread_.joinable()) writerThread_.join();
        if (sock_ != INVALID_SOCK) { CLOSE_SOCK(sock_); sock_ = INVALID_SOCK; }
    }

    // Send a full snapshot + stats in one call (called from your main loop)
    void SendSnapshot(
        const PriceLevelSnapshot& snap,
        const PerformanceMetrics& metrics,
        const MarketAnalytics&    analytics,
        int depth = 20)
    {
        Enqueue(BinarySerializer::SerializeSnapshot(snap, MSG_SNAPSHOT, depth));
        Enqueue(BinarySerializer::SerializeStats(metrics, analytics));
    }

    // Send a trade execution
    void SendTrade(int32_t price, uint32_t qty, bool isBuy, uint64_t seqNum) {
        Enqueue(BinarySerializer::SerializeTrade(price, qty, isBuy, seqNum));
    }

    // Send microstructure results
    void SendMicrostructure(const MicrostructureResults& micro) {
        Enqueue(BinarySerializer::SerializeMicrostructure(micro));
    }

    // Send signal engine results
    void SendSignals(const SignalResults& sig) {
        Enqueue(BinarySerializer::SerializeSignals(sig));
    }

    // Send risk engine results
    void SendRisk(const RiskResults& risk) {
        Enqueue(BinarySerializer::SerializeRisk(risk));
    }

    // Send book dynamics results
    void SendBookDynamics(const BookDynamicsResults& bd) {
        Enqueue(BinarySerializer::SerializeBookDynamics(bd));
    }

    // Send regime engine results
    void SendRegime(const RegimeResults& reg) {
        Enqueue(BinarySerializer::SerializeRegime(reg));
    }

    // Send strategy engine results
    void SendStrategy(const StrategyResults& strat) {
        Enqueue(BinarySerializer::SerializeStrategy(strat));
    }

private:
    std::string  host_;
    uint16_t     port_;
    socket_t     sock_     = INVALID_SOCK;
    std::atomic<bool> connected_{false};
    size_t       maxQueue_;

    std::deque<std::vector<uint8_t>> queue_;
    std::mutex   mtx_;
    std::condition_variable cv_;
    std::thread  writerThread_;

    void Enqueue(std::vector<uint8_t> buf) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.size() >= maxQueue_) {
            queue_.pop_front();  // drop oldest (ring-buffer behaviour)
        }
        queue_.push_back(std::move(buf));
        cv_.notify_one();
    }

    void WriterLoop() {
        while (connected_) {
            std::vector<uint8_t> buf;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]{ return !queue_.empty() || !connected_; });
                if (!connected_) break;
                buf = std::move(queue_.front());
                queue_.pop_front();
            }

            // Send entire buffer
            const uint8_t* ptr = buf.data();
            size_t remaining   = buf.size();
            while (remaining > 0) {
                int sent = ::send(sock_, (const char*)ptr, (int)remaining, 0);
                if (sent <= 0) {
                    std::cerr << "[TcpSender] Send error — disconnected\n";
                    connected_ = false;
                    return;
                }
                ptr       += sent;
                remaining -= sent;
            }
        }
    }
};

// =============================================================================
// HOW TO INTEGRATE INTO YOUR main() IN LiveMarketData.cpp
// =============================================================================
//
// 1. Add at top of LiveMarketData.cpp:
//      #include "BinarySerializer.h"
//      #include "TcpSender.h"
//
// 2. In main(), before the processing loop:
//      TcpSender sender("127.0.0.1", 9001);
//      sender.Connect();   // non-blocking — won't abort if gateway isn't up yet
//
// 3. Inside the while(true) loop, after PrintOrderbookDisplay (or instead of it):
//
//      if (timeSinceLast.count() >= refreshIntervalMs) {
//          PrintOrderbookDisplay(...);           // keep your terminal view
//          sender.SendSnapshot(snapshot, metrics, analytics);  // ADD THIS
//          lastStatsPrintTime = now;
//      }
//
//      // For individual trades (optional — requires trade detection logic):
//      // sender.SendTrade(tradePrice, tradeQty, isBuy, metrics.sequenceNumber);
//
// =============================================================================