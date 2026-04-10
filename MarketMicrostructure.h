#pragma once
// ============================================================================
// MarketMicrostructure.h  —  HFT-Grade Market Microstructure Analytics
//
// Self-contained header. Zero dependencies on MarketTypes.h or OrderBook.h.
// Include this BEFORE MarketTypes.h in every translation unit.
//
// Algorithm              Formula                           Window / Cost
// ──────────────────────────────────────────────────────────────────────────
// 1. VWAP (rolling)      Σ(p·v)/Σv   time-based deque    60s / O(1) amort.
// 2. TWAP (rolling)      Σ(p·Δt)/Σt  time-based deque    60s / O(N) query
// 3. Lee-Ready           quote rule primary, tick rule     O(1) per trade
//                        fallback (RFC: Binance `m` wins)
// 4. Kyle's λ            OLS: Cov(Δmid,Q_s)/Var(Q_s)     200 trades / O(1)
// 5. Amihud ILLIQ        E[|logR|/notional] × 10^6        200 trades / O(1)
// 6. Microprice (WMP)    (Bid×AskQ + Ask×BidQ)/(BidQ+AskQ) O(1) per tick
// 7. Roll Spread         2·√max(0,−Cov(Δp_t,Δp_{t-1}))  100 prices / O(1)
//
// All rolling windows: pre-allocated deque with running-sum eviction.
// Thread-safety: NONE — call only from the single processing thread.
// Price units: raw double in USD (divide encoded int64 by 10000 before use).
// ============================================================================

#include <deque>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <chrono>
#include <vector>

// ─── Trade side ───────────────────────────────────────────────────────────────
enum class TradeSide : int8_t {
    BUY     = +1,   // buyer is aggressor (taker)
    SELL    = -1,   // seller is aggressor (taker)
    UNKNOWN =  0
};

// ─── All microstructure results in one flat struct ────────────────────────────
struct MicrostructureResults {
    // VWAP / TWAP
    double   vwap           = 0.0;   // 60s rolling VWAP (USD)
    double   twap           = 0.0;   // 60s rolling TWAP (USD)

    // Microprice / WMP
    double   microprice     = 0.0;   // Stoikov weighted mid price (USD)

    // Kyle's lambda
    double   kyleLambda     = 0.0;   // USD per unit of signed flow (≈1e-5 to 1e-3)

    // Amihud illiquidity
    double   amihudIlliq    = 0.0;   // ×10^6 scaled; <0.5 = very liquid

    // Roll's effective spread
    double   rollSpread     = 0.0;   // USD, same units as price

    // Signed flow accumulator
    double   signedFlow     = 0.0;   // cumulative net buy (+) / sell (-) volume

    // Last trade info (for display)
    int      lastTradeSide  = 0;     // +1=BUY / -1=SELL / 0=unknown
    double   lastTradePrice = 0.0;
    double   lastTradeQty   = 0.0;
    uint64_t tradeCount     = 0;

    // Mid-price reference at last trade (set externally before OnTrade)
    double   midAtLastTrade = 0.0;
};

// ============================================================================
// 1. VWAPTWAPEngine
//
// VWAP:
//   Uses running sums (sumPV_, sumV_) maintained over a sliding time window.
//   On each OnTrade() call: add new entry, expire entries older than windowUs_.
//   GetVWAP() = sumPV_ / sumV_  — O(1), exact.
//
// TWAP:
//   Stores (price, timestamp) pairs. GetTWAP(nowUs) walks the window once to
//   compute Σ(p_i × Δt_i) / Σ(Δt_i).  O(N) per query, N ≤ window entries.
//   Typical crypto at 100ms tick rate: N ≤ 600 for 60s window.
// ============================================================================
class VWAPTWAPEngine {
public:
    explicit VWAPTWAPEngine(int64_t windowUs = 60'000'000LL)
        : windowUs_(windowUs) {}

    void OnTrade(double price, double qty, int64_t tsUs) {
        // ── VWAP ─────────────────────────────────────────────────────────────
        vwapW_.push_back({price, qty, tsUs});
        sumPV_ += price * qty;
        sumV_  += qty;
        ExpireVWAP(tsUs - windowUs_);

        // ── TWAP ─────────────────────────────────────────────────────────────
        twapW_.push_back({price, tsUs});
        ExpireTWAP(tsUs - windowUs_);
    }

