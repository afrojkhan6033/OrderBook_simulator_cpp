#pragma once
// ============================================================================
// RegimeEngine.h  —  Statistical Regime & Market State Engine  v1
//
// Dependencies (must precede this header in every TU):
//   SignalEngine.h   → SignalResults (for OFI, VPIN, momentum inputs)
//
// Feature                      Algorithm / Method                     Cost
// ─────────────────────────────────────────────────────────────────────────
// P1. VolatilityRegime         Yang-Zhang estimator (adapted for      O(1)
//                              tick data via Rogers-Satchell method)
//                              Annualised vol → LOW/NORMAL/HIGH/EXTREME
//
// P2. SpreadRegimeTick         Rolling spread histogram (100 bars)    O(1)
//                              Tick-normalised, mean/σ widening alert
//
// P2. MidReturnAutocorrelation Lag-1 ACF via running Welford sums    O(1)
//                              ρ₁>+0.10=momentum / <-0.10=mean-rev
//
// P2. HurstExponent            R/S rescaled range on rolling 64-bar   O(N)
//                              window. H>0.55=trend / H<0.45=MR
//
// P3. HMMRegimeFilter          Online 2-state HMM (Bayesian filter)  O(1)
//                              Observations: [OFI, VPIN, spreadBps]
//                              States: BULL_MICRO / BEAR_MICRO
//                              Fixed transition + EWM emission update
//
// Direction accuracy improvement:
//   RegimeAdjustedScore: composite signal gated and scaled by regime
//   confidence. HMM bull-state probability amplifies long signals;
//   bear-state probability amplifies short signals.
//
// Thread-safety: NONE — single processing thread.
// All prices/quantities in USD / base-asset (NOT ×10000 encoded).
// ============================================================================
#include "SignalEngine.h"   // SignalResults
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _USE_MATH_DEFINES

#include <cmath>

#include <deque>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <limits>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── All regime results in one flat struct ────────────────────────────────────
struct RegimeResults {
    // ── P1: Volatility Regime ─────────────────────────────────────────────────
    double realizedVolAnnualized  = 0.0;  // % per year (e.g., 85.0 = 85%)
    double realizedVolPerTick     = 0.0;  // log-return std per depth tick
    int    volRegime              = 0;    // 0=LOW 1=NORMAL 2=HIGH 3=EXTREME
    // Regime thresholds (annualised %): <30=LOW, 30-80=NORMAL, 80-150=HIGH, >150=EXTREME

    // ── P2: Spread Regime ─────────────────────────────────────────────────────
    double spreadTicks            = 0.0;  // current spread / tick_size
    double spreadTickMean         = 0.0;  // rolling 100-bar mean (ticks)
    double spreadTickStd          = 0.0;  // rolling 100-bar std
    double spreadZScore           = 0.0;  // (current - mean) / std
    bool   spreadWideningAlert    = false; // spread > mean + 2σ
    bool   flashCrashPrecursor    = false; // widening + VPIN > 0.55
    // Histogram: buckets [0-1, 1-2, 2-3, 3-5, 5-10, >10] ticks
    int    spreadHist[6]          = {};

    // ── P2: Autocorrelation of Mid Returns ────────────────────────────────────
    double autocorrLag1           = 0.0;  // ρ₁ ∈ [-1, +1]
    int    autocorrRegime         = 0;    // +1=momentum / -1=mean-rev / 0=neutral
    double autocorrZScore         = 0.0;  // statistical significance (√N × ρ₁)

    // ── P2: Hurst Exponent ────────────────────────────────────────────────────
    double hurstExponent          = 0.5;  // H ∈ (0, 1)
    int    hurstRegime            = 0;    // +1=trending / -1=mean-rev / 0=random-walk
    bool   hurstReliable          = false; // needs ≥ 64 observations

    // ── P3: HMM State ─────────────────────────────────────────────────────────
    int    hmmState               = 0;    // 0=BULL_MICRO / 1=BEAR_MICRO
    double hmmBullProb            = 0.5;  // P(state=BULL|observations)
    double hmmBearProb            = 0.5;  // P(state=BEAR|observations)
    double hmmTransitionProb      = 0.0;  // P(state changed this tick)
    bool   hmmConfident           = false; // max(prob) > 0.75

    // ── Direction Accuracy: Regime-Adjusted Composite Score ───────────────────
    // This is the primary trading signal for the engine.
    // Blends all regime information into a single directional probability.
    double regimeAdjustedScore   = 0.0;  // [-1, +1] enhanced directional signal
    int    regimeAdjustedDir     = 0;    // +1=LONG / -1=SHORT / 0=FLAT
    double regimeConfidence      = 0.0;  // [0, 1] signal confidence
    double edgeScore             = 0.0;  // expected edge per trade (bps proxy)
};

// ============================================================================
// P1. YangZhangVolatilityEstimator
//
// Yang-Zhang (2000) is the minimum-variance unbiased estimator for
// historical volatility using OHLC data. For L2 order-book tick data
// (100ms bars, no distinct open/high/low/close), we use the Rogers-Satchell
// (1991) adaptation for close-only data, which reduces to the standard
// close-to-close estimator with a bias correction:
//
//   r_t = ln(mid_t / mid_{t-1})   (log mid-price return per tick)
//
//   σ²_RS = (1 / N) × Σ(r_t²)    (standard Rogers-Satchell for tick data)
//
//   This equals E[r²] which IS the Yang-Zhang estimator when open=close
//   (i.e., no gap between bar-close and next-bar-open — true for
//   continuous tick data).
//
//   σ_tick       = sqrt(σ²_RS)
//   σ_annualised = σ_tick × sqrt(ticks_per_year)
//
// Ticks per year calibration (100ms depth stream, 24/7 crypto):
//   ticks/second = 10 (at 100ms)
//   ticks/year   = 10 × 86400 × 365 = 315,360,000
//   sqrt(ticks/year) = 17,759
//
// But for display: σ_annualised = σ_tick × ANNUALISE_FACTOR
//
// Welford online variance for O(1) updates, O(1) query.
// Rolling window of N_WINDOW returns; oldest evicted via deque.
//
// Regime thresholds (annualised % for crypto):
//   LOW     < 30%   — quiet market, tight spreads expected
//   NORMAL  30-80%  — typical crypto conditions
//   HIGH    80-150% — elevated: reduce position size
//   EXTREME > 150%  — crisis: halt new positions
// ============================================================================
class YangZhangVolatilityEstimator {
public:
    // For 100ms tick stream, 24/7 crypto: ticks per year = 315,360,000
    // sqrt(315360000) ≈ 17759.  Multiply tick vol by this for annual vol.
    // Express as % → × 100.
    static constexpr double TICKS_PER_YEAR  = 315'360'000.0;
    static constexpr double ANNUALISE_FACTOR = 17759.0 * 100.0;  // → annual %

    static constexpr int    N_WINDOW = 100;  // rolling 100 ticks ≈ 10 seconds

    // Regime thresholds (annual %)
    static constexpr double THRESH_LOW     =  30.0;
    static constexpr double THRESH_NORMAL  =  80.0;
    static constexpr double THRESH_HIGH    = 150.0;

    void OnMidPrice(double midPrice) {
        if (midPrice <= 0.0) return;
        if (lastMid_ > 0.0) {
            double r = std::log(midPrice / lastMid_);
            AddReturn(r);
        }
        lastMid_ = midPrice;
    }

    // Returns annualised volatility in %
    double GetAnnualisedVol() const {
        if (n_ < 5) return 0.0;
        return std::sqrt(GetTickVariance()) * ANNUALISE_FACTOR;
    }

    double GetTickVol() const {
        return n_ >= 5 ? std::sqrt(GetTickVariance()) : 0.0;
    }

    // 0=LOW 1=NORMAL 2=HIGH 3=EXTREME
    int GetRegime() const {
        double av = GetAnnualisedVol();
        if      (av < THRESH_LOW)    return 0;
        else if (av < THRESH_NORMAL) return 1;
        else if (av < THRESH_HIGH)   return 2;
        else                         return 3;
    }

    int Count() const { return n_; }

private:
    void AddReturn(double r) {
        // Evict oldest if full
        if (static_cast<int>(buf_.size()) >= N_WINDOW) {
            double old = buf_.front();
            buf_.pop_front();
            sumR_  -= old;
            sumR2_ -= old * old;
            n_--;
        }
        buf_.push_back(r);
        sumR_  += r;
        sumR2_ += r * r;
        n_++;
    }

    // Population variance (Rogers-Satchell = E[r²] for zero-mean tick returns)
    double GetTickVariance() const {
        if (n_ < 2) return 0.0;
        double N    = static_cast<double>(n_);
        double mean = sumR_ / N;
        // Sample variance: more unbiased for small N
        return std::max(0.0, (sumR2_ - N * mean * mean) / (N - 1.0));
    }

    std::deque<double> buf_;
    double sumR_   = 0.0;
    double sumR2_  = 0.0;
    double lastMid_= 0.0;
    int    n_      = 0;
};

// ============================================================================
// P2. SpreadRegimeTick
//
// Spread in ticks = (bestAsk − bestBid) / TICK_SIZE
//
// TICK_SIZE = $0.01 for SOLUSDT spot (Binance minimum price increment).
//   Note: actual tick size varies by symbol. The engine accepts it as a
//   runtime parameter so it works for BTCUSDT ($0.01), SOLUSDT ($0.01),
//   ETHUSDT ($0.01), futures, etc.
//
// Rolling histogram (100 bars):
//   Buckets: [0-1), [1-2), [2-3), [3-5), [5-10), [≥10) ticks
//   Tracking the shape: a heavy-tailed spread distribution indicates
//   intermittent liquidity crises.
//
// Widening alert: spreadZScore > +2.0 (current spread > mean + 2σ)
//   This flags sudden adverse conditions: inventory builds, HFT withdrawal.
//
// Flash-crash precursor: widening alert AND VPIN > 0.55
//   The combination of toxic flow (VPIN) + market-maker withdrawal (spread
//   widening) is the signature of flash-crash initiation.
//
// Welford online stats (O(1) per tick) for mean/variance.
// ============================================================================
class SpreadRegimeTick {
public:
    static constexpr int    N_WINDOW    = 100;
    static constexpr double ACF_THRESH  = 2.0;   // z-score for widening alert
    static constexpr double VPIN_THRESH = 0.55;  // VPIN threshold for flash precursor

    void OnSpread(double spreadUSD, double tickSize, double vpin) {
        tickSize_ = (tickSize > 1e-9) ? tickSize : 0.01;
        double spreadTicks = spreadUSD / tickSize_;

        // Rolling stats (Welford)
        if (static_cast<int>(buf_.size()) >= N_WINDOW) {
            double old = buf_.front(); buf_.pop_front();
            double delta = old - mean_;
            mean_ -= delta / static_cast<double>(n_);
            // Update M2 on removal (Welford reverse)
            m2_  -= (old - mean_) * (old - old); // simplified: recalculate
            n_--;
            // Recompute M2 properly from scratch is expensive; use running sum
            sum_  -= old;
            sum2_ -= old * old;
        }
        buf_.push_back(spreadTicks);
        sum_  += spreadTicks;
        sum2_ += spreadTicks * spreadTicks;
        n_++;
        double N    = static_cast<double>(n_);
        mean_       = sum_  / N;
        double var  = (sum2_ / N) - (mean_ * mean_);
        if (n_ >= 2) var = var * N / (N - 1.0);
        std_        = (var > 0.0) ? std::sqrt(var) : 1e-9;

        lastSpreadTicks_ = spreadTicks;

        // Z-score
        zScore_ = (std_ > 1e-9) ? (spreadTicks - mean_) / std_ : 0.0;

        // Alerts
        wideningAlert_     = (zScore_ > ACF_THRESH);
        flashPrecursor_    = wideningAlert_ && (vpin > VPIN_THRESH);

        // Histogram
        UpdateHistogram(spreadTicks);
    }

    double GetSpreadTicks()   const { return lastSpreadTicks_; }
    double GetMean()          const { return mean_; }
    double GetStd()           const { return std_; }
    double GetZScore()        const { return zScore_; }
    bool   IsWidening()       const { return wideningAlert_; }
    bool   IsFlashPrecursor() const { return flashPrecursor_; }
    const int* GetHistogram() const { return hist_; }
    int    Count()            const { return n_; }

private:
    void UpdateHistogram(double ticks) {
        int bucket;
        if      (ticks < 1.0)  bucket = 0;
        else if (ticks < 2.0)  bucket = 1;
        else if (ticks < 3.0)  bucket = 2;
        else if (ticks < 5.0)  bucket = 3;
        else if (ticks < 10.0) bucket = 4;
        else                   bucket = 5;
        hist_[bucket]++;
    }

    std::deque<double> buf_;
    double sum_  = 0.0, sum2_ = 0.0;
    double mean_ = 0.0, std_  = 1e-9;
    double m2_   = 0.0;
    double zScore_          = 0.0;
    double lastSpreadTicks_ = 0.0;
    double tickSize_        = 0.01;
    bool   wideningAlert_   = false;
    bool   flashPrecursor_  = false;
    int    hist_[6]         = {};
    int    n_               = 0;
};

// ============================================================================
// P2. MidReturnAutocorrelation
//
// Lag-1 autocorrelation of mid-price log-returns:
//
//   r_t   = ln(mid_t / mid_{t-1})
//   ρ₁    = Cov(r_t, r_{t-1}) / Var(r_t)
//         = [N·Σr_t·r_{t-1} − Σr_t·Σr_{t-1}] / [N·Σr²_{t} − (Σr_t)²]
//
// Running sums via same rolling-OLS approach as KyleLambdaEstimator:
//   sumX, sumY, sumX2, sumXY (where X=r_{t-1}, Y=r_t)
//
// Statistical significance: Bartlett's standard error for ACF:
//   SE(ρ₁) ≈ 1/√N  →  z-score = ρ₁ × √N
//   |z| > 1.96 → statistically significant at 95% level
//
// Regime classification (with significance filter):
//   |z| > 1.96 AND ρ₁ > +0.10 → MOMENTUM (+1)
//   |z| > 1.96 AND ρ₁ < -0.10 → MEAN-REVERSION (-1)
//   Otherwise → NEUTRAL (0)
//
// Rolling window N=100 returns (same as vol estimator for consistency).
// ============================================================================
class MidReturnAutocorrelation {
public:
    static constexpr int    N_WINDOW     = 100;
    static constexpr double ACF_THRESH   = 0.10;   // ρ₁ threshold
    static constexpr double ZSCORE_CRIT  = 1.96;   // 95% confidence

    void OnMidPrice(double midPrice) {
        if (midPrice <= 0.0) return;
        if (lastMid_ > 0.0) {
            double r = std::log(midPrice / lastMid_);

            if (hasLastR_) {
                // Add pair (lastR_, r) as (X, Y)
                if (static_cast<int>(pairBuf_.size()) >= N_WINDOW) {
                    auto &old = pairBuf_.front();
                    n_--;
                    sumX_  -= old.x; sumY_  -= old.y;
                    sumX2_ -= old.x * old.x;
                    sumXY_ -= old.x * old.y;
                    pairBuf_.pop_front();
                }
                Pair p{lastR_, r};
                pairBuf_.push_back(p);
                n_++;
                sumX_  += lastR_; sumY_  += r;
                sumX2_ += lastR_ * lastR_;
                sumXY_ += lastR_ * r;
            }
            lastR_    = r;
            hasLastR_ = true;
        }
        lastMid_ = midPrice;
        ComputeACF();
    }

    double GetACF()      const { return acf_;     }
    double GetZScore()   const { return zScore_;  }
    int    GetRegime()   const { return regime_;  }
    int    Count()       const { return n_;       }

private:
    void ComputeACF() {
        if (n_ < 10) { acf_ = 0; zScore_ = 0; regime_ = 0; return; }
        double N    = static_cast<double>(n_);
        double varX = N * sumX2_ - sumX_ * sumX_;
        if (std::abs(varX) < 1e-20) { acf_ = 0; zScore_ = 0; regime_ = 0; return; }
        double covXY = N * sumXY_ - sumX_ * sumY_;
        // Normalize: pearson correlation (using same variance for X and Y since
        // both are returns from same series; assume var(X)≈var(Y))
        double varY  = N * sumX2_ - sumX_ * sumX_;  // symmetric assumption
        double denom = std::sqrt(std::abs(varX * varY));
        if (denom < 1e-20) { acf_ = 0; zScore_ = 0; regime_ = 0; return; }
        acf_    = covXY / denom;
        acf_    = std::max(-1.0, std::min(1.0, acf_));
        zScore_ = acf_ * std::sqrt(static_cast<double>(n_));   // Bartlett SE
        // Regime
        if      (std::abs(zScore_) > ZSCORE_CRIT && acf_ >  ACF_THRESH) regime_ = +1;
        else if (std::abs(zScore_) > ZSCORE_CRIT && acf_ < -ACF_THRESH) regime_ = -1;
        else                                                              regime_ =  0;
    }

    struct Pair { double x, y; };
    std::deque<Pair> pairBuf_;
    double sumX_=0, sumY_=0, sumX2_=0, sumXY_=0;
    double lastMid_=0, lastR_=0;
    bool   hasLastR_=false;
    int    n_=0;
    double acf_=0, zScore_=0;
    int    regime_=0;
};

// ============================================================================
// P3. HurstExponentRS
//
// Hurst Exponent via Rescaled Range (R/S) Analysis (Hurst 1951):
//
// Given a time series of N log-returns {r_1, ..., r_N}:
//
//   1. Compute mean: μ = (1/N) Σ r_t
//   2. Compute cumulative deviation: Y_t = Σ_{s=1}^{t} (r_s − μ)
//   3. Range:  R = max(Y_t) − min(Y_t)   over t=1..N
//   4. Scale:  S = std(r_1..r_N)
//   5. R/S statistic: (R/S)_N
//
// For a self-similar process: E[R/S] ≈ c × N^H
//   → H = log(R/S) / log(N)   (single-scale estimator)
//
// Rolling window: N_RS = 64 observations (power of 2).
// At 100ms depth stream: 64 ticks = 6.4 seconds lookback.
//
// Implementation: O(N) per update (full R/S on rolling window).
// For HFT latency: 64 iterations is negligible (< 1μs).
//
// Interpretation:
//   H > 0.55 → TRENDING (long memory, momentum persists)
//   H < 0.45 → MEAN-REVERTING (anti-persistent, mean-reversion)
//   0.45-0.55 → RANDOM WALK (efficient market in this regime)
//
// H is unreliable for N < 32; flag hurstReliable accordingly.
// ============================================================================
class HurstExponentRS {
public:
    static constexpr int    N_RS          = 64;
    static constexpr double HURST_TREND   = 0.55;
    static constexpr double HURST_MR      = 0.45;

    void OnMidPrice(double midPrice) {
        if (midPrice <= 0.0) return;
        if (lastMid_ > 0.0) {
            double r = std::log(midPrice / lastMid_);
            buf_.push_back(r);
            if (static_cast<int>(buf_.size()) > N_RS) buf_.pop_front();
        }
        lastMid_ = midPrice;
        if (static_cast<int>(buf_.size()) >= 32) ComputeHurst();
    }

    double GetHurst()    const { return hurst_;   }
    int    GetRegime()   const { return regime_;  }
    bool   IsReliable()  const { return reliable_;}
    int    Count()       const { return static_cast<int>(buf_.size()); }

private:
    void ComputeHurst() {
        int N  = static_cast<int>(buf_.size());
        if (N < 4) return;

        // Mean
        double mu = 0.0;
        for (double r : buf_) mu += r;
        mu /= static_cast<double>(N);

        // Cumulative deviations and range
        double Y = 0.0, Ymax = 0.0, Ymin = 0.0;
        double sumSq = 0.0;
        for (int i = 0; i < N; ++i) {
            Y    += buf_[i] - mu;
            Ymax  = std::max(Ymax, Y);
            Ymin  = std::min(Ymin, Y);
            double dev = buf_[i] - mu;
            sumSq += dev * dev;
        }

        double R = Ymax - Ymin;
        double S = std::sqrt(sumSq / static_cast<double>(N));

        if (S < 1e-15 || R < 1e-15) { reliable_ = false; return; }

        double RS  = R / S;
        hurst_     = std::log(RS) / std::log(static_cast<double>(N));
        hurst_     = std::max(0.01, std::min(0.99, hurst_));
        reliable_  = (N >= 32);

        if      (hurst_ > HURST_TREND) regime_ = +1;   // trending
        else if (hurst_ < HURST_MR)    regime_ = -1;   // mean-reverting
        else                           regime_ =  0;   // random walk
    }

    std::deque<double> buf_;
    double lastMid_ = 0.0;
    double hurst_   = 0.5;
    bool   reliable_= false;
    int    regime_  = 0;
};

// ============================================================================
// P3. OnlineHMMFilter
//
// 2-state Hidden Markov Model with online Bayesian (forward-algorithm) update.
//
// States:
//   0 = BULL_MICRO: low adverse-selection, strong buy-side pressure, tight book
//   1 = BEAR_MICRO: high adverse-selection, sell-side dominance, wide book
//
// Observation vector at each tick (3 dimensions):
//   z[0] = ofi_norm    (OFI normalised ∈ [-1, +1])
//   z[1] = vpin        (VPIN ∈ [0, 1])
//   z[2] = spread_bps  (quoted spread in bps, normalised to [0, 1])
//
// Emission model: factored Gaussian (independent dimensions per state):
//   P(z | state=k) = Π_d N(z[d]; μ_k[d], σ_k²[d])
//
// Transition matrix A (fixed, estimated from crypto microstructure priors):
//   A[i][j] = P(state_t+1=j | state_t=i)
//   A = [[0.95, 0.05],   // bull→bull=0.95, bull→bear=0.05
//        [0.10, 0.90]]   // bear→bull=0.10, bear→bear=0.90
//   Bull state more persistent than bear (market-maker presence is sticky).
//
// Emission parameters initialised to market priors:
//   State 0 (BULL): ofi_mean=+0.10, vpin_mean=0.25, spread_mean=0.10
//   State 1 (BEAR): ofi_mean=-0.10, vpin_mean=0.60, spread_mean=0.40
//   All σ initialised to 0.20 (broad prior uncertainty).
//
// Online emission update via EWM (α=0.02 per observation):
//   μ_k[d] ← α × z[d] × γ_k + (1-α) × μ_k[d]   (responsibility-weighted)
//   σ²_k[d] ← α × γ_k × (z[d]-μ_k[d])² + (1-α) × σ²_k[d]
//   where γ_k = P(state=k | all observations up to t)
//
// Forward algorithm (α-pass) at each tick:
//   α_new[j] = Σ_i [ α_prev[i] × A[i][j] ] × P(z|state=j)
//   Normalise to sum to 1 → state probabilities
//
// Confidence: max(P(state)) > 0.75 → HMM is confident about current regime.
// ============================================================================
class OnlineHMMFilter {
public:
    static constexpr int    N_STATES    = 2;
    static constexpr int    N_OBS       = 3;   // [ofi, vpin, spread_norm]
    static constexpr double EWM_ALPHA   = 0.02;  // emission update rate
    static constexpr double CONF_THRESH = 0.75;  // confidence threshold

    OnlineHMMFilter() {
        // Initial state probabilities (uniform)
        alpha_[0] = 0.5; alpha_[1] = 0.5;

        // Transition matrix (row=from, col=to)
        A_[0][0] = 0.95; A_[0][1] = 0.05;
        A_[1][0] = 0.10; A_[1][1] = 0.90;

        // Emission means (state 0=BULL, state 1=BEAR)
        // obs[0]=ofi_norm, obs[1]=vpin, obs[2]=spread_norm
        mu_[0][0] = +0.10;  mu_[0][1] = 0.25;  mu_[0][2] = 0.10;
        mu_[1][0] = -0.10;  mu_[1][1] = 0.60;  mu_[1][2] = 0.40;

        // Emission variances
        for (int k = 0; k < N_STATES; ++k)
            for (int d = 0; d < N_OBS; ++d)
                sigma2_[k][d] = 0.04;  // σ=0.20 initial
    }

    // obs: {ofi_norm, vpin, spread_norm (0-1 scaled)}
    void Update(const double obs[N_OBS]) {
        // ── Forward step ─────────────────────────────────────────────────────
        double alpha_new[N_STATES];
        double totalNorm = 0.0;

        for (int j = 0; j < N_STATES; ++j) {
            // Σ_i alpha_prev[i] × A[i][j]
            double trans = 0.0;
            for (int i = 0; i < N_STATES; ++i)
                trans += alpha_[i] * A_[i][j];

            // Emission: P(z|state=j) = Π_d N(z[d]; mu[j][d], sigma2[j][d])
            double emission = 1.0;
            for (int d = 0; d < N_OBS; ++d) {
                double sig2 = std::max(sigma2_[j][d], 1e-6);
                double diff = obs[d] - mu_[j][d];
                double pdf  = std::exp(-0.5 * diff * diff / sig2)
                              / std::sqrt(2.0 * M_PI * sig2);
                emission   *= std::max(1e-10, pdf);  // numerical floor
            }
            alpha_new[j] = trans * emission;
            totalNorm   += alpha_new[j];
        }

        // Normalise
        if (totalNorm > 1e-20) {
            for (int j = 0; j < N_STATES; ++j) alpha_[j] = alpha_new[j] / totalNorm;
        } else {
            // Degenerate: reset to prior
            for (int j = 0; j < N_STATES; ++j) alpha_[j] = 1.0 / N_STATES;
        }

        // ── Emission parameter update (EWM, responsibility-weighted) ──────────
        for (int k = 0; k < N_STATES; ++k) {
            double gamma = alpha_[k];   // posterior responsibility
            for (int d = 0; d < N_OBS; ++d) {
                double diff   = obs[d] - mu_[k][d];
                mu_[k][d]    += EWM_ALPHA * gamma * diff;
                sigma2_[k][d] = (1.0 - EWM_ALPHA) * sigma2_[k][d]
                                + EWM_ALPHA * gamma * diff * diff;
                sigma2_[k][d] = std::max(sigma2_[k][d], 1e-4);  // min σ=0.01
            }
        }

        // ── State tracking ────────────────────────────────────────────────────
        int prevState = state_;
        state_   = (alpha_[0] >= alpha_[1]) ? 0 : 1;
        changed_ = (state_ != prevState);
    }

    double GetBullProb()      const { return alpha_[0]; }
    double GetBearProb()      const { return alpha_[1]; }
    int    GetState()         const { return state_;    }
    bool   IsConfident()      const { return std::max(alpha_[0], alpha_[1]) > CONF_THRESH; }
    bool   DidTransition()    const { return changed_;  }

    // Return the emission mean for a given state and observation dimension
    double GetMean(int state, int dim) const { return mu_[state][dim]; }

private:
    double alpha_[N_STATES];          // forward variable (state probabilities)
    double A_[N_STATES][N_STATES];   // transition matrix
    double mu_[N_STATES][N_OBS];     // emission means (adaptive)
    double sigma2_[N_STATES][N_OBS]; // emission variances (adaptive)
    int    state_   = 0;
    bool   changed_ = false;
};

// ============================================================================
// RegimeEngine — orchestrates all 5 sub-engines + direction score enhancement
//
// Regime-Adjusted Composite Score:
//   The core innovation: filter and amplify the raw composite score from
//   RiskEngine based on aligned regime signals.
//
//   Alignment score = average of aligned regime signals:
//     HMM alignment:   +1 if BULL+long or BEAR+short, else -1, scaled by confidence
//     Hurst alignment: +1 if trending+momentum, -1 if MR+momentum (counter-trend)
//     ACF alignment:   +1 if ACF regime matches signal direction
//     Vol gate:        vol=EXTREME → reduce score by 50% (uncertainty penalty)
//
//   regime_adjusted = raw_composite × (1 + 0.5 × alignment_score)
//   Clamped to [-1, +1].
//
// Edge score (proxy for expected trade profitability in bps):
//   edge = |regime_adjusted| × 10 × hitRate × (1 - VPIN) × (1/volRegime_penalty)
//   Higher edge = better risk-adjusted opportunity.
//
// Usage:
//   RegimeEngine regime;
//   regime.OnDepthUpdate(snap, mid, spreadUSD, tickSize, sig, rawComposite, vpin, hitRate, nowUs);
//   const RegimeResults& r = regime.GetResults();
// ============================================================================
class RegimeEngine {
public:
    void OnDepthUpdate(double midPrice,
                       double spreadUSD,
                       double tickSize,
                       const SignalResults &sig,
                       double rawCompositeScore,   // from RiskEngine CompositeDirectionModel
                       double vpin,
                       double hitRate,
                       int64_t /*nowUs*/)
    {
        // ── Feed all sub-engines ──────────────────────────────────────────────
        yzVol_.OnMidPrice(midPrice);
        acf_.OnMidPrice(midPrice);
        hurst_.OnMidPrice(midPrice);

        double spreadBps = (midPrice > 0.0)
            ? (spreadUSD / midPrice) * 10000.0 : 0.0;
        spread_.OnSpread(spreadUSD, tickSize, vpin);

        // Normalise observations for HMM
        double ofi_norm    = sig.ofiNormalized;              // already [-1,+1]
        double vpin_obs    = vpin;                           // [0,1]
        double spread_norm = std::min(1.0, spreadBps / 20.0); // cap at 20bps=1.0
        double obs[3] = {ofi_norm, vpin_obs, spread_norm};
        hmm_.Update(obs);

        // ── Assemble results ──────────────────────────────────────────────────
        results_.realizedVolAnnualized = yzVol_.GetAnnualisedVol();
        results_.realizedVolPerTick    = yzVol_.GetTickVol();
        results_.volRegime             = yzVol_.GetRegime();

        results_.spreadTicks           = spread_.GetSpreadTicks();
        results_.spreadTickMean        = spread_.GetMean();
        results_.spreadTickStd         = spread_.GetStd();
        results_.spreadZScore          = spread_.GetZScore();
        results_.spreadWideningAlert   = spread_.IsWidening();
        results_.flashCrashPrecursor   = spread_.IsFlashPrecursor();
        {
            const int *h = spread_.GetHistogram();
            for (int i=0;i<6;++i) results_.spreadHist[i] = h[i];
        }

        results_.autocorrLag1          = acf_.GetACF();
        results_.autocorrRegime        = acf_.GetRegime();
        results_.autocorrZScore        = acf_.GetZScore();

        results_.hurstExponent         = hurst_.GetHurst();
        results_.hurstRegime           = hurst_.GetRegime();
        results_.hurstReliable         = hurst_.IsReliable();

        results_.hmmState              = hmm_.GetState();
        results_.hmmBullProb           = hmm_.GetBullProb();
        results_.hmmBearProb           = hmm_.GetBearProb();
        results_.hmmTransitionProb     = hmm_.DidTransition() ? 1.0 : 0.0;
        results_.hmmConfident          = hmm_.IsConfident();

        // ── Regime-Adjusted Composite Score ───────────────────────────────────
        ComputeRegimeAdjustedScore(rawCompositeScore, vpin, hitRate);
    }

    const RegimeResults& GetResults() const { return results_; }
    RegimeResults&       GetResults()       { return results_; }

private:
    void ComputeRegimeAdjustedScore(double rawScore, double vpin, double hitRate) {
        // Signal direction from raw composite
        double dir = (rawScore > 0.0) ? 1.0 : (rawScore < 0.0) ? -1.0 : 0.0;

        // ── Compute alignment score ─────────────────────────────────────────
        double alignment = 0.0;
        int    alignCount = 0;

        // 1. HMM alignment: bull→long agreement or bear→short agreement
        if (results_.hmmConfident) {
            double hmmDir = results_.hmmBullProb - results_.hmmBearProb;  // ∈[-1,+1]
            alignment += hmmDir * dir;
            alignCount++;
        }

        // 2. Hurst alignment: H>0.55 endorses momentum; H<0.45 endorses MR fade
        if (results_.hurstReliable) {
            double hurstContrib = 0.0;
            if (results_.hurstRegime == +1) {
                // Trending: amplify signals (momentum is real)
                hurstContrib = (results_.hurstExponent - 0.5) * 4.0;  // 0→0.2 map
            } else if (results_.hurstRegime == -1) {
                // Mean-reverting: fade direction signals
                hurstContrib = -(0.5 - results_.hurstExponent) * 2.0;
            }
            alignment += std::max(-1.0, std::min(1.0, hurstContrib));
            alignCount++;
        }

        // 3. ACF alignment: positive ACF endorses momentum signal
        if (acf_.Count() >= 30) {
            double acfContrib = results_.autocorrLag1 * dir;  // ACF × direction
            alignment += std::max(-1.0, std::min(1.0, acfContrib * 3.0));
            alignCount++;
        }

        // 4. Volatility gate: extreme vol reduces conviction
        double volPenalty = 1.0;
        switch (results_.volRegime) {
            case 0: volPenalty = 1.10; break;  // LOW: slightly amplify (thin market momentum)
            case 1: volPenalty = 1.00; break;  // NORMAL: no change
            case 2: volPenalty = 0.75; break;  // HIGH: reduce 25%
            case 3: volPenalty = 0.40; break;  // EXTREME: reduce 60%
        }

        // 5. Flash crash precursor overrides
        if (results_.flashCrashPrecursor) {
            volPenalty *= 0.20;  // reduce to 20% during flash crash precursor
        }

        // Average alignment
        double alignScore = (alignCount > 0) ? alignment / alignCount : 0.0;
        alignScore = std::max(-1.0, std::min(1.0, alignScore));

        // Apply to raw score
        double adjusted = rawScore * (1.0 + 0.5 * alignScore) * volPenalty;
        adjusted = std::max(-1.0, std::min(1.0, adjusted));

        results_.regimeAdjustedScore = adjusted;

        // Direction with enhanced threshold (tighter in high-vol)
        double thresh = 0.18 * (1.0 + 0.5 * (results_.volRegime == 3 ? 1.0 : 0.0));
        if      (adjusted >  thresh) results_.regimeAdjustedDir = +1;
        else if (adjusted < -thresh) results_.regimeAdjustedDir = -1;
        else                         results_.regimeAdjustedDir =  0;

        // Regime confidence: how well all indicators agree
        double maxHMM  = std::max(results_.hmmBullProb, results_.hmmBearProb);
        double hurstC  = results_.hurstReliable ? std::abs(results_.hurstExponent - 0.5) * 4.0 : 0.0;
        results_.regimeConfidence = std::max(0.0, std::min(1.0,
            0.40 * maxHMM +
            0.20 * std::min(1.0, hurstC) +
            0.20 * std::min(1.0, std::abs(results_.autocorrLag1) * 5.0) +
            0.20 * hitRate));

        // Edge score: proxy for expected profitability per trade (bps)
        // Higher = better opportunity after accounting for costs and regime
        double vpinDamper  = std::max(0.0, 1.0 - vpin * 1.5);
        results_.edgeScore = std::abs(results_.regimeAdjustedScore)
                            * 10.0
                            * hitRate
                            * vpinDamper
                            * volPenalty
                            * results_.regimeConfidence;
    }

    YangZhangVolatilityEstimator yzVol_;
    SpreadRegimeTick             spread_;
    MidReturnAutocorrelation     acf_;
    HurstExponentRS              hurst_;
    OnlineHMMFilter              hmm_;
    RegimeResults                results_;
};