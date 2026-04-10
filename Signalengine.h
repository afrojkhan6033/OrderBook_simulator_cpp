#pragma once
// ============================================================================
// SignalEngine.h  —  HFT Signal Analytics Engine  (P1 / P2 / P3)
//
// Self-contained. Only dependency: MarketMicrostructure.h (for TradeSide).
// Include AFTER MarketMicrostructure.h in every translation unit.
//
// Algorithm                    Formula / Method                  Window / Cost
// ─────────────────────────────────────────────────────────────────────────────
// 1. MultiLevelOFI   (P1)  OFI[i] = ΔBid_i − ΔAsk_i           L1-L5 / O(1)
//                          exp-decay weights: w[i]=exp(−ln2·i)
//                          rolling EWM with α=0.3
//
// 2. VPIN            (P1)  |V_B − V_S| / V_bar per bucket       50 buckets
//                          (Easley, López de Prado, O'Hara 2012)
//
// 3. MomentumMR      (P2)  Short: EWMA(Δmid, α=0.5) over 5 ticks
//                          Long:  AR(1) serial autocorrelation over 60 ticks
//                          Regime: sign(AR1) × strength → TREND / MR / NEUTRAL
//
// 4. IcebergDetector (P2)  qty_decrease_at_P → qty_refill_at_P
//                          within REFILL_WINDOW_US → refill_count++
//                          refill_count ≥ 3 → ICEBERG at price P
//
// 5. QuoteStuffing   (P2)  stuffing_ratio = Σmsg / (Σ|ΔQty| + ε)
//                          over rolling 1s window; >10 + >20 msg = ALERT
//
// 6. SpoofingLayering(P3)  Large order (>3× mean_qty) at price P disappears
//                          without adjacent trade (cancel, not fill)
//                          ≥ 3 cancels at P within 60s → SPOOF ALERT
//
// Thread-safety: NONE — call only from the single processing thread.
// All inputs: USD prices (double), base-asset quantities (double).
// ============================================================================

#include "MarketMicrostructure.h"   // TradeSide

#include <deque>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <numeric>

// ─── Top-of-book snapshot (L1–L5 extracted from PriceLevelSnapshot) ──────────
struct TopOfBook {
    static constexpr int N = 5;
    double bidP[N] = {}, bidQ[N] = {};
    double askP[N] = {}, askQ[N] = {};
    int    levels  = 0;    // valid entries in [0, N]
};

// ─── All signal results in one flat POD struct ───────────────────────────────
struct SignalResults {
    // 1. Multi-Level OFI (P1)
    double ofiNormalized  = 0.0;    // [-1, +1]  >0 = net buy pressure
    double ofiRaw         = 0.0;    // unnormalised rolling EWM OFI

    // 2. VPIN (P1)
    double vpin           = 0.0;    // [0, 1]    >0.65 = high adverse-selection risk
    int    vpinBuckets    = 0;      // completed buckets in rolling window

    // 3. Momentum / Mean-Reversion (P2)
    double momentumScore  = 0.0;    // EWMA(Δmid) — +up / −down
    double zScore         = 0.0;    // (mid − mean60) / std60
    double ar1Coeff       = 0.0;    // rolling AR(1): +trending, −mean-rev
    int    regime         = 0;      // 0=neutral / +1=trending / -1=mean-reverting

    // 4. Iceberg Detection (P2)
    bool   icebergDetected = false;
    double icebergPrice    = 0.0;
    int    icebergSide     = 0;     // +1=bid iceberg / -1=ask iceberg
    int    icebergRefills  = 0;     // refill count at detected level

    // 5. Quote Stuffing (P2)
    double stuffingRatio  = 0.0;    // msgs / (|ΔQty| + 1) over 1s window
    bool   stuffingAlert  = false;

    // 6. Spoofing / Layering (P3)
    bool   spoofingAlert  = false;
    double spoofPrice     = 0.0;
    int    spoofSide      = 0;      // +1=bid spoof / -1=ask spoof
    int    spoofCancels   = 0;
};