    double GetVWAP() const {
        return sumV_ > 1e-15 ? sumPV_ / sumV_ : 0.0;
    }

    double GetTWAP(int64_t nowUs) const {
        if (twapW_.size() < 2) return twapW_.empty() ? 0.0 : twapW_.front().price;
        double sumPT = 0.0;
        int64_t sumT = 0;
        for (size_t i = 0; i + 1 < twapW_.size(); ++i) {
            int64_t dt  = twapW_[i + 1].tsUs - twapW_[i].tsUs;
            sumPT += twapW_[i].price * static_cast<double>(dt);
            sumT  += dt;
        }
        // Open interval from last sample to now
        int64_t dtOpen = nowUs - twapW_.back().tsUs;
        if (dtOpen > 0) {
            sumPT += twapW_.back().price * static_cast<double>(dtOpen);
            sumT  += dtOpen;
        }
        return sumT > 0 ? sumPT / static_cast<double>(sumT) : twapW_.back().price;
    }

    bool HasData() const { return !vwapW_.empty(); }

private:
    struct VEntry { double price, qty; int64_t tsUs; };
    struct TEntry { double price;      int64_t tsUs; };

    void ExpireVWAP(int64_t cutoff) {
        while (!vwapW_.empty() && vwapW_.front().tsUs < cutoff) {
            sumPV_ -= vwapW_.front().price * vwapW_.front().qty;
            sumV_  -= vwapW_.front().qty;
            vwapW_.pop_front();
        }
        // Guard against floating-point drift
        if (vwapW_.empty()) { sumPV_ = 0.0; sumV_ = 0.0; }
    }

    void ExpireTWAP(int64_t cutoff) {
        while (!twapW_.empty() && twapW_.front().tsUs < cutoff)
            twapW_.pop_front();
    }

    int64_t              windowUs_;
    std::deque<VEntry>   vwapW_;
    std::deque<TEntry>   twapW_;
    double               sumPV_ = 0.0;
    double               sumV_  = 0.0;
};

// ============================================================================
// 2. LeeReadyClassifier
//
// Two-stage algorithm (Roll 1984 / Lee-Ready 1991):
//
// Stage 1 — Quote rule (primary):
//   Trade price > prevailing mid  →  BUY  (buyer lifted the ask)
//   Trade price < prevailing mid  →  SELL (seller hit the bid)
//   Trade price = mid             →  fallback to tick rule
//
// Stage 2 — Tick rule (fallback):
//   Price > last price where price changed  →  BUY  (uptick)
//   Price < last price where price changed  →  SELL (downtick)
//   Price = last price where changed        →  carry previous classification
//
// In practice: Binance provides the `m` (isBuyerMaker) flag which is SUPERIOR
// to Lee-Ready for on-exchange data. Use Binance `m` as primary; call this
// only as validation or when `m` is unavailable.
// ============================================================================
class LeeReadyClassifier {
public:
    // Returns the classified side AND updates internal tick-rule state.
    TradeSide Classify(double tradePrice, double midPrice) {
        TradeSide side = TradeSide::UNKNOWN;

        // Stage 1: quote rule
        if      (tradePrice > midPrice) side = TradeSide::BUY;
        else if (tradePrice < midPrice) side = TradeSide::SELL;
        else {
            // Stage 2: tick rule
            if (lastDiffPrice_ > 0.0) {
                if      (tradePrice > lastDiffPrice_) side = TradeSide::BUY;
                else if (tradePrice < lastDiffPrice_) side = TradeSide::SELL;
                else                                  side = lastSide_;  // continuation
            } else {
                side = lastSide_;  // no history yet
            }
        }

        // Update tick-rule state
        if (lastTradePrice_ > 0.0 && tradePrice != lastTradePrice_)
            lastDiffPrice_ = lastTradePrice_;
        lastTradePrice_ = tradePrice;
        if (side != TradeSide::UNKNOWN) lastSide_ = side;

        return side;
    }

    void Reset() {
        lastTradePrice_ = 0.0;
        lastDiffPrice_  = 0.0;
        lastSide_       = TradeSide::UNKNOWN;
    }

private:
    double    lastTradePrice_ = 0.0;
    double    lastDiffPrice_  = 0.0;   // last price WHERE A CHANGE OCCURRED
    TradeSide lastSide_       = TradeSide::UNKNOWN;
};

// ============================================================================
// 3. KyleLambdaEstimator
//
// Model:  Δmid_t = α + λ · Q_signed_t + ε_t
//
// Rolling OLS over the last N (price_change, signed_qty) pairs:
//
//   λ = Cov(Δmid, Q_s) / Var(Q_s)
//     = [N·Σ(Δmid·Q_s) − ΣΔmid·ΣQ_s] / [N·ΣQ_s² − (ΣQ_s)²]
//
// Where:
//   Δmid     = mid_after_trade − mid_before_trade  (USD)
//   Q_signed = +qty for BUY, −qty for SELL
//
// Running sums (sumDm, sumQ, sumDm2, sumQ2, sumDmQ) allow O(1) amortized
// updates. Oldest entry evicted when window reaches maxWindow_ observations.
//
// Interpretation:
//   λ ≈ 1e-6  →  very liquid; each unit of flow barely moves price
//   λ ≈ 1e-3  →  illiquid; large price impact per unit flow
//   Always positive; sign of signed flow determines direction.
// ============================================================================
class KyleLambdaEstimator {
public:
    explicit KyleLambdaEstimator(int maxWindow = 200) : maxW_(maxWindow) {}

    // dm = new_mid - old_mid (signed, USD)
    // signedQty = +qty for BUY, -qty for SELL
    void Update(double dm, double signedQty) {
        // Evict oldest if full
        if (static_cast<int>(w_.size()) >= maxW_) {
            auto &o = w_.front();
            n_--;
            sumDm_  -= o.dm;  sumQ_   -= o.q;
            sumDm2_ -= o.dm * o.dm;
            sumQ2_  -= o.q  * o.q;
            sumDmQ_ -= o.dm * o.q;
            w_.pop_front();
        }
        w_.push_back({dm, signedQty});
        n_++;
        sumDm_  += dm;           sumQ_   += signedQty;
        sumDm2_ += dm * dm;      sumQ2_  += signedQty * signedQty;
        sumDmQ_ += dm * signedQty;
    }

    double GetLambda() const {
        if (n_ < 10) return 0.0;
        double N    = static_cast<double>(n_);
        double varQ = N * sumQ2_ - sumQ_ * sumQ_;
        if (std::abs(varQ) < 1e-20) return 0.0;
        double covDmQ = N * sumDmQ_ - sumDm_ * sumQ_;
        return covDmQ / varQ;
    }

    int Count() const { return n_; }

private:
    struct Entry { double dm, q; };
    std::deque<Entry> w_;
    int    maxW_;
    int    n_      = 0;
    double sumDm_  = 0.0, sumQ_   = 0.0;
    double sumDm2_ = 0.0, sumQ2_  = 0.0, sumDmQ_ = 0.0;
};

// ============================================================================
// 4. AmihudEstimator
//
//   ILLIQ_t = |ln(P_t / P_{t-1})| / (P_t × Q_t)
//             ────────────────────────────────────
//             |log return| / notional (USD volume)
//
//   ILLIQ   = (1/N) Σ ILLIQ_t          (rolling mean over N trades)
//
// Scaled by 10^6 for display readability.
// Typical values:
//   SOL/USDT spot: ~0.1 to 1.0  (very liquid)
//   Thin altcoin:  >10           (illiquid)
//
// A spike in ILLIQ signals that price is moving a lot per unit of volume —
// early warning of flash crash or thin-book conditions.
// ============================================================================
class AmihudEstimator {
public:
    explicit AmihudEstimator(int maxWindow = 200) : maxW_(maxWindow) {}

    // price in USD, qty in base asset (e.g., SOL)
    void OnTrade(double price, double qty) {
        if (lastPrice_ <= 0.0) { lastPrice_ = price; return; }

        double logRet   = std::log(price / lastPrice_);
        double absRet   = std::abs(logRet);
        double notional = price * qty;                   // USD volume
        double illiq    = notional > 1e-12 ? absRet / notional : 0.0;

        if (static_cast<int>(w_.size()) >= maxW_) {
            sumIlliq_ -= w_.front();
            w_.pop_front();
        }
        w_.push_back(illiq);
        sumIlliq_ += illiq;
        lastPrice_  = price;
    }

    // Returns mean ILLIQ × 10^6
    double GetIlliquidity() const {
        if (w_.empty()) return 0.0;
        return (sumIlliq_ / static_cast<double>(w_.size())) * 1e6;
    }

    int Count() const { return static_cast<int>(w_.size()); }

private:
    std::deque<double> w_;
    int    maxW_;
    double lastPrice_  = 0.0;
    double sumIlliq_   = 0.0;
};

// ============================================================================
// 5. ComputeMicroprice (Stoikov WMP — no state)
//
//   WMP = (BestBid × AskQty + BestAsk × BidQty) / (BidQty + AskQty)
//
// Intuition: if BidQty >> AskQty, more demand than supply, so fair value
// tilts toward the ask. This is a better instantaneous fair-value estimate
// than the plain mid during periods of one-sided pressure.
//
// Called on every top-of-book change. O(1), no memory.
// ============================================================================
inline double ComputeMicroprice(double bestBid, double bestAsk,
                                double bidQty,  double askQty) {
    double total = bidQty + askQty;
    if (total < 1e-15) return (bestBid + bestAsk) * 0.5;
    return (bestBid * askQty + bestAsk * bidQty) / total;
}

// ============================================================================
// 6. RollSpreadEstimator
//
// Based on Roll (1984):
//   Trade price alternates between bid and ask: P_t = V_t + c·I_t
//   where V_t = fundamental value, c = half-spread, I_t ∈ {+1,−1}
//
//   This implies: Cov(ΔP_t, ΔP_{t-1}) = −c²
//   Therefore:    s_eff = 2√max(0, −Cov(ΔP_t, ΔP_{t-1}))
//
// Rolling serial covariance via lag-1 products:
//   cov ≈ (1/N) Σ ΔP_t · ΔP_{t-1}
//
// Stores consecutive price-change pairs in a deque.
// When serial covariance is positive (strong momentum), model breaks down
// and Roll's estimator returns 0 (undefined).
//
// Input: can be trade prices OR mid prices — mid gives smoother estimate.
// Both are fed in: OnPrice() is called from both OnTrade() and OnBookUpdate().
// ============================================================================
class RollSpreadEstimator {
public:
    explicit RollSpreadEstimator(int maxWindow = 100) : maxW_(maxWindow) {}

    void OnPrice(double price) {
        if (lastPrice_ == 0.0) { lastPrice_ = price; return; }
        double dp = price - lastPrice_;
        lastPrice_ = price;

        dpW_.push_back(dp);

        if (dpW_.size() >= 2) {
            // Lag-1 product: dp[t] × dp[t-1]
            double lp = dpW_[dpW_.size()-1] * dpW_[dpW_.size()-2];
            lagW_.push_back(lp);
            sumLag_ += lp;
            n_++;

            // Expire oldest when over window (evict oldest dp AND its lag product)
            if (static_cast<int>(dpW_.size()) > maxW_) {
                // The lag product of the oldest dp pair is lagW_.front()
                sumLag_ -= lagW_.front();
                lagW_.pop_front();
                dpW_.pop_front();
                n_--;
            }
        }
    }

    // Returns effective spread in same price units as input
    double GetRollSpread() const {
        if (n_ < 5) return 0.0;
        double cov = sumLag_ / static_cast<double>(n_);
        return 2.0 * std::sqrt(std::max(0.0, -cov));
    }

    int Count() const { return n_; }

private:
    std::deque<double> dpW_;    // recent price changes
    std::deque<double> lagW_;   // lag-1 products
    int    maxW_;
    int    n_        = 0;
    double lastPrice_ = 0.0;
    double sumLag_    = 0.0;
};

// ============================================================================
// MicrostructureEngine — orchestrates all 6 estimators
//
// Usage:
//   MicrostructureEngine micro;
//
//   // On each aggTrade tick (before calling, record current mid):
//   double prevMid = currentMid;
//   // process depth update...
//   double newMid = updatedMid;
//   micro.OnTrade(tradePrice, tradeQty, side, tradeTimeUs, prevMid, newMid);
//
//   // On each top-of-book update (after depth applied):
//   micro.OnBookUpdate(bestBid, bestAsk, bidQty, askQty);
//
//   // Read:
//   const MicrostructureResults& r = micro.GetResults();
// ============================================================================
class MicrostructureEngine {
public:
    MicrostructureEngine()
        : vwapTwap_(60'000'000LL)   // 60s window
        , kyleLambda_(200)           // 200 trades
        , amihud_(200)               // 200 trades
        , rollSpread_(100)           // 100 price observations
    {}

    // ── Called on each confirmed trade ────────────────────────────────────────
    // binanceSide: derived from Binance `m` flag:
    //   m=false → buyer is taker → BUY aggressor
    //   m=true  → seller is taker → SELL aggressor
    // midBefore: mid price BEFORE this trade is processed into the book
    // midAfter:  mid price AFTER the book is updated (for Kyle's Δmid)
    //   If depth update has not yet been applied, midBefore == midAfter (use one value)
    void OnTrade(double price, double qty, TradeSide binanceSide,
                 int64_t tradeTimeUs, double midBefore, double midAfter) {

        // ── 1. VWAP / TWAP ────────────────────────────────────────────────────
        vwapTwap_.OnTrade(price, qty, tradeTimeUs);

        // ── 2. Lee-Ready (cross-validation vs Binance flag) ───────────────────
        TradeSide lrSide = leeReady_.Classify(price, midBefore);
        // Binance `m` flag is more accurate for on-exchange data; use it as primary
        TradeSide effectiveSide = (binanceSide != TradeSide::UNKNOWN) ? binanceSide : lrSide;
        double signedQty = qty * static_cast<double>(static_cast<int>(effectiveSide));

        // ── 3. Kyle's λ: feed Δmid and signed qty ─────────────────────────────
        // Δmid = mid after the trade vs mid before (captures price impact)
        double dm = midAfter - midBefore;
        kyleLambda_.Update(dm, signedQty);

        // ── 4. Amihud ILLIQ ───────────────────────────────────────────────────
        amihud_.OnTrade(price, qty);

        // ── 5. Roll Spread: feed trade price ─────────────────────────────────
        rollSpread_.OnPrice(price);

        // ── 6. Signed flow accumulator ────────────────────────────────────────
        results_.signedFlow += signedQty;

        // ── Update result fields ──────────────────────────────────────────────
        int64_t nowUs = CurrentUsEpoch();
        results_.vwap           = vwapTwap_.GetVWAP();
        results_.twap           = vwapTwap_.GetTWAP(nowUs);
        results_.kyleLambda     = kyleLambda_.GetLambda();
        results_.amihudIlliq    = amihud_.GetIlliquidity();
        results_.rollSpread     = rollSpread_.GetRollSpread();
        results_.lastTradeSide  = static_cast<int>(effectiveSide);
        results_.lastTradePrice = price;
        results_.lastTradeQty   = qty;
        results_.midAtLastTrade = midBefore;
        results_.tradeCount++;
    }

    // ── Called on each top-of-book change ─────────────────────────────────────
    // Provides additional Roll spread data points and refreshes microprice.
    void OnBookUpdate(double bestBid, double bestAsk,
                      double bidQty,  double askQty) {
        results_.microprice = ComputeMicroprice(bestBid, bestAsk, bidQty, askQty);
        // Feed mid into Roll's estimator (more data points = lower variance estimate)
        rollSpread_.OnPrice((bestBid + bestAsk) * 0.5);
        // Refresh TWAP timestamp
        if (vwapTwap_.HasData()) {
            results_.twap = vwapTwap_.GetTWAP(CurrentUsEpoch());
        }
        // Refresh Roll spread
        results_.rollSpread = rollSpread_.GetRollSpread();
    }

    const MicrostructureResults& GetResults() const { return results_; }
    MicrostructureResults&       GetResults()       { return results_; }

    // Expose Lee-Ready for external validation / display
    TradeSide ClassifyLeeReady(double price, double mid) {
        return leeReady_.Classify(price, mid);
    }

    static int64_t CurrentUsEpoch() {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    }

private:
    VWAPTWAPEngine      vwapTwap_;
    LeeReadyClassifier  leeReady_;
    KyleLambdaEstimator kyleLambda_;
    AmihudEstimator     amihud_;
    RollSpreadEstimator rollSpread_;
    MicrostructureResults results_;
};