#pragma once
// ============================================================================
// CrossExchangeFeed.h  —  Multi-Exchange WebSocket Price Feed (Standalone)
//
// Zero dependency on any engine header. Provides real-time L1 bid/ask from
// secondary exchanges for cross-exchange arbitrage.
//
// B6 fix: Bybit/OKX require a heartbeat ping. The blocking ws.read() loop
// cannot check time-based conditions. Fix: launch a dedicated pinger thread
// BEFORE entering the read loop. The pinger sleeps 15s between pings,
// independent of whether any messages arrive from the server.
//
// Supported market types / endpoints:
//   Bybit spot:    wss://stream.bybit.com/v5/public/spot    (orderbook.1.SYM)
//   Bybit linear:  wss://stream.bybit.com/v5/public/linear  (USDT perp)
//   Bybit inverse: wss://stream.bybit.com/v5/public/inverse (coin perp)
//   OKX spot:      wss://ws.okx.com:8443/ws/v5/public       (bbo-tbt)
//   OKX perp:      same host, instId = SOL-USDT-SWAP
//
// Symbol conversion (Binance → exchange-native):
//   Bybit: SOLUSDT → SOLUSDT (unchanged)
//   OKX:   SOLUSDT → SOL-USDT (spot), SOL-USDT-SWAP (perp)
//
// Thread safety: lock-free atomics for bid/ask/connected. One writer thread
// (feed thread), one reader thread (main processing thread).
// ============================================================================

#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <cstring>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>

// ─── Market type ─────────────────────────────────────────────────────────────
enum class ExchMarketType {
    SPOT,       // Bybit spot / OKX spot
    LINEAR,     // Bybit USDT-margined perpetual
    INVERSE,    // Bybit coin-margined perpetual
    OKX_SPOT,   // OKX spot (uses OKX symbol format)
    OKX_PERP    // OKX perpetual (SOL-USDT-SWAP etc.)
};

// ─── Feed parameters ─────────────────────────────────────────────────────────
struct ExchangeFeedParams {
    std::string    exchange = "Bybit";    // "Bybit" or "OKX"
    std::string    symbol   = "SOLUSDT"; // Binance-format symbol
    ExchMarketType mktType  = ExchMarketType::SPOT;
};

// ─── Internal connection spec ─────────────────────────────────────────────────
struct ExchConnSpec {
    std::string host, port, path, subMsg, pingMsg, name;
    bool        isSpot = true;
};

// ─── Build Bybit connection spec ─────────────────────────────────────────────
static ExchConnSpec MakeBybitSpec(const std::string& sym, ExchMarketType mt) {
    ExchConnSpec s;
    s.host    = "stream.bybit.com";
    s.port    = "443";
    s.name    = "Bybit";
    s.pingMsg = "{\"op\":\"ping\"}";
    s.isSpot  = (mt == ExchMarketType::SPOT);

    switch (mt) {
        case ExchMarketType::LINEAR:
            s.path = "/v5/public/linear"; break;
        case ExchMarketType::INVERSE:
            s.path = "/v5/public/inverse"; break;
        default:
            s.path = "/v5/public/spot"; break;
    }
    s.subMsg = "{\"op\":\"subscribe\",\"args\":[\"orderbook.1." + sym + "\"]}";
    return s;
}

// ─── Build OKX connection spec ────────────────────────────────────────────────
static ExchConnSpec MakeOKXSpec(const std::string& binanceSym, ExchMarketType mt) {
    ExchConnSpec s;
    s.host    = "ws.okx.com";
    s.port    = "8443";
    s.path    = "/ws/v5/public";
    s.name    = "OKX";
    s.pingMsg = "ping";
    s.isSpot  = (mt == ExchMarketType::OKX_SPOT);

    // Convert SOLUSDT → SOL-USDT or SOL-USDT-SWAP
    std::string base = binanceSym;
    std::string okxSym;
    if (base.size() > 4 && base.substr(base.size()-4) == "USDT") {
        okxSym = base.substr(0, base.size()-4) + "-USDT";
    } else if (base.size() > 4 && base.substr(base.size()-4) == "USDC") {
        okxSym = base.substr(0, base.size()-4) + "-USDC";
    } else {
        okxSym = base;
    }
    if (mt == ExchMarketType::OKX_PERP) okxSym += "-SWAP";

    s.subMsg = "{\"op\":\"subscribe\",\"args\":"
               "[{\"channel\":\"bbo-tbt\",\"instId\":\"" + okxSym + "\"}]}";
    return s;
}

// ============================================================================
// SecondExchangeFeed
//
// Runs one WebSocket connection in a background thread.
// B6 fix: dedicated pinger thread runs alongside blocking ws.read() loop.
//
// Price parsing:
//   Bybit orderbook.1: {"data":{"b":[["price","qty"]],"a":[["price","qty"]]}}
//   OKX bbo-tbt:       {"data":[{"bidPx":"price","askPx":"price",...}]}
// ============================================================================
class SecondExchangeFeed {
public:
    static constexpr int PING_INTERVAL_S = 15;   // B6: ping every 15s
    static constexpr int RECONNECT_S     = 3;

    SecondExchangeFeed() : bid_(0.0), ask_(0.0), conn_(false), stop_(false), isSpot_(true) {}
    ~SecondExchangeFeed() { Stop(); }