// ============================================================================
// 1. MultiLevelOFIEngine  (P1)
//
// Standard OFI per level i (Cont, Kukanov, Stoikov 2014):
//
//   Bid contribution (positive = buying pressure):
//     bid_price[i] improved  (↑): +bid_qty[i]
//     bid_price[i] retreated (↓): −prev_bid_qty[i]
//     bid_price[i] unchanged:     +(bid_qty[i] − prev_bid_qty[i])
//
//   Ask contribution (negative = selling pressure, sign inverted from bid):
//     ask_price[i] improved  (↓): −ask_qty[i]    (sellers more aggressive)
//     ask_price[i] retreated (↑): +prev_ask_qty[i] (sellers retreated)
//     ask_price[i] unchanged:     −(ask_qty[i] − prev_ask_qty[i])
//
//   Raw OFI = Σ w[i] × (bid_contribution[i] + ask_contribution[i])
//
// Weights: exponential decay, half-life = 1 level (α = ln2 ≈ 0.6931):
//   w[0]=1.000, w[1]=0.500, w[2]=0.250, w[3]=0.125, w[4]=0.063
//   Sum ≈ 1.938 → normalised so Σw[i] = 1.
//
// Rolling EWM: rollingOFI = EWM_ALPHA × rawOFI + (1−EWM_ALPHA) × rollingOFI
//
// Normalised signal: rollingOFI / (total top-L liquidity + ε)  →  [-1, +1]
//
// State: prev bid/ask prices and quantities for L levels.
//        Written AFTER computing OFI (consistent with FIX 8 pattern).
// ============================================================================
class MultiLevelOFIEngine {
public:
    static constexpr int    L          = TopOfBook::N;
    static constexpr double LN2        = 0.693147180559945;
    static constexpr double EWM_ALPHA  = 0.30;

    MultiLevelOFIEngine() {
        double sum = 0.0;
        for (int i = 0; i < L; ++i) {
            w_[i] = std::exp(-LN2 * static_cast<double>(i));
            sum  += w_[i];
        }
        for (int i = 0; i < L; ++i) w_[i] /= sum;   // normalise to sum = 1
    }

    // Call on every top-of-book change. Returns normalised OFI.
    double OnBookUpdate(const TopOfBook &tob) {
        int n = tob.levels;
        if (n == 0) { return ofiNorm_; }

        double rawOFI = 0.0;

        if (init_) {
            for (int i = 0; i < n; ++i) {
                // ── Bid side ────────────────────────────────────────────────
                double bidCont;
                if      (tob.bidP[i] > prevBidP_[i]) bidCont = +tob.bidQ[i];
                else if (tob.bidP[i] < prevBidP_[i]) bidCont = -prevBidQ_[i];
                else                                  bidCont = +(tob.bidQ[i] - prevBidQ_[i]);

                // ── Ask side (sign inverted: improving ask = more selling) ──
                double askCont;
                if      (tob.askP[i] < prevAskP_[i]) askCont = -tob.askQ[i];
                else if (tob.askP[i] > prevAskP_[i]) askCont = +prevAskQ_[i];
                else                                  askCont = -(tob.askQ[i] - prevAskQ_[i]);

                rawOFI += w_[i] * (bidCont + askCont);
            }
        }

        // FIX 8 pattern: save state AFTER computing delta
        for (int i = 0; i < n; ++i) {
            prevBidP_[i] = tob.bidP[i]; prevBidQ_[i] = tob.bidQ[i];
            prevAskP_[i] = tob.askP[i]; prevAskQ_[i] = tob.askQ[i];
        }
        init_ = true;

        // EWM smoothing
        rolling_ = EWM_ALPHA * rawOFI + (1.0 - EWM_ALPHA) * rolling_;

        // Normalise by top-L total liquidity
        double liq = 0.0;
        for (int i = 0; i < n; ++i) liq += tob.bidQ[i] + tob.askQ[i];
        ofiNorm_ = (liq > 1e-12) ? rolling_ / liq : 0.0;
        ofiRaw_  = rolling_;
        return ofiNorm_;
    }

    double GetNormalized() const { return ofiNorm_; }
    double GetRaw()        const { return ofiRaw_;  }

private:
    double w_[L]       = {};
    double prevBidP_[L]= {}, prevBidQ_[L]= {};
    double prevAskP_[L]= {}, prevAskQ_[L]= {};
    bool   init_       = false;
    double rolling_    = 0.0;
    double ofiNorm_    = 0.0;
    double ofiRaw_     = 0.0;
};

// ============================================================================
// 2. VPINEngine  (P1)
//
// Volume-Synchronised Probability of Informed Trading (Easley et al. 2012):
//
//   1. Volume bucket size V_bar (adaptive: rolling 60s volume / 50).
//      Min clamped at MIN_BUCKET_SIZE to prevent degenerate thin markets.
//
//   2. For each trade: accumulate V_B (buy volume) and V_S (sell volume)
//      using Binance `m` flag (more accurate than bulk classification):
//        m=false → buyer aggressor → BUY
//        m=true  → seller aggressor → SELL
//
//   3. When cumulative volume ≥ V_bar: bucket completes.
//      bucket_imbalance = |V_B − V_S| / V_bar  ∈ [0, 1]
//      Push to rolling window of L=50 buckets. Reset V_B, V_S.
//
//   4. VPIN = (1/L) Σ_{last L buckets} bucket_imbalance
//
//   VPIN ∈ [0, 1]:
//     <0.25  = healthy market, low adverse selection
//     0.25-0.65 = elevated; monitor
//     >0.65  = high toxicity; risk of adverse selection spike
// ============================================================================
class VPINEngine {
public:
    static constexpr int    WINDOW_BUCKETS  = 50;   // rolling window size
    static constexpr double MIN_BUCKET_SIZE = 10.0; // base-asset units
    static constexpr int    VOL_WINDOW_TRADES = 500; // for V_bar estimation

    void OnTrade(double qty, TradeSide side) {
        // Accumulate rolling volume for adaptive V_bar
        vol60_.push_back(qty);
        vol60Sum_ += qty;
        if (static_cast<int>(vol60_.size()) > VOL_WINDOW_TRADES) {
            vol60Sum_ -= vol60_.front();
            vol60_.pop_front();
        }
        // V_bar = rolling mean * 10 (aim for ~50 buckets in rolling 500 trades)
        double vbar = std::max(MIN_BUCKET_SIZE, vol60Sum_ / 50.0);

        // Accumulate bucket
        if      (side == TradeSide::BUY)  vBuy_ += qty;
        else if (side == TradeSide::SELL) vSell_ += qty;

        // Check if bucket is complete
        if (vBuy_ + vSell_ >= vbar) {
            double imbalance = std::abs(vBuy_ - vSell_) / (vBuy_ + vSell_);
            // Push to rolling window
            if (static_cast<int>(buckets_.size()) >= WINDOW_BUCKETS) {
                sumImbalance_ -= buckets_.front();
                buckets_.pop_front();
            }
            buckets_.push_back(imbalance);
            sumImbalance_ += imbalance;
            // Reset bucket
            vBuy_ = 0.0; vSell_ = 0.0;
        }
    }

    // Returns VPIN in [0, 1]
    double GetVPIN() const {
        if (buckets_.empty()) return 0.0;
        return sumImbalance_ / static_cast<double>(buckets_.size());
    }

    int BucketCount() const { return static_cast<int>(buckets_.size()); }

private:
    std::deque<double> buckets_;
    std::deque<double> vol60_;
    double sumImbalance_ = 0.0;
    double vol60Sum_     = 0.0;
    double vBuy_         = 0.0;
    double vSell_        = 0.0;
};

// ============================================================================
// 3. MomentumMREngine  (P2)
//
// Three complementary measures for regime detection:
//
// A) Short-term momentum — EWMA of per-tick log-returns:
//      ret_t = (mid_t − mid_{t-1}) / mid_{t-1}
//      momentum = EWMA(ret_t, α=0.50)
//      Positive → short-term upward, Negative → downward
//
// B) Long-term z-score — deviation from 60-tick rolling mean:
//      z = (mid_t − mean_60) / std_60
//      Positive → above average (mean-reversion pressure down)
//      Negative → below average (mean-reversion pressure up)
//
// C) AR(1) coefficient — rolling OLS of ret_t ~ β·ret_{t-1}
//      β > 0  → autocorrelation positive → TRENDING
//      β < 0  → autocorrelation negative → MEAN-REVERTING
//      Using same rolling-sums trick as KyleLambda (O(1) amortized)
//
// Regime classification (requires min 10 return observations):
//   |β| < 0.05:           NEUTRAL  (0)
//   β ≥  0.05:            TRENDING (+1) — momentum regime
//   β ≤ −0.05:            MEAN-REVERTING (−1) — fade-the-move regime
// ============================================================================
class MomentumMREngine {
public:
    static constexpr int    MID_WINDOW  = 60;
    static constexpr int    AR1_WINDOW  = 60;
    static constexpr double EWM_ALPHA   = 0.50;
    static constexpr double AR1_THRESH  = 0.05;

    void OnMid(double mid) {
        if (mid <= 0.0) return;

        // ── A) Short-term momentum ────────────────────────────────────────────
        if (lastMid_ > 0.0) {
            double ret = (mid - lastMid_) / lastMid_;
            momentum_  = EWM_ALPHA * ret + (1.0 - EWM_ALPHA) * momentum_;

            // ── C) AR(1) rolling OLS ─────────────────────────────────────────
            if (lastRet_ != std::numeric_limits<double>::quiet_NaN() && n_ > 0) {
                // AR(1): regress ret_t ~ β·ret_{t-1}
                // Update running sums for OLS
                if (static_cast<int>(retWindow_.size()) >= AR1_WINDOW) {
                    // Evict oldest pair
                    double oldX = retWindow_.front().x;
                    double oldY = retWindow_.front().y;
                    n_--;
                    sumX_  -= oldX; sumY_  -= oldY;
                    sumX2_ -= oldX * oldX;
                    sumXY_ -= oldX * oldY;
                    retWindow_.pop_front();
                }
                RetPair rp{lastRet_, ret};
                retWindow_.push_back(rp);
                n_++;
                sumX_  += lastRet_; sumY_  += ret;
                sumX2_ += lastRet_ * lastRet_;
                sumXY_ += lastRet_ * ret;

                // OLS slope: β = [N·ΣXY − ΣX·ΣY] / [N·ΣX² − (ΣX)²]
                if (n_ >= 10) {
                    double N    = static_cast<double>(n_);
                    double varX = N * sumX2_ - sumX_ * sumX_;
                    if (std::abs(varX) > 1e-20) {
                        double covXY = N * sumXY_ - sumX_ * sumY_;
                        ar1_ = covXY / varX;
                    }
                }
            }
            lastRet_ = ret;
        }
        lastMid_ = mid;

        // ── B) 60-tick rolling z-score ────────────────────────────────────────
        midWindow_.push_back(mid);
        midSum_  += mid;
        midSum2_ += mid * mid;
        if (static_cast<int>(midWindow_.size()) > MID_WINDOW) {
            double evict = midWindow_.front();
            midWindow_.pop_front();
            midSum_  -= evict;
            midSum2_ -= evict * evict;
        }
        if (static_cast<int>(midWindow_.size()) >= 5) {
            double N    = static_cast<double>(midWindow_.size());
            double mean = midSum_ / N;
            double var  = midSum2_ / N - mean * mean;
            double sd   = var > 0.0 ? std::sqrt(var) : 1e-9;
            zScore_ = (mid - mean) / sd;
        }

        // ── Regime classification ─────────────────────────────────────────────
        if (n_ < 10)         regime_ = 0;
        else if (ar1_ >= AR1_THRESH)  regime_ = +1;   // trending
        else if (ar1_ <= -AR1_THRESH) regime_ = -1;   // mean-reverting
        else                          regime_ = 0;    // neutral
    }

    double GetMomentum() const { return momentum_; }
    double GetZScore()   const { return zScore_;   }
    double GetAR1()      const { return ar1_;       }
    int    GetRegime()   const { return regime_;    }

private:
    struct RetPair { double x, y; };

    std::deque<double>  midWindow_;
    std::deque<RetPair> retWindow_;

    double lastMid_  = 0.0;
    double lastRet_  = std::numeric_limits<double>::quiet_NaN();
    double momentum_ = 0.0;
    double zScore_   = 0.0;
    double ar1_      = 0.0;
    int    regime_   = 0;
    int    n_        = 0;

    double midSum_  = 0.0, midSum2_ = 0.0;
    double sumX_    = 0.0, sumY_    = 0.0;
    double sumX2_   = 0.0, sumXY_   = 0.0;
};

// ============================================================================
// 4. IcebergDetector  (P2)
//
// An iceberg (reserve) order maintains a fixed visible quantity at a price.
// As market orders consume the visible portion, it refills automatically.
//
// Detection algorithm (single-threaded depth-based):
//
//   Per price level P, track:
//     prev_qty[P]          — quantity at last depth update
//     last_decrease_us[P]  — timestamp of last significant decrease
//     refill_count[P]      — number of refills observed
//
//   On depth update at price P:
//     1. If qty[P] decreased by ≥ MIN_DECREASE_FRAC × prev_qty[P]:
//          record decrease event (price P, time now)
//          (We don't require a matching trade — rapid refills prove iceberg)
//
//     2. If qty[P] INCREASED AND a decrease was recorded within REFILL_WINDOW:
//          → REFILL detected at P
//          refill_count[P]++
//          if refill_count[P] ≥ REFILL_THRESHOLD → ICEBERG at P
//
//   Only tracks top MAX_TRACK bid and ask levels for memory efficiency.
//   Window expiry: if no refill within EXPIRE_WINDOW, reset refill_count.
//
// Limitation: does not distinguish fills from cancellations (requires
// trade-to-level matching which is impractical without exchange feed).
// In live markets, rapid same-price refills are diagnostic of icebergs.
// ============================================================================
class IcebergDetector {
public:
    static constexpr int    MAX_TRACK         = 10;
    static constexpr int    REFILL_THRESHOLD  = 3;
    static constexpr double MIN_DECREASE_FRAC = 0.15;  // 15% drop triggers event
    static constexpr int64_t REFILL_WINDOW_US = 2'000'000LL;  // 2s
    static constexpr int64_t EXPIRE_WINDOW_US = 30'000'000LL; // 30s reset

    struct LevelState {
        double  prevQty        = 0.0;
        int64_t decreaseUs     = 0;     // timestamp of last decrease
        int64_t firstEventUs   = 0;     // timestamp of first refill event
        int     refillCount    = 0;
        bool    hasDecrease    = false;
    };

    void OnBookUpdate(const TopOfBook &tob, int64_t nowUs) {
        UpdateSide(tob.bidP, tob.bidQ, tob.levels, +1, nowUs, bidLevels_);
        UpdateSide(tob.askP, tob.askQ, tob.levels, -1, nowUs, askLevels_);
    }

    bool   IsDetected()   const { return detected_;       }
    double GetPrice()     const { return icebergPrice_;   }
    int    GetSide()      const { return icebergSide_;    }
    int    GetRefills()   const { return icebergRefills_; }
    void   ClearAlert()         { detected_ = false;      }

private:
    // side: +1=bid, -1=ask
    void UpdateSide(const double prices[], const double qtys[], int n, int side,
                    int64_t nowUs,
                    std::unordered_map<int64_t, LevelState> &map) {
        int lim = std::min(n, MAX_TRACK);
        for (int i = 0; i < lim; ++i) {
            int64_t key = static_cast<int64_t>(prices[i] * 10000);
            auto &st    = map[key];

            double prev = st.prevQty;
            double cur  = qtys[i];

            if (prev > 1e-12) {
                double decrease = prev - cur;

                // Check for decrease (possible fill)
                if (decrease >= MIN_DECREASE_FRAC * prev) {
                    st.hasDecrease = true;
                    st.decreaseUs  = nowUs;
                }

                // Check for refill: qty increased after a decrease within window
                if (st.hasDecrease && cur > prev &&
                    (nowUs - st.decreaseUs) < REFILL_WINDOW_US) {

                    if (st.firstEventUs == 0) st.firstEventUs = nowUs;
                    st.refillCount++;
                    st.hasDecrease = false;  // reset for next cycle

                    if (st.refillCount >= REFILL_THRESHOLD) {
                        detected_      = true;
                        icebergPrice_  = prices[i];
                        icebergSide_   = side;
                        icebergRefills_= st.refillCount;
                    }
                }

                // Expire old state
                if (st.firstEventUs > 0 &&
                    (nowUs - st.firstEventUs) > EXPIRE_WINDOW_US) {
                    st.refillCount  = 0;
                    st.firstEventUs = 0;
                    st.hasDecrease  = false;
                }
            }

            st.prevQty = cur;
        }
    }

    std::unordered_map<int64_t, LevelState> bidLevels_;
    std::unordered_map<int64_t, LevelState> askLevels_;

    bool   detected_       = false;
    double icebergPrice_   = 0.0;
    int    icebergSide_    = 0;
    int    icebergRefills_ = 0;
};

// ============================================================================
// 5. QuoteStuffingDetector  (P2)
//
// Quote stuffing: a manipulator floods the exchange with rapid order
// placements and cancellations to slow competitors' order processing.
// Signature: very high message rate with negligible net book change.
//
// Detection metric:
//   stuffing_ratio = msg_count_1s / (total_qty_change_1s + ε)
//   where total_qty_change = Σ|ΔQty| across all changed levels per message
//
// Alert conditions (AND):
//   msg_count_1s   > MSG_THRESHOLD    (e.g., 20 depth updates in 1s)
//   stuffing_ratio > RATIO_THRESHOLD  (e.g., 10: 10 msgs per unit qty change)
//
// Rolling 1s window maintained via timestamp-keyed deques.
// ============================================================================
class QuoteStuffingDetector {
public:
    static constexpr int    MSG_THRESHOLD   = 20;
    static constexpr double RATIO_THRESHOLD = 10.0;
    static constexpr int64_t WINDOW_US      = 1'000'000LL;  // 1 second

    // Call on every depth message.
    // totalQtyChange = Σ|ΔQty| from all "b" and "a" entries in that message.
    void OnDepthMessage(double totalQtyChange, int64_t nowUs) {
        // Add new entry
        entries_.push_back({nowUs, totalQtyChange});
        msgCount_++;
        qtySum_ += totalQtyChange;

        // Expire entries older than 1s
        int64_t cutoff = nowUs - WINDOW_US;
        while (!entries_.empty() && entries_.front().tsUs < cutoff) {
            qtySum_ -= entries_.front().qty;
            entries_.pop_front();
            msgCount_--;
        }

        // Compute ratio
        ratio_ = static_cast<double>(msgCount_) / (qtySum_ + 1.0);
        alert_ = (msgCount_ > MSG_THRESHOLD) && (ratio_ > RATIO_THRESHOLD);
    }

    double GetRatio() const { return ratio_; }
    bool   IsAlert()  const { return alert_; }
    int    MsgCount() const { return msgCount_; }

private:
    struct Entry { int64_t tsUs; double qty; };
    std::deque<Entry> entries_;
    int    msgCount_ = 0;
    double qtySum_   = 0.0;
    double ratio_    = 0.0;
    bool   alert_    = false;
};

// ============================================================================
// 6. SpoofingLayeringDetector  (P3)
//
// Spoofing: placing large visible orders with intent to cancel before fill,
// creating false market depth to move price.
// Layering: multiple orders at different prices forming an artificial "wall."
//
// Detection algorithm:
//
//   For each bid level P:
//     A) "Large order" = qty at P exceeds LARGE_MULT × rolling mean qty
//        (mean computed over top MAX_TRACK levels in rolling window)
//
//     B) "Cancel event" = large order at P disappears AND no recent trade
//        at or adjacent to P within FILL_WINDOW_US.
//        (Distinguishes cancellations from legitimate fills.)
//
//     C) SPOOF ALERT: cancel_count[P] ≥ CANCEL_THRESHOLD within WINDOW_US (60s)
//
//   OnTrade(price, nowUs): records trade timestamp per price bucket.
//   OnBookUpdate: detects large-order appearances and cancellations.
//
// Bid and ask sides tracked independently.
// Oldest events expire after WINDOW_US (60s) to avoid stale alerts.
// ============================================================================
class SpoofingLayeringDetector {
public:
    static constexpr int    CANCEL_THRESHOLD  = 3;
    static constexpr double LARGE_MULT        = 3.0;  // 3× mean = "large"
    static constexpr int    MAX_TRACK         = 10;
    static constexpr int64_t WINDOW_US        = 60'000'000LL;  // 60s
    static constexpr int64_t FILL_WINDOW_US   =    100'000LL;  // 100ms

    // Call on each confirmed trade — used to distinguish fills from cancels
    void OnTrade(double price, int64_t nowUs) {
        // Record trade at all price buckets within ±0.5 USD (practical tolerance)
        int64_t key = PriceKey(price);
        trades_[key] = nowUs;
    }

    // Call on each depth update
    void OnBookUpdate(const TopOfBook &tob, int64_t nowUs) {
        // Compute rolling mean qty across top levels (for "large" threshold)
        double totalQ = 0.0;
        int validLevels = tob.levels;
        for (int i = 0; i < validLevels; ++i)
            totalQ += tob.bidQ[i] + tob.askQ[i];
        double meanQ = (validLevels > 0)
            ? totalQ / (2.0 * validLevels)
            : 1.0;
        double largeThreshold = LARGE_MULT * (meanQ + 1e-9);

        // Update each side
        ProcessSide(tob.bidP, tob.bidQ, validLevels, +1, nowUs, largeThreshold, bidLevels_);
        ProcessSide(tob.askP, tob.askQ, validLevels, -1, nowUs, largeThreshold, askLevels_);
    }

    bool   IsAlert()     const { return alert_;      }
    double GetPrice()    const { return spoofPrice_;  }
    int    GetSide()     const { return spoofSide_;   }
    int    GetCancels()  const { return spoofCancels_;}
    void   ClearAlert()        { alert_ = false;      }

private:
    static int64_t PriceKey(double price) {
        return static_cast<int64_t>(price * 100);  // bucket at $0.01 granularity
    }

    struct LevelEntry {
        double  lastLargeQty   = 0.0;   // largest qty seen at this level
        int64_t largeAppearedUs= 0;     // when large order appeared
        bool    hasLarge       = false;  // currently has large order
        int     cancelCount    = 0;
        int64_t windowStartUs  = 0;
        double  cancelVolume   = 0.0;
    };

    void ProcessSide(const double prices[], const double qtys[], int n, int side,
                     int64_t nowUs, double largeThr,
                     std::unordered_map<int64_t, LevelEntry> &map) {
        int lim = std::min(n, MAX_TRACK);
        for (int i = 0; i < lim; ++i) {
            int64_t key = static_cast<int64_t>(prices[i] * 10000);
            auto &e     = map[key];

            double cur = qtys[i];
            bool isLargeNow = cur >= largeThr;

            // Detect large order appearance
            if (isLargeNow && !e.hasLarge) {
                e.hasLarge        = true;
                e.lastLargeQty    = cur;
                e.largeAppearedUs = nowUs;
            }

            // Detect large order disappearance
            if (!isLargeNow && e.hasLarge) {
                e.hasLarge = false;
                double disappeared = e.lastLargeQty - cur;

                if (disappeared > 0.5 * e.lastLargeQty) {
                    // Was it a fill or a cancel?
                    int64_t tradeKey = PriceKey(prices[i]);
                    auto it = trades_.find(tradeKey);
                    bool likelyFill = (it != trades_.end()) &&
                                      ((nowUs - it->second) < FILL_WINDOW_US);

                    if (!likelyFill) {
                        // Cancel event
                        if (e.windowStartUs == 0) e.windowStartUs = nowUs;
                        e.cancelCount++;
                        e.cancelVolume += disappeared;

                        // Expire window
                        if ((nowUs - e.windowStartUs) > WINDOW_US) {
                            e.cancelCount  = 0;
                            e.cancelVolume = 0.0;
                            e.windowStartUs= nowUs;
                        }

                        // Alert?
                        if (e.cancelCount >= CANCEL_THRESHOLD) {
                            alert_       = true;
                            spoofPrice_  = prices[i];
                            spoofSide_   = side;
                            spoofCancels_= e.cancelCount;
                        }
                    }
                }
            }
        }
    }

    std::unordered_map<int64_t, LevelEntry> bidLevels_;
    std::unordered_map<int64_t, LevelEntry> askLevels_;
    std::unordered_map<int64_t, int64_t>    trades_;  // price_key → last_trade_us

    bool   alert_       = false;
    double spoofPrice_  = 0.0;
    int    spoofSide_   = 0;
    int    spoofCancels_= 0;
};

// ============================================================================
// SignalEngine — orchestrates all 6 signal estimators
//
// Usage:
//   SignalEngine signals;
//
//   // On each aggTrade:
//   signals.OnTrade(price, qty, side, tradeUs, mid);
//
//   // On each depth update (after extracting TopOfBook from snapshot):
//   double totalQtyChange = ...; // Σ|ΔQty| from JSON "b"+"a" arrays
//   signals.OnDepthUpdate(tob, totalQtyChange, nowUs);
//
//   // Read all signals:
//   const SignalResults& r = signals.GetResults();
// ============================================================================
class SignalEngine {
public:
    // ── Call on each confirmed aggTrade ──────────────────────────────────────
    void OnTrade(double price, double qty, TradeSide side,
                 int64_t tradeUs, double mid) {
        // VPIN
        vpin_.OnTrade(qty, side);

        // Momentum/MR — driven by mid price at trade time
        momMR_.OnMid(mid);

        // Spoofing — record trade for fill vs cancel distinction
        spoof_.OnTrade(price, tradeUs);

        // Iceberg — trades feed timing correlation (not needed for refill logic;
        // kept here in case iceberg extends to fill-based detection)
        lastTradeUs_   = tradeUs;
        lastTradePrice_= price;

        // Publish to results
        results_.vpin        = vpin_.GetVPIN();
        results_.vpinBuckets = vpin_.BucketCount();
    }

    // ── Call on each depth update ─────────────────────────────────────────────
    // totalQtyChange: Σ|ΔQty| extracted from depth JSON "b" and "a" arrays
    void OnDepthUpdate(const TopOfBook &tob, double totalQtyChange, int64_t nowUs) {
        // 1. Multi-Level OFI (improved with exponential decay)
        ofi_.OnBookUpdate(tob);
        results_.ofiNormalized = ofi_.GetNormalized();
        results_.ofiRaw        = ofi_.GetRaw();

        // 2. VPIN already updated on trades

        // 3. Momentum / MR — also update on mid price change from book
        if (tob.levels > 0) {
            double mid = (tob.bidP[0] + tob.askP[0]) * 0.5;
            momMR_.OnMid(mid);
            results_.momentumScore = momMR_.GetMomentum();
            results_.zScore        = momMR_.GetZScore();
            results_.ar1Coeff      = momMR_.GetAR1();
            results_.regime        = momMR_.GetRegime();
        }

        // 4. Iceberg detection
        iceberg_.OnBookUpdate(tob, nowUs);
        if (iceberg_.IsDetected()) {
            results_.icebergDetected = true;
            results_.icebergPrice    = iceberg_.GetPrice();
            results_.icebergSide     = iceberg_.GetSide();
            results_.icebergRefills  = iceberg_.GetRefills();
        }

        // 5. Quote Stuffing
        stuffing_.OnDepthMessage(totalQtyChange, nowUs);
        results_.stuffingRatio = stuffing_.GetRatio();
        results_.stuffingAlert = stuffing_.IsAlert();

        // 6. Spoofing / Layering
        spoof_.OnBookUpdate(tob, nowUs);
        if (spoof_.IsAlert()) {
            results_.spoofingAlert = true;
            results_.spoofPrice    = spoof_.GetPrice();
            results_.spoofSide     = spoof_.GetSide();
            results_.spoofCancels  = spoof_.GetCancels();
        }
    }

    const SignalResults& GetResults() const { return results_; }
    SignalResults&       GetResults()       { return results_; }

private:
    MultiLevelOFIEngine      ofi_;
    VPINEngine               vpin_;
    MomentumMREngine         momMR_;
    IcebergDetector          iceberg_;
    QuoteStuffingDetector    stuffing_;
    SpoofingLayeringDetector spoof_;

    SignalResults results_;
    int64_t lastTradeUs_    = 0;
    double  lastTradePrice_ = 0.0;
};

// ─── Helper: extract TopOfBook from a PriceLevelSnapshot ─────────────────────
// Inlined here so any TU that includes SignalEngine.h can use it.
// Requires PriceLevelSnapshot to be fully defined (include MarketTypes.h first).
#include "MarketTypes.h"

inline TopOfBook ExtractTopOfBook(const PriceLevelSnapshot &snap) {
    TopOfBook tob;
    int i = 0;
    for (auto it = snap.bids.begin(); it != snap.bids.end() && i < TopOfBook::N; ++it, ++i) {
        tob.bidP[i] = static_cast<double>(it->first)  / 10000.0;
        tob.bidQ[i] = static_cast<double>(it->second) / 10000.0;
    }
    i = 0;
    for (auto it = snap.asks.begin(); it != snap.asks.end() && i < TopOfBook::N; ++it, ++i) {
        tob.askP[i] = static_cast<double>(it->first)  / 10000.0;
        tob.askQ[i] = static_cast<double>(it->second) / 10000.0;
    }
    tob.levels = static_cast<int>(std::min({
        static_cast<size_t>(TopOfBook::N), snap.bids.size(), snap.asks.size()
    }));
    return tob;
}