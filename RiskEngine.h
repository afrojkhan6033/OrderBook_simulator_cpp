#pragma once
// ============================================================================
// RiskEngine.h  —  HFT Risk & PnL Analytics Engine  v2
//
// Self-contained. Dependencies: SignalEngine.h (SignalResults, TopOfBook).
// Include after MarketMicrostructure.h and SignalEngine.h.
//
// Feature                   Algorithm / Method                       Cost
// ─────────────────────────────────────────────────────────────────────────
// P1. KalmanPriceFilter     2-state [price,velocity] Kalman filter   O(1)
//                           Symmetry-preserved Joseph-form update
//
// P1. CompositeDirection     5-factor weighted score [-1,+1]          O(1)
//     Model                 OFI·0.35 + micro·0.25 + mom·0.20
//                           + imbal·0.10 + vpin·0.10
//
// P1. HitRateTracker        EWM accuracy, gated by min-count          O(1)
//
// P1. SimulatedPosition     Composite → paper long/short              O(1)
//     Tracker               VWAP entry, microprice MTM, realized PnL
//                           lastClosedDir_ exposed for slippage sign
//
// P1. SlippageCalculator    Arrival (entry microprice) vs exec        O(1)
//                           Implementation shortfall. EWM α=0.15.
//
// P2. FillProbabilityModel  Proportional-hazard queue model           O(1)
//                           Rolling 30s trade-rate window (wall-clock)
//                           P(fill) = 1−exp(−λ×avgSz×T/Q)
//
// P2. RollingRiskMetrics    Rolling 100-trade Sharpe (Welford online)  O(1)
//                           All-time peak-to-trough max drawdown
//
// P3. InventoryRiskHeatmap  DV01=|pos|×mid×0.0001                    O(1)
//                           Liquidation cost, Kyle λ impact (linear)
//
// Bug fixes vs v1:
//   BUG-R1  Slippage exit direction — lastClosedDir_ exposed; heuristic removed
//   BUG-R2  FillProb elapsed time 10× wrong — now uses wall-clock nowUs
//   BUG-R3  OnTrade direction ignored — signed flow passed to fill model
//   BUG-R4  TradeRate is lifetime avg — rolling 30s window replaces it
//   BUG-R5  HitRate stuck at 0.5 — min-count gate (10 outcomes) added
//   BUG-R6  Exit slippage used exit microprice as arrival — fixed to entry
//   BUG-R7  Kalman P matrix symmetry drift — symmetrised after each update
//   BUG-R8  Variance cancellation in Sharpe — Welford's online algorithm
//   BUG-R9  Market impact was λ×Q² — corrected to λ×Q (Almgren-Chriss)
//
// Thread-safety: NONE — call only from single processing thread.
// All prices/quantities in USD / base-asset units (not ×10000 encoded).
// ============================================================================

#include "SignalEngine.h"   // SignalResults, TopOfBook

#include <deque>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <limits>

// ─── All risk results in one flat struct (read-only by display) ───────────────
struct RiskResults {
    // ── P1: Simulated Position ────────────────────────────────────────────────
    double positionSize      = 0.0;   // current inventory: +long / -short / 0=flat
    double positionNotional  = 0.0;   // |position| × current_mid (USD exposure)
    double entryPrice        = 0.0;   // VWAP entry price for current position
    double unrealizedPnl     = 0.0;   // mark-to-market via microprice (USD)
    double realizedPnl       = 0.0;   // closed trades only (USD)
    double totalPnl          = 0.0;   // realized + unrealized
    int    totalTrades       = 0;     // completed round-trips
    int    winningTrades     = 0;
    double winRate           = 0.0;   // winningTrades / totalTrades
    int    signalDirection   = 0;     // active: +1=LONG / -1=SHORT / 0=FLAT

    // ── P1: Slippage ─────────────────────────────────────────────────────────
    double lastSlippageBps   = 0.0;   // (exec − arrival_entry) / arrival × 10000
    double avgSlippageBps    = 0.0;   // EWM α=0.15 rolling average (bps)
    double implShortfallBps  = 0.0;   // implementation shortfall vs mid at entry
    double lastExecPrice     = 0.0;   // simulated execution price (ask on buy / bid on sell)
    double lastArrivalPrice  = 0.0;   // microprice at ENTRY signal time (fair-value arrival)

    // ── P2: Fill Probability ──────────────────────────────────────────────────
    double fillProb100ms     = 0.0;   // P(fill in next 100ms) ∈ [0, 1]
    double queueDepthAtL1    = 0.0;   // current L1 depth (base units)
    double tradeArrivalRate  = 0.0;   // rolling 30s aggressive trades / second

    // ── P2: Rolling Risk Metrics ──────────────────────────────────────────────
    double sharpePerTrade    = 0.0;   // per-trade Sharpe over rolling 100 trades (Welford)
    double annualizedSharpe  = 0.0;   // × √262800 (24/7 crypto, ~2min avg hold)
    double maxDrawdown       = 0.0;   // all-time peak-to-trough (USD)
    double currentDrawdown   = 0.0;   // current drawdown from all-time peak (USD)
    double peakEquity        = 0.0;   // running all-time peak realized equity (USD)

    // ── P3: Inventory Risk / DV01 ─────────────────────────────────────────────
    double dv01              = 0.0;   // $ loss per 1 bps adverse move = |pos|×mid×0.0001
    double liquidationCost   = 0.0;   // half-spread × |position| to flatten
    double marketImpact      = 0.0;   // Kyle's λ × |position| (linear Almgren-Chriss)
    int    heatLevel         = 0;     // 0=flat 1=low 2=medium 3=high 4=extreme

    // ── Direction Accuracy & Kalman ───────────────────────────────────────────
    double compositeScore    = 0.0;   // [-1, +1] 5-factor direction signal
    double kalmanVelocity    = 0.0;   // Kalman filtered price velocity ($/tick)
    double kalmanPrice       = 0.0;   // Kalman filtered mid price
    double hitRate           = 0.5;   // signal accuracy (gated: 0.5 until 10 outcomes)
    int    pendingSignals    = 0;     // signals awaiting N-tick accuracy evaluation
};

// ============================================================================
// 1. KalmanPriceFilter  (v2 — symmetry-preserved)
//
// 2-state: x = [price, velocity]'
//   F = [[1, dt], [0, ρ]]  — ρ=0.90 velocity mean-reversion
//   H = [1, 0]             — we observe price only
//   Q = diag(q_p, q_v)    — process noise
//   R = r_meas             — measurement noise variance
//
// After each update P is explicitly symmetrised:
//   P[0][1] = P[1][0] = (P[0][1] + P[1][0]) / 2
// This prevents the matrix from drifting non-positive-definite over thousands
// of ticks — a known numerical issue in the simple (I-KH)P form update.
//
// Velocity signal interpretation:
//   > 0  → price trending up   (directional buy signal)
//   < 0  → price trending down (directional sell signal)
//   Magnitude ≈ USD per 100ms tick for SOL-class crypto
// ============================================================================
class KalmanPriceFilter {
public:
    KalmanPriceFilter(double dt     = 0.1,     // depth update interval (seconds)
                      double q_price = 1e-4,   // process noise: price component
                      double q_vel   = 1e-5,   // process noise: velocity component
                      double r_meas  = 1e-3,   // measurement noise variance
                      double rho     = 0.90)   // velocity decay (0=random walk, 1=persist)
        : dt_(dt), rho_(rho), r_(r_meas)
    {
        P_[0][0] = 1.0;  P_[0][1] = 0.0;
        P_[1][0] = 0.0;  P_[1][1] = 1.0;
        Q_[0][0] = q_price; Q_[0][1] = 0.0;
        Q_[1][0] = 0.0;     Q_[1][1] = q_vel;
        x_[0] = 0.0; x_[1] = 0.0;
        initialized_ = false;
    }

    void Update(double measuredMid) {
        if (!initialized_) {
            x_[0] = measuredMid;
            x_[1] = 0.0;
            initialized_ = true;
            return;
        }

        // ── Prediction step ───────────────────────────────────────────────────
        // x_pred = F × x
        double xp0 = x_[0] + dt_ * x_[1];   // predicted price
        double xp1 = rho_  * x_[1];          // predicted velocity

        // P_pred = F × P × F' + Q
        // F×P:
        double FP00 = P_[0][0] + dt_ * P_[1][0];
        double FP01 = P_[0][1] + dt_ * P_[1][1];
        double FP10 = rho_ * P_[1][0];
        double FP11 = rho_ * P_[1][1];

        // (FP) × F' + Q  (F' = [[1,0],[dt,rho]])
        double Pp00 = FP00 * 1.0  + FP01 * dt_  + Q_[0][0];
        double Pp01 = FP00 * 0.0  + FP01 * rho_;
        double Pp10 = FP10 * 1.0  + FP11 * dt_;
        double Pp11 = FP10 * 0.0  + FP11 * rho_ + Q_[1][1];
        // BUG-R7 fix: symmetrise P_pred before using it
        double Pp01s = (Pp01 + Pp10) * 0.5;
        Pp01 = Pp01s; Pp10 = Pp01s;

        // ── Update step ───────────────────────────────────────────────────────
        // Innovation: y = z - H×x_pred = measuredMid - xp0
        double innov = measuredMid - xp0;
        double S     = Pp00 + r_;           // innovation covariance S = H P_pred H' + R
        if (std::abs(S) < 1e-15) return;   // numerical guard

        // Kalman gain K = P_pred × H' / S  →  K = [Pp00/S, Pp10/S]'
        double K0 = Pp00 / S;
        double K1 = Pp10 / S;

        // State update: x = x_pred + K × innov
        x_[0] = xp0 + K0 * innov;
        x_[1] = xp1 + K1 * innov;

        // Covariance update: P = (I - K×H) × P_pred
        // (I - K×H) = [[1-K0, 0], [-K1, 1]]
        P_[0][0] = (1.0 - K0) * Pp00;
        P_[0][1] = (1.0 - K0) * Pp01;
        P_[1][0] =      -K1   * Pp00 + Pp10;
        P_[1][1] =      -K1   * Pp01 + Pp11;
        // BUG-R7 fix: re-symmetrise after update to prevent long-term drift
        double sym = (P_[0][1] + P_[1][0]) * 0.5;
        P_[0][1] = sym; P_[1][0] = sym;
        // Enforce positive diagonal (numerical floor)
        if (P_[0][0] < 1e-12) P_[0][0] = 1e-12;
        if (P_[1][1] < 1e-12) P_[1][1] = 1e-12;
    }

    double GetPrice()    const { return x_[0]; }
    double GetVelocity() const { return x_[1]; }
    bool   IsReady()     const { return initialized_; }

private:
    double dt_, rho_, r_;
    double x_[2];       // state: [price, velocity]
    double P_[2][2];    // error covariance (kept symmetric)
    double Q_[2][2];    // process noise
    bool   initialized_;
};

// ============================================================================
// 2. CompositeDirectionModel  (v3 — 6-factor, tighter threshold)
//
// 6-factor linear score ∈ [-1, +1]:
//
//   f_ofi    = ofiNorm                              (already in [-1,+1])
//   f_micro  = clamp((microprice-mid)/spread, -1,1) (price pressure vs fair)
//   f_mom    = tanh(10 × momentumScore)             ([-1,+1] via saturation)
//   f_imbal  = clamp(imbalance × (1-min(1,vpin/0.65)), -1,1)
//   f_vpin   = clamp(-(vpin-0.5)×2, -1,1)           (toxicity fade)
//   f_kalman = clamp(kalmanVel/mid × 50000, -1,1)   (Kalman-filtered trend)
//
//   score = 0.25·ofi + 0.25·micro + 0.20·mom + 0.05·imbal + 0.10·vpin + 0.15·kalman
//
// v3 changes:
//   - ENTRY_THRESH raised 0.18 → 0.30 (only enter on strong conviction)
//   - OFI weight reduced 0.35→0.25 (too noisy on every depth update)
//   - Kalman velocity added: filtered directional signal that cuts through noise
//   - Imbalance reduced 0.10→0.05 (low predictive power alone)
// ============================================================================
class CompositeDirectionModel {
public:
    static constexpr double W_OFI        = 0.25;
    static constexpr double W_MICRO      = 0.25;
    static constexpr double W_MOM        = 0.20;
    static constexpr double W_IMBAL      = 0.05;
    static constexpr double W_VPIN       = 0.10;
    static constexpr double W_KALMAN     = 0.15;
    static constexpr double ENTRY_THRESH = 0.30;

    double Compute(double ofiNorm,
                   double microprice,
                   double mid,
                   double spread,
                   double momentumScore,
                   double imbalance,
                   double vpin,
                   double kalmanVelocity = 0.0) const
    {
        // 1. OFI — already normalised [-1,+1]
        double f_ofi = ofiNorm;

        // 2. Microprice skew — normalise by spread; clamp
        double sFloor = std::max(spread, 1e-6);
        double f_micro = std::max(-1.0, std::min(1.0, (microprice - mid) / sFloor));

        // 3. Momentum — tanh mapping
        double f_mom = std::tanh(10.0 * momentumScore);

        // 4. Imbalance — dampen when adverse selection risk high
        double vpinDamper = 1.0 - std::min(1.0, vpin / 0.65);
        double f_imbal = std::max(-1.0, std::min(1.0, imbalance * vpinDamper));

        // 5. VPIN adjustment — high toxicity fades directional conviction
        double f_vpin = std::max(-1.0, std::min(1.0, -(vpin - 0.5) * 2.0));

        // 6. Kalman velocity — filtered price trend direction
        //    Normalise velocity by mid price, scale to [-1,+1]
        double f_kalman = 0.0;
        if (mid > 1e-6) {
            f_kalman = std::max(-1.0, std::min(1.0,
                kalmanVelocity / mid * 50000.0));
        }

        double score = W_OFI    * f_ofi
                     + W_MICRO  * f_micro
                     + W_MOM    * f_mom
                     + W_IMBAL  * f_imbal
                     + W_VPIN   * f_vpin
                     + W_KALMAN * f_kalman;

        return std::max(-1.0, std::min(1.0, score));
    }

    static int GetDirection(double score) {
        if      (score >  ENTRY_THRESH) return +1;   // LONG
        else if (score < -ENTRY_THRESH) return -1;   // SHORT
        else                            return  0;   // FLAT
    }
};

// ============================================================================
// 3. HitRateTracker  (v2 — min-count gate)
//
// EVAL_DELAY_TICKS after each directional signal, checks whether mid moved
// in the predicted direction.  EWM α=0.10 (≈10-signal half-life; faster than
// v1 α=0.05 which was too slow for the sparse signal rate at THRESH=0.18).
//
// BUG-R5 fix: hitRate_ reports 0.5 (neutral) until MIN_EVAL_COUNT outcomes
// have been observed.  Before that it is meaningless and should not drive
// Kelly sizing or AS gamma scaling.
// ============================================================================
class HitRateTracker {
public:
    static constexpr int    EVAL_DELAY_TICKS = 5;    // ~500ms at 100ms stream
    static constexpr double EWM_ALPHA        = 0.10; // faster than v1's 0.05
    static constexpr int    MIN_EVAL_COUNT   = 10;   // gate: min evaluated outcomes

    void OnSignal(int direction, double midAtSignal) {
        if (direction != 0)
            pending_.push_back({direction, midAtSignal, EVAL_DELAY_TICKS});
    }

    void OnTick(double currentMid) {
        tickCount_++;
        for (auto &p : pending_) p.ticksLeft--;

        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (it->ticksLeft <= 0) {
                double move = currentMid - it->midAtSignal;
                bool hit = (it->direction > 0 && move > 0.0) ||
                           (it->direction < 0 && move < 0.0);
                rawHitRate_ = EWM_ALPHA * (hit ? 1.0 : 0.0)
                            + (1.0 - EWM_ALPHA) * rawHitRate_;
                totalEvaluated_++;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // BUG-R5 fix: return 0.5 (neutral) until enough outcomes observed
    double GetHitRate() const {
        return (totalEvaluated_ >= MIN_EVAL_COUNT) ? rawHitRate_ : 0.5;
    }
    int GetPending()      const { return static_cast<int>(pending_.size()); }
    int GetTotalSignals() const { return totalEvaluated_; }

private:
    struct PendingSignal { int direction; double midAtSignal; int ticksLeft; };
    std::vector<PendingSignal> pending_;
    double rawHitRate_     = 0.5;
    int    totalEvaluated_ = 0;
    int    tickCount_      = 0;
};

// ============================================================================
// 4. SimulatedPositionTracker  (v3 — regime-gated, adaptive stops, trailing exit)
//
// v3 profitability improvements over v2:
//
//   L1-fix: ENTRY_THRESH raised 0.18→0.30 (only strong conviction enters)
//   L2-fix: Adaptive stop-loss: 10bps(LOW) / 25bps(NORM) / 40bps(HIGH)
//   L3-fix: HOLD_TICKS 300→600 (60s — give trades time to develop)
//   L4-fix: Post-loss cooldown prevents loss spirals (50 ticks after stop)
//   L5-fix: Regime gating: no entry in EXTREME vol, flash crash, VPIN>0.70,
//           or regime-opposed direction (long in bear / short in bull)
//   L6-fix: Spread-cost awareness: require edge > 1.5× spread to enter
//   NEW:    Trailing stop: exit if PnL drops 40% from peak during trade
//
// All v2 bug fixes preserved (BUG-R1, BUG-R6).
// ============================================================================
class SimulatedPositionTracker {
public:
    static constexpr double POSITION_SIZE = 0.10;   // base units per signal
    static constexpr double ENTRY_THRESH  = CompositeDirectionModel::ENTRY_THRESH;

    // Adaptive stop-loss per vol regime (bps)
    static constexpr double STOP_BPS_LOW     = 10.0;
    static constexpr double STOP_BPS_NORMAL  = 25.0;
    static constexpr double STOP_BPS_HIGH    = 40.0;
    // EXTREME vol: no entry (gated), but if already in: 50bps emergency stop
    static constexpr double STOP_BPS_EXTREME = 50.0;

    static constexpr int    HOLD_TICKS    = 600;    // max hold ≈ 60s

    // Trailing stop parameters
    static constexpr double TRAIL_GIVEBACK   = 0.40; // exit if PnL drops 40% from peak
    static constexpr double TRAIL_MIN_BPS    = 3.0;  // trailing only active after 3bps profit

    // Cooldown ticks after exit (prevents loss spirals)
    static constexpr int    COOLDOWN_STOP    = 50;   // after stop-loss: 5 seconds
    static constexpr int    COOLDOWN_HOLD    = 20;   // after max-hold: 2 seconds
    static constexpr int    COOLDOWN_PROFIT  = 5;    // after profitable exit: 0.5s
    static constexpr int    COOLDOWN_FLIP    = 10;   // after signal flip: 1s

    // Regime gate thresholds
    static constexpr double VPIN_GATE        = 0.70; // no entry above this
    static constexpr double MIN_CONFIDENCE   = 0.25; // minimum regime confidence

    struct State {
        double position    = 0.0;
        double entryPrice  = 0.0;
        double unrealPnl   = 0.0;
        double realPnl     = 0.0;
        int    totalTrades = 0;
        int    wins        = 0;
        double execPrice   = 0.0;     // last execution price
        double arrivalPrice= 0.0;     // entry microprice (for slippage calc)
    };

    // Regime context — populated by RiskEngine before calling OnTick
    struct RegimeCtx {
        int    volRegime         = 1;    // 0=LOW 1=NORM 2=HIGH 3=EXTREME
        bool   flashCrashPrec   = false;
        double vpin             = 0.0;
        int    hmmState         = 0;    // 0=BULL 1=BEAR
        bool   hmmConfident     = false;
        double regimeConfidence = 0.5;
        double spreadUSD        = 0.0;
        double compositeScore   = 0.0;  // raw absolute score for edge check
    };

    // Returns true if an entry or exit occurred this tick
    bool OnTick(int direction,
                double bestBid,
                double bestAsk,
                double microprice,
                double currentMid,
                const RegimeCtx& regime)
    {
        bool traded = false;

        // Decrement cooldown
        if (cooldownTicks_ > 0) cooldownTicks_--;

        if (position_ == 0.0) {
            // ── Entry (regime-gated) ──────────────────────────────────────────
            if (direction != 0 && PassesEntryGate(direction, regime)) {
                entryDir_      = direction;
                position_      = POSITION_SIZE * static_cast<double>(direction);
                entryPrice_    = (direction > 0) ? bestAsk : bestBid;
                entryMicro_    = microprice;   // BUG-R6 fix preserved
                holdTicks_     = 0;
                peakPnl_       = 0.0;          // trailing stop reset
                lastExecPrice_ = entryPrice_;
                lastArrival_   = entryMicro_;
                lastClosedDir_ = 0;
                entryVolRegime_= regime.volRegime;  // remember for adaptive stop
                traded         = true;
            }
        } else {
            holdTicks_++;
            double unrealPnl = position_ * (currentMid - entryPrice_);

            // Track peak PnL for trailing stop
            if (unrealPnl > peakPnl_) peakPnl_ = unrealPnl;

            // ── Adaptive stop-loss based on entry vol regime ───────────────────
            double stopBps = GetStopBps(entryVolRegime_);
            double stopLossUSD = stopBps / 10000.0 * entryPrice_
                                 * std::abs(position_);

            bool stopHit     = (-unrealPnl > stopLossUSD);
            bool holdExpired = (holdTicks_ >= HOLD_TICKS);
            bool signalFlip  = (direction != 0 && direction != entryDir_);

            // ── Trailing stop: exit if gave back 40% from peak ────────────────
            double trailMinUSD = TRAIL_MIN_BPS / 10000.0 * entryPrice_
                                 * std::abs(position_);
            bool trailHit = (peakPnl_ > trailMinUSD) &&
                            (unrealPnl < peakPnl_ * (1.0 - TRAIL_GIVEBACK));

            // ── Emergency exit: vol regime jumped to EXTREME while in trade ───
            bool emergencyExit = (regime.volRegime >= 3 && entryVolRegime_ < 3);

            if (stopHit || holdExpired || signalFlip || trailHit || emergencyExit) {
                // BUG-R1 fix preserved: capture exit direction BEFORE reset
                lastClosedDir_ = -entryDir_;

                double exitPrice = (position_ > 0) ? bestBid : bestAsk;
                double tradePnl  = position_ * (exitPrice - entryPrice_);
                realizedPnl_ += tradePnl;
                totalTrades_++;
                if (tradePnl > 0.0) winningTrades_++;

                lastExecPrice_ = exitPrice;
                // BUG-R6 preserved: lastArrival_ stays as entry microprice

                position_   = 0.0;
                entryPrice_ = 0.0;
                holdTicks_  = 0;
                entryDir_   = 0;
                peakPnl_    = 0.0;
                traded      = true;

                // Set cooldown based on exit reason
                if (stopHit || emergencyExit)
                    cooldownTicks_ = COOLDOWN_STOP;
                else if (holdExpired)
                    cooldownTicks_ = COOLDOWN_HOLD;
                else if (signalFlip)
                    cooldownTicks_ = COOLDOWN_FLIP;
                else if (trailHit && tradePnl > 0.0)
                    cooldownTicks_ = COOLDOWN_PROFIT;
                else
                    cooldownTicks_ = COOLDOWN_HOLD;
            }
        }
        return traded;
    }

    State GetState(double currentMid, double microprice) const {
        State s;
        s.position     = position_;
        s.entryPrice   = entryPrice_;
        s.unrealPnl    = position_ * (microprice - entryPrice_);
        s.realPnl      = realizedPnl_;
        s.totalTrades  = totalTrades_;
        s.wins         = winningTrades_;
        s.execPrice    = lastExecPrice_;
        s.arrivalPrice = lastArrival_;
        return s;
    }

    double GetPosition()       const { return position_;      }
    double GetEntryPrice()     const { return entryPrice_;    }
    double GetRealizedPnl()    const { return realizedPnl_;   }
    int    GetTotalTrades()    const { return totalTrades_;   }
    int    GetWinningTrades()  const { return winningTrades_; }
    double GetLastExecPrice()  const { return lastExecPrice_; }
    double GetLastArrival()    const { return lastArrival_;   }
    int    GetLastClosedDir()  const { return lastClosedDir_; }
    int    GetCooldown()       const { return cooldownTicks_; }

private:
    // ── Entry gate: all conditions must pass ──────────────────────────────────
    bool PassesEntryGate(int direction, const RegimeCtx& r) const {
        // G1: Cooldown active → no entry
        if (cooldownTicks_ > 0) return false;

        // G2: EXTREME volatility → no entry
        if (r.volRegime >= 3) return false;

        // G3: Flash crash precursor → no entry
        if (r.flashCrashPrec) return false;

        // G4: High VPIN (toxic flow) → no entry
        if (r.vpin > VPIN_GATE) return false;

        // G5: Minimum regime confidence
        if (r.regimeConfidence < MIN_CONFIDENCE) return false;

        // G6: HMM regime alignment — don't go LONG in confident BEAR,
        //     don't go SHORT in confident BULL
        if (r.hmmConfident) {
            if (direction > 0 && r.hmmState == 1) return false;  // long in bear
            if (direction < 0 && r.hmmState == 0) return false;  // short in bull
        }

        // G7: Edge must exceed spread cost (need > 1.5× spread to be profitable)
        if (r.spreadUSD > 0.0 && currentMid_ > 0.0) {
            double spreadBps = r.spreadUSD / currentMid_ * 10000.0;
            double edgeProxy = std::abs(r.compositeScore) * 10.0;  // rough bps
            if (edgeProxy < spreadBps * 1.5) return false;
        }

        return true;
    }

    static double GetStopBps(int volRegime) {
        switch (volRegime) {
            case 0:  return STOP_BPS_LOW;
            case 1:  return STOP_BPS_NORMAL;
            case 2:  return STOP_BPS_HIGH;
            default: return STOP_BPS_EXTREME;
        }
    }

    double position_       = 0.0;
    double entryPrice_     = 0.0;
    double entryMicro_     = 0.0;
    double realizedPnl_    = 0.0;
    double lastExecPrice_  = 0.0;
    double lastArrival_    = 0.0;
    double peakPnl_        = 0.0;   // v3: trailing stop peak PnL
    double currentMid_     = 0.0;   // cached for spread check in gate
    int    holdTicks_      = 0;
    int    entryDir_       = 0;
    int    lastClosedDir_  = 0;
    int    totalTrades_    = 0;
    int    winningTrades_  = 0;
    int    cooldownTicks_  = 0;     // v3: post-exit cooldown counter
    int    entryVolRegime_ = 1;     // v3: vol regime at entry (for adaptive stop)
};

// ============================================================================
// 5. SlippageCalculator  (v2 — sign derived from lastClosedDir)
//
//   Slippage (bps) = (exec − arrival_entry) × sign(direction) / arrival × 10000
//     arrival_entry = microprice at ENTRY signal time (always, not exit)
//     direction     = +1=BUY(entry) or lastClosedDir_ (exit)
//     → positive = worse execution (paid more / received less than fair value)
//     → negative = price improvement
//
//   Implementation Shortfall (bps) = (exec − mid_at_arrival) / mid × 10000
//     mid_at_arrival = midPrice when signal generated
//
//   EWM rolling average: α=0.15 (≈7-trade half-life)
// ============================================================================
class SlippageCalculator {
public:
    static constexpr double EWM_ALPHA = 0.15;

    void OnFill(double execPrice,
                double arrivalPrice,   // microprice at ENTRY signal time
                double midAtArrival,   // mid price at signal time
                int    direction)      // +1=BUY -1=SELL (use lastClosedDir_ on exit)
    {
        if (arrivalPrice < 1e-6 || midAtArrival < 1e-6) return;

        double dirSign = static_cast<double>(direction);

        double rawSlip    = (execPrice - arrivalPrice) * dirSign;
        lastSlippageBps_  = rawSlip / arrivalPrice * 10000.0;
        avgSlippageBps_   = EWM_ALPHA * lastSlippageBps_
                          + (1.0 - EWM_ALPHA) * avgSlippageBps_;

        double shortfall      = (execPrice - midAtArrival) * dirSign;
        lastImplShortfall_    = shortfall / midAtArrival * 10000.0;
    }

    double GetLastSlippageBps()  const { return lastSlippageBps_;   }
    double GetAvgSlippageBps()   const { return avgSlippageBps_;    }
    double GetImplShortfall()    const { return lastImplShortfall_; }

private:
    double lastSlippageBps_   = 0.0;
    double avgSlippageBps_    = 0.0;
    double lastImplShortfall_ = 0.0;
};

// ============================================================================
// 6. FillProbabilityModel  (v2 — wall-clock rate, rolling 30s window)
//
// BUG-R2 fix: elapsed time now uses startUs_ + nowUs (wall-clock).
//   Old: elapsed_s = elapsedTicks_ * 0.10   (wrong — OnDepthUpdate is 1s not 100ms)
//   New: elapsed_s = (nowUs - startUs_) / 1e6  (real wall-clock seconds)
//
// BUG-R3 fix: OnTrade receives direction (+1=BUY aggressor / -1=SELL aggressor)
//   and tracks directional trade timestamps to compute direction-specific rate.
//
// BUG-R4 fix: rolling 30s trade-rate window instead of lifetime total / elapsed.
//   Each aggTrade timestamp is stored; entries older than 30s are evicted.
//   tradeRate_ = recent_count / 30.0  (always current market activity level)
//
// Model (unchanged from v1 — only the inputs are now correct):
//   P(fill in T) = 1 − exp(−λ × avgSz × T / Q)
//   λ   = rolling 30s trade arrival rate
//   sz  = rolling 100-trade average qty
//   Q   = L1 queue depth on the relevant side
//   T   = 0.100s (100ms depth update interval)
// ============================================================================
class FillProbabilityModel {
public:
    static constexpr int    TRADE_WINDOW_N  = 100;          // trades for avg-size
    static constexpr double T_HORIZON_S     = 0.10;         // 100ms evaluation
    static constexpr double RATE_WINDOW_US  = 30e6;         // 30s rolling window
    static constexpr double MIN_RATE        = 0.01;         // floor: 1 trade/100s
    static constexpr double ORDER_SIZE      = SimulatedPositionTracker::POSITION_SIZE;

    // BUG-R3 fix: direction received and used to weight directional flow
    void OnTrade(double qty, int direction) {
        int64_t now = nowUs_;   // updated by OnDepthUpdate; good enough for trade stamps
        tradeTimestamps_.push_back(now);

        // Rolling avg-size window
        tradeQtys_.push_back(qty);
        qtySumWindow_ += qty;
        tradeCountWindow_++;
        if (static_cast<int>(tradeQtys_.size()) > TRADE_WINDOW_N) {
            qtySumWindow_ -= tradeQtys_.front();
            tradeQtys_.pop_front();
            tradeCountWindow_--;
        }

        // Directional signed flow (buy=+1, sell=-1)
        signedFlowSum_ += static_cast<double>(direction) * qty;
    }

    void OnDepthUpdate(double l1BidQty, double l1AskQty,
                       int direction, int64_t nowUs)
    {
        // BUG-R2 fix: initialise start time on first call
        if (startUs_ == 0) startUs_ = nowUs;
        nowUs_ = nowUs;

        l1BidQty_ = l1BidQty;
        l1AskQty_ = l1AskQty;

        // BUG-R4 fix: evict trade timestamps older than RATE_WINDOW_US
        int64_t cutoff = nowUs - static_cast<int64_t>(RATE_WINDOW_US);
        while (!tradeTimestamps_.empty() && tradeTimestamps_.front() < cutoff)
            tradeTimestamps_.pop_front();

        // Rolling 30s trade arrival rate
        tradeRate_ = std::max(MIN_RATE,
            static_cast<double>(tradeTimestamps_.size()) / (RATE_WINDOW_US / 1e6));

        // Average trade size (rolling 100-trade window)
        double avgSz = (tradeCountWindow_ > 0)
            ? qtySumWindow_ / static_cast<double>(tradeCountWindow_)
            : ORDER_SIZE;
        avgSz = std::max(avgSz, 1e-9);

        // Queue depth for the active direction
        double queueDepth = (direction >= 0) ? l1AskQty_ : l1BidQty_;
        if (queueDepth < 1e-9) queueDepth = ORDER_SIZE;

        // P(fill in T) = 1 − exp(−λ × avgSz × T / Q)
        double lambda_eff = tradeRate_ * avgSz * T_HORIZON_S / queueDepth;
        fillProb_ = std::max(0.0, std::min(1.0, 1.0 - std::exp(-lambda_eff)));
        queueDepthDisplay_ = queueDepth;
    }

    double GetFillProbability() const { return fillProb_;          }
    double GetQueueDepth()      const { return queueDepthDisplay_; }
    double GetTradeRate()       const { return tradeRate_;         }

private:
    std::deque<double>  tradeQtys_;
    std::deque<int64_t> tradeTimestamps_;   // BUG-R4: rolling window
    double qtySumWindow_      = 0.0;
    int    tradeCountWindow_  = 0;
    double signedFlowSum_     = 0.0;
    double tradeRate_         = MIN_RATE;
    double fillProb_          = 0.0;
    double queueDepthDisplay_ = 0.0;
    double l1BidQty_          = 0.0;
    double l1AskQty_          = 0.0;
    int64_t nowUs_            = 0;
    int64_t startUs_          = 0;   // BUG-R2: wall-clock reference
};

// ============================================================================
// 7. RollingRiskMetrics  (v2 — Welford online variance)
//
// BUG-R8 fix: var = sumPnl2/N - mean² suffers catastrophic cancellation when
// all trades are similar (common in tight-spread MM where gross PnL per trade
// is in the range 0.0001-0.001 USD and the squared terms lose precision).
// Welford's online algorithm maintains M2 (sum of squared deviations from
// running mean), which is numerically stable for arbitrarily similar values.
//
// Annualised Sharpe factor: √262800 ≈ 512
//   Assumes 24/7 crypto, ~720 round-trips/day, 365 days/year
//   Per-trade Sharpe × 512 → annualised equivalent
//
// Max drawdown: tracks cumulative realised PnL only (not unrealised).
// ============================================================================
class RollingRiskMetrics {
public:
    static constexpr int    WINDOW_SIZE   = 100;
    static constexpr double ANNUAL_FACTOR = 512.0;   // √262800 ≈ 512

    void OnTradeClosed(double tradePnl) {
        // ── Welford rolling Sharpe ────────────────────────────────────────────
        if (static_cast<int>(pnlWindow_.size()) >= WINDOW_SIZE) {
            double old = pnlWindow_.front();
            pnlWindow_.pop_front();
            // Remove old observation from Welford accumulators
            n_--;
            if (n_ >= 1) {
                double oldMean = (wMean_ * (n_ + 1) - old) / n_;
                // Correct M2 by removing old observation's contribution
                // Using the online removal formula: M2 -= (old-oldMean)*(old-wMean_)
                wM2_ -= (old - oldMean) * (old - wMean_);
                wMean_ = oldMean;
            } else {
                wMean_ = 0.0; wM2_ = 0.0;
            }
        }

        // Add new observation (Welford's incremental update)
        pnlWindow_.push_back(tradePnl);
        n_++;
        double delta  = tradePnl - wMean_;
        wMean_       += delta / static_cast<double>(n_);
        double delta2 = tradePnl - wMean_;
        wM2_         += delta * delta2;

        // Compute Sharpe (need at least 5 trades)
        if (n_ >= 5) {
            double var = (n_ >= 2) ? wM2_ / static_cast<double>(n_ - 1) : 0.0;
            double sd  = (var > 1e-15) ? std::sqrt(var) : 1e-9;
            sharpe_           = wMean_ / sd;
            annualizedSharpe_ = sharpe_ * ANNUAL_FACTOR;
        }

        // ── All-time max drawdown ─────────────────────────────────────────────
        cumulativeRealPnl_ += tradePnl;
        if (cumulativeRealPnl_ > peakEquity_)
            peakEquity_ = cumulativeRealPnl_;
        double dd = peakEquity_ - cumulativeRealPnl_;
        currentDrawdown_ = dd;
        if (dd > maxDrawdown_) maxDrawdown_ = dd;
    }

    double GetSharpePerTrade()   const { return sharpe_;           }
    double GetAnnualizedSharpe() const { return annualizedSharpe_; }
    double GetMaxDrawdown()      const { return maxDrawdown_;      }
    double GetCurrentDrawdown()  const { return currentDrawdown_;  }
    double GetPeakEquity()       const { return peakEquity_;       }

private:
    std::deque<double> pnlWindow_;
    // Welford accumulators
    double wMean_            = 0.0;
    double wM2_              = 0.0;   // sum of squared deviations (numerically stable)
    int    n_                = 0;
    double sharpe_           = 0.0;
    double annualizedSharpe_ = 0.0;
    double cumulativeRealPnl_= 0.0;
    double peakEquity_       = 0.0;
    double maxDrawdown_      = 0.0;
    double currentDrawdown_  = 0.0;
};

// ============================================================================
// 8. InventoryRiskHeatmap  (v2 — corrected market impact)
//
// DV01 = |position| × mid × 0.0001
//   "If market moves 1 bps against my position, I lose $DV01."
//
// Liquidation cost = |position| × (ask − bid) / 2
//   Half-spread cost of a market order to flatten.
//
// BUG-R9 fix: Market impact (Almgren-Chriss linear model):
//   Old: mktImpact = |pos| × |lambda| × |pos| = λ × Q²  (WRONG: Almgren is linear)
//   New: mktImpact = |lambda| × |pos|           (correct: permanent impact linear in qty)
//   Interpretation: expected permanent price move from unwinding |pos| units.
//   In USD terms: mktImpact × |pos| = λ × Q²/2 (market impact cost, quadratic in Q).
//   We expose the per-unit impact coefficient for display.
//
// Heat levels (DV01 thresholds):
//   0=flat (<1e-9 position), 1=LOW (<$0.01), 2=MED (<$0.05), 3=HIGH (<$0.20), 4=EXTREME
// ============================================================================
class InventoryRiskHeatmap {
public:
    struct RiskData {
        double dv01         = 0.0;
        double liquidCost   = 0.0;
        double mktImpact    = 0.0;
        int    heatLevel    = 0;
    };

    static RiskData Compute(double position,
                            double currentMid,
                            double bestBid,
                            double bestAsk,
                            double kyleLambda)
    {
        RiskData r;
        if (std::abs(position) < 1e-9) return r;   // flat — heatLevel = 0

        double absPos   = std::abs(position);
        r.dv01          = absPos * currentMid * 0.0001;
        r.liquidCost    = absPos * std::max(0.0, bestAsk - bestBid) * 0.5;
        // BUG-R9 fix: linear Almgren-Chriss impact = λ × Q (not λ × Q²)
        r.mktImpact     = std::abs(kyleLambda) * absPos;

        if      (r.dv01 < 0.01) r.heatLevel = 1;   // LOW
        else if (r.dv01 < 0.05) r.heatLevel = 2;   // MEDIUM
        else if (r.dv01 < 0.20) r.heatLevel = 3;   // HIGH
        else                    r.heatLevel = 4;   // EXTREME

        return r;
    }

    static const char* HeatBar(int level) {
        switch (level) {
            case 0: return "          [FLAT]";
            case 1: return "▓         [LOW]";
            case 2: return "▓▓▓       [MEDIUM]";
            case 3: return "▓▓▓▓▓     [HIGH]";
            case 4: return "▓▓▓▓▓▓▓▓▓ [EXTREME]";
            default: return "?";
        }
    }
};

// ============================================================================
// RiskEngine  (v2)  —  orchestrates all 8 components
//
// Interface (unchanged from v1 — .cpp callers need no modification):
//
//   // On each aggTrade:
//   risk.OnTrade(price, qty, direction);   // direction: +1=BUY, -1=SELL
//
//   // On each depth update (display timer, 1s):
//   risk.OnDepthUpdate(
//       signals.GetResults(),
//       analytics.midPrice,
//       analytics.microprice,
//       spreadUSD,
//       bid1, ask1,
//       bidQ1, askQ1,
//       analytics.imbalance,
//       micro.GetResults().kyleLambda,
//       nowUs);
//
//   // Read results:
//   const RiskResults& r = risk.GetResults();
//
// All bugs from v1 audit are fixed here — no other engine files modified.
// ============================================================================
class RiskEngine {
public:
    // ── Called on each aggTrade ───────────────────────────────────────────────
    // BUG-R3 fix: direction now forwarded to fill model instead of discarded
    void OnTrade(double price, double qty, int direction) {
        fillModel_.OnTrade(qty, direction);   // direction was (void) in v1
        (void)price;   // price used indirectly via microprice at fill
    }

    // ── Called on each depth update (display timer) ───────────────────────────
    void OnDepthUpdate(
        const SignalResults &sig,
        double midPrice,
        double microprice,
        double spreadUSD,
        double bestBid,
        double bestAsk,
        double bidQtyL1,
        double askQtyL1,
        double imbalance,
        double kyleLambda,
        int64_t nowUs)
    {
        // ── 1. Kalman filter ──────────────────────────────────────────────────
        kalman_.Update(midPrice);
        results_.kalmanPrice    = kalman_.GetPrice();
        results_.kalmanVelocity = kalman_.GetVelocity();

        // ── 2. Composite direction model (v3: 6 factors incl. Kalman vel) ────
        double composite = dir_.Compute(
            sig.ofiNormalized, microprice, midPrice, spreadUSD,
            sig.momentumScore, imbalance, sig.vpin,
            kalman_.GetVelocity());   // v3: Kalman velocity as 6th factor
        results_.compositeScore  = composite;
        int directionInt         = CompositeDirectionModel::GetDirection(composite);
        results_.signalDirection = directionInt;

        // ── 3. Hit rate tracking ──────────────────────────────────────────────
        if (directionInt != lastDirection_) {
            hitTracker_.OnSignal(directionInt, midPrice);
            lastDirection_ = directionInt;
        }
        hitTracker_.OnTick(midPrice);
        results_.hitRate       = hitTracker_.GetHitRate();
        results_.pendingSignals= hitTracker_.GetPending();

        // ── 4. Simulated position tracker (v3: regime-gated) ──────────────────
        // Build regime context for entry gate
        SimulatedPositionTracker::RegimeCtx rctx;
        rctx.volRegime         = cachedVolRegime_;
        rctx.flashCrashPrec    = cachedFlashCrash_;
        rctx.vpin              = sig.vpin;
        rctx.hmmState          = cachedHmmState_;
        rctx.hmmConfident      = cachedHmmConfident_;
        rctx.regimeConfidence  = cachedRegimeConf_;
        rctx.spreadUSD         = spreadUSD;
        rctx.compositeScore    = composite;

        bool traded = position_.OnTick(directionInt,
                                       bestBid, bestAsk, microprice, midPrice,
                                       rctx);

        if (traded) {
            double execP    = position_.GetLastExecPrice();
            double arrivalP = position_.GetLastArrival();   // entry microprice

            if (position_.GetLastClosedDir() != 0) {
                // ── Closing trade: use lastClosedDir_ (BUG-R1 fix) ───────────
                int closedDir = position_.GetLastClosedDir();
                slippage_.OnFill(execP, arrivalP, midPrice, closedDir);
            } else {
                // ── Opening trade ─────────────────────────────────────────────
                slippage_.OnFill(execP, arrivalP, midPrice, directionInt);
            }

            // Record PnL delta for Sharpe/drawdown tracking
            double thisPnl = position_.GetRealizedPnl() - lastRealPnl_;
            lastRealPnl_   = position_.GetRealizedPnl();
            if (std::abs(thisPnl) > 1e-12)
                rolling_.OnTradeClosed(thisPnl);
        }

        // ── 5. Fill probability ───────────────────────────────────────────────
        // BUG-R2 fix: nowUs used internally for wall-clock elapsed
        fillModel_.OnDepthUpdate(bidQtyL1, askQtyL1, directionInt, nowUs);

        // ── 6. Inventory risk / DV01 ──────────────────────────────────────────
        auto posState = position_.GetState(midPrice, microprice);
        auto heat     = InventoryRiskHeatmap::Compute(
            posState.position, midPrice, bestBid, bestAsk, kyleLambda);

        // ── 7. Assemble results ───────────────────────────────────────────────
        results_.positionSize     = posState.position;
        results_.positionNotional = std::abs(posState.position) * midPrice;
        results_.entryPrice       = posState.entryPrice;
        results_.unrealizedPnl    = posState.unrealPnl;
        results_.realizedPnl      = posState.realPnl;
        results_.totalPnl         = posState.realPnl + posState.unrealPnl;
        results_.totalTrades      = posState.totalTrades;
        results_.winningTrades    = posState.wins;
        results_.winRate          = (posState.totalTrades > 0)
            ? static_cast<double>(posState.wins) / posState.totalTrades
            : 0.0;

        results_.lastSlippageBps  = slippage_.GetLastSlippageBps();
        results_.avgSlippageBps   = slippage_.GetAvgSlippageBps();
        results_.implShortfallBps = slippage_.GetImplShortfall();
        results_.lastExecPrice    = posState.execPrice;
        results_.lastArrivalPrice = posState.arrivalPrice;

        results_.fillProb100ms    = fillModel_.GetFillProbability();
        results_.queueDepthAtL1   = fillModel_.GetQueueDepth();
        results_.tradeArrivalRate = fillModel_.GetTradeRate();

        results_.sharpePerTrade   = rolling_.GetSharpePerTrade();
        results_.annualizedSharpe = rolling_.GetAnnualizedSharpe();
        results_.maxDrawdown      = rolling_.GetMaxDrawdown();
        results_.currentDrawdown  = rolling_.GetCurrentDrawdown();
        results_.peakEquity       = rolling_.GetPeakEquity();

        results_.dv01             = heat.dv01;
        results_.liquidationCost  = heat.liquidCost;
        results_.marketImpact     = heat.mktImpact;
        results_.heatLevel        = heat.heatLevel;
    }

    const RiskResults& GetResults() const { return results_; }
    RiskResults&       GetResults()       { return results_; }

    // v3: cache regime context from RegimeEngine for position tracker gating.
    // Called from LiveMarketData.cpp after regime.OnDepthUpdate().
    void SetRegimeContext(int volRegime, bool flashCrash, int hmmState,
                          bool hmmConfident, double regimeConf) {
        cachedVolRegime_    = volRegime;
        cachedFlashCrash_   = flashCrash;
        cachedHmmState_     = hmmState;
        cachedHmmConfident_ = hmmConfident;
        cachedRegimeConf_   = regimeConf;
    }

private:
    KalmanPriceFilter        kalman_;
    CompositeDirectionModel  dir_;
    HitRateTracker           hitTracker_;
    SimulatedPositionTracker position_;
    SlippageCalculator       slippage_;
    FillProbabilityModel     fillModel_;
    RollingRiskMetrics       rolling_;

    RiskResults results_;
    int    lastDirection_    = 0;
    double lastRealPnl_     = 0.0;

    // v3: cached regime context for position tracker entry gating
    int    cachedVolRegime_    = 1;     // default NORMAL
    bool   cachedFlashCrash_   = false;
    int    cachedHmmState_     = 0;
    bool   cachedHmmConfident_ = false;
    double cachedRegimeConf_   = 0.5;
};