    void Start(const ExchangeFeedParams& p) {
        params_ = p;
        stop_   = false;
        if (p.exchange == "OKX" || p.exchange == "okx") {
            spec_ = MakeOKXSpec(p.symbol, p.mktType);
        } else {
            spec_ = MakeBybitSpec(p.symbol, p.mktType);
        }
        isSpot_ = spec_.isSpot;
        th_ = std::thread([this]{ RunLoop(); });
    }

    void Stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }

    // Lock-free reads
    double GetBid()              const { return bid_.load(std::memory_order_relaxed); }
    double GetAsk()              const { return ask_.load(std::memory_order_relaxed); }
    bool   IsConnected()         const { return conn_.load(std::memory_order_relaxed); }
    bool   IsSpot()              const { return isSpot_; }
    const  std::string& GetExchangeName() const { return spec_.name; }

private:
    void RunLoop() {
        namespace beast = boost::beast;
        namespace ws_ns = beast::websocket;
        namespace net   = boost::asio;
        namespace ssl   = net::ssl;
        using tcp       = net::ip::tcp;

        while (!stop_) {
            try {
                net::io_context ioc;
                ssl::context ctx{ssl::context::tlsv12_client};
                // B6 upgrade: Disable strict verify for Windows compatibility; add SNI
                ctx.set_verify_mode(ssl::verify_none); 
                
                tcp::resolver resolver{ioc};
                ws_ns::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

                auto ep = resolver.resolve(spec_.host, spec_.port);
                net::connect(ws.next_layer().next_layer(), ep.begin(), ep.end());

                // SNI is REQUIRED for Bybit/OKX (Cloudflare)
                if(!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), spec_.host.c_str())) {
                    throw beast::system_error(
                        beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()),
                        "Failed to set SNI hostname");
                }

                ws.next_layer().handshake(ssl::stream_base::client);
                ws.handshake(spec_.host, spec_.path);
                ws.write(net::buffer(spec_.subMsg));
                conn_ = true;

                // B6: launch ping thread BEFORE blocking read loop
                std::atomic<bool> pingStop{false};
                std::string pingMsg = spec_.pingMsg;
                std::thread pinger([&]{
                    while (!pingStop && !stop_) {
                        std::this_thread::sleep_for(std::chrono::seconds(PING_INTERVAL_S));
                        if (!pingStop && !stop_) {
                            try { ws.write(net::buffer(pingMsg)); }
                            catch (...) { break; }
                        }
                    }
                });

                beast::flat_buffer buf;
                while (!stop_) {
                    ws.read(buf);
                    std::string msg = beast::buffers_to_string(buf.data());
                    buf.consume(buf.size());
                    Dispatch(msg);
                }

                pingStop = true;
                if (pinger.joinable()) pinger.join();

            } catch (const std::exception& e) {
                conn_ = false;
                std::cerr << "[XchgFeed] " << spec_.name << " WS Error: " << e.what() << "\n";
                if (!stop_)
                    std::this_thread::sleep_for(std::chrono::seconds(RECONNECT_S));
            } catch (...) {
                conn_ = false;
                std::cerr << "[XchgFeed] " << spec_.name << " WS Unknown Error\n";
                if (!stop_)
                    std::this_thread::sleep_for(std::chrono::seconds(RECONNECT_S));
            }
        }
        conn_ = false;
    }

    void Dispatch(const std::string& m) {
        // Skip pong/heartbeat responses
        if (m.find("\"pong\"") != std::string::npos) return;
        if (m == "pong") return;
        if (spec_.name == "Bybit") ParseBybit(m);
        else                       ParseOKX(m);
    }

    // Bybit: {"data":{"b":[["130.5","10.5"]],"a":[["130.6","8.2"]],...}}
    void ParseBybit(const std::string& m) {
        if (m.find("orderbook") == std::string::npos) return;
        double b = Pull(m, "\"b\":[[\"");
        double a = Pull(m, "\"a\":[[\"");
        if (b > 0.0) bid_.store(b, std::memory_order_relaxed);
        if (a > 0.0) ask_.store(a, std::memory_order_relaxed);
    }

    // OKX: {"data":[{"bidPx":"130.5","bidSz":"...","askPx":"130.6",...}]}
    void ParseOKX(const std::string& m) {
        if (m.find("\"data\"") == std::string::npos) return;
        double b = Pull(m, "\"bidPx\":\"");
        double a = Pull(m, "\"askPx\":\"");
        if (b > 0.0) bid_.store(b, std::memory_order_relaxed);
        if (a > 0.0) ask_.store(a, std::memory_order_relaxed);
    }

    // Zero-allocation string price extractor
    static double Pull(const std::string& m, const char* key) {
        size_t p = m.find(key);
        if (p == std::string::npos) return 0.0;
        p += std::strlen(key);
        size_t e = m.find('"', p);
        if (e == std::string::npos) return 0.0;
        try { return std::stod(m.substr(p, e - p)); } catch (...) { return 0.0; }
    }

    ExchangeFeedParams params_;
    ExchConnSpec       spec_;
    std::atomic<double> bid_, ask_;
    std::atomic<bool>   conn_, stop_, isSpot_;
    std::thread         th_;
};