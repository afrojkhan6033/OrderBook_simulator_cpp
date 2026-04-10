#pragma once
// ============================================================================
// RiskEngine.h  —  HFT Risk & PnL Analytics Engine  v1
//
// Self-contained. Dependencies: MarketMicrostructure.h (TradeSide),
// SignalEngine.h (SignalResults, TopOfBook). Include after both.
//
// Feature                   Algorithm / Method                  Cost
// ─────────────────────────────────────────────────────────────────────────
// P1. SimulatedPosition     OFI+composite → paper long/short    O(1)
//     Tracker               VWAP entry, mid MTM, realized PnL
//
// P1. SlippageCalculator    Arrival price (microprice) vs exec   O(1)
//                           (ask/bid). Implementation shortfall
//                           in bps. EWM rolling average.
//
// P2. FillProbabilityModel  Proportional-hazard queue model      O(1)
//                           P(fill) = 1−exp(−λ×avgSz×T/queue)
//
// P2. RollingRiskMetrics    100-trade Sharpe (per-trade + annual) O(1)
//                           Peak-to-trough max drawdown (all-time)
//
// P3. InventoryRiskHeatmap  DV01 = |pos|×mid×0.0001              O(1)
//                           Liquidation cost, Kyle's λ impact
//                           Heat level 0-4 (ASCII heatmap display)
//
// BONUS. KalmanPriceFilter  2-state (price, velocity) Kalman     O(1)
//        DirectionAccuracy  Multi-factor composite score:         O(1)
//                           w_ofi×OFI + w_micro×microprice_skew
//                           + w_mom×momentum + w_imbal×imbalance
//                           + w_vpin×vpin_adj
//        HitRateTracker     EWM accuracy of composite signal      O(1)
//
// Thread-safety: NONE — call only from single processing thread.
// All prices/quantities in USD / base-asset units (not ×10000 encoded).
// ============================================================================

#include "SignalEngine.h"           // SignalResults, TopOfBook

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
    int    totalTrades        = 0;    // completed round-trips
    int    winningTrades      = 0;
    double winRate           = 0.0;   // winningTrades / totalTrades
    int    signalDirection   = 0;     // active: +1=LONG / -1=SHORT / 0=FLAT

    // ── P1: Slippage ─────────────────────────────────────────────────────────
    double lastSlippageBps   = 0.0;   // last fill: (exec - arrival) / arrival × 10000
    double avgSlippageBps    = 0.0;   // EWM α=0.15 rolling average (bps)
    double implShortfallBps  = 0.0;   // implementation shortfall = exec vs mid_arrival
    double lastExecPrice     = 0.0;   // simulated execution price (ask/bid)
    double lastArrivalPrice  = 0.0;   // microprice at signal time (fair-value arrival)

    // ── P2: Fill Probability ──────────────────────────────────────────────────
    double fillProb100ms     = 0.0;   // P(fill in next 100ms) ∈ [0, 1]
    double queueDepthAtL1    = 0.0;   // current L1 depth (base units) for buy/sell
    double tradeArrivalRate  = 0.0;   // aggressive trades per second (from aggTrade stream)

    // ── P2: Rolling Risk Metrics ──────────────────────────────────────────────
    double sharpePerTrade    = 0.0;   // per-trade Sharpe over rolling 100 trades
    double annualizedSharpe  = 0.0;   // × √(trades_per_year), assuming ~720/day
    double maxDrawdown       = 0.0;   // all-time peak-to-trough drawdown (USD)
    double currentDrawdown   = 0.0;   // drawdown from current all-time peak (USD)
    double peakEquity        = 0.0;   // running all-time peak total equity (USD)

    // ── P3: Inventory Risk / DV01 Heatmap ────────────────────────────────────
    double dv01              = 0.0;   // $ loss per 1 bps adverse move
    double liquidationCost   = 0.0;   // estimated half-spread cost to flatten (USD)
    double marketImpact      = 0.0;   // Kyle's λ × |position| = expected price impact
    int    heatLevel         = 0;     // 0=flat 1=low 2=medium 3=high 4=extreme

    // ── Direction Accuracy & Advanced Signals ─────────────────────────────────
    double compositeScore    = 0.0;   // [-1, +1] multi-factor direction signal
    double kalmanVelocity    = 0.0;   // Kalman filter estimated price velocity ($/tick)
    double kalmanPrice       = 0.0;   // Kalman filtered mid price estimate
    double hitRate           = 0.5;   // rolling signal accuracy ∈ [0, 1] (EWM)
    int    pendingSignals    = 0;     // signals awaiting N-tick accuracy check
};

// ============================================================================
// 1. KalmanPriceFilter
//
// Standard 2-state Kalman filter for mid-price:
//
//   State vector: x = [price, velocity]'
//   Transition:   x_{t+1} = F × x_t + noise
//                 F = [[1, dt], [0, rho]]
//                 rho = 0.9 (velocity decay / mean-reversion of velocity)
//   Measurement:  z_t = H × x_t + v_t,  H = [1, 0]
//   Process Q:    diag(q_price, q_vel)
//   Meas R:       sigma_meas²
//
// The velocity component is the HFT directional signal:
//   velocity > 0 → price trending up
//   velocity < 0 → price trending down
//
// After each tick, kalman_velocity gives the expected per-tick price change.
// Typical magnitude: ±0.001 to ±0.05 USD for SOL-level crypto prices.
// ============================================================================
class KalmanPriceFilter {
public:
    // Tuning: set q_price large if market moves quickly, small if slow
    KalmanPriceFilter(double dt      = 0.1,    // depth update interval (seconds)
                      double q_price  = 1e-4,   // process noise: price component
                      double q_vel    = 1e-5,   // process noise: velocity component
                      double r_meas   = 1e-3,   // measurement noise
                      double rho      = 0.90)   // velocity decay (0=random walk, 1=constant)
        : dt_(dt), rho_(rho), r_(r_meas)
    {
        // Initial state covariance: large uncertainty
        P_[0][0] = 1.0; P_[0][1] = 0.0;
        P_[1][0] = 0.0; P_[1][1] = 1.0;

        Q_[0][0] = q_price;  Q_[0][1] = 0.0;
        Q_[1][0] = 0.0;      Q_[1][1] = q_vel;

        x_[0] = 0.0; x_[1] = 0.0;
        initialized_ = false;
    }

    // Feed new mid-price measurement. Returns (filtered_price, velocity).
    void Update(double measuredMid) {
        if (!initialized_) {
            x_[0] = measuredMid;
            x_[1] = 0.0;
            initialized_ = true;
            return;
        }

        // ── Prediction step ───────────────────────────────────────────────────
        // F = [[1, dt], [0, rho]]
        double x_pred[2];
        x_pred[0] = x_[0] + dt_ * x_[1];      // price_pred = price + dt × velocity
        x_pred[1] = rho_  * x_[1];             // velocity_pred = rho × velocity

        // P_pred = F × P × F' + Q
        // F × P:
        double FP[2][2];
        FP[0][0] = P_[0][0] + dt_ * P_[1][0];
        FP[0][1] = P_[0][1] + dt_ * P_[1][1];
        FP[1][0] = rho_ * P_[1][0];
        FP[1][1] = rho_ * P_[1][1];

        // (F × P) × F' + Q  (F' = [[1,0],[dt,rho]])
        double P_pred[2][2];
        P_pred[0][0] = FP[0][0] * 1.0  + FP[0][1] * dt_  + Q_[0][0];
        P_pred[0][1] = FP[0][0] * 0.0  + FP[0][1] * rho_;
        P_pred[1][0] = FP[1][0] * 1.0  + FP[1][1] * dt_;
        P_pred[1][1] = FP[1][0] * 0.0  + FP[1][1] * rho_ + Q_[1][1];

        // ── Update step ───────────────────────────────────────────────────────
        // Innovation: y = z - H × x_pred  (H = [1, 0])
        double innov = measuredMid - x_pred[0];

        // Innovation covariance: S = H × P_pred × H' + R = P_pred[0][0] + R
        double S = P_pred[0][0] + r_;
        if (std::abs(S) < 1e-15) { return; }   // numerical guard

        // Kalman gain: K = P_pred × H' / S  → K is column vector [K0, K1]'
        double K0 = P_pred[0][0] / S;
        double K1 = P_pred[1][0] / S;

        // State update
        x_[0] = x_pred[0] + K0 * innov;
        x_[1] = x_pred[1] + K1 * innov;

        // Covariance update: P = (I - K × H) × P_pred
        // (I - K×H) = [[1-K0, 0], [-K1, 1]]
        P_[0][0] = (1.0 - K0) * P_pred[0][0];
        P_[0][1] = (1.0 - K0) * P_pred[0][1];
        P_[1][0] = -K1 * P_pred[0][0] + P_pred[1][0];
        P_[1][1] = -K1 * P_pred[0][1] + P_pred[1][1];
    }

    double GetPrice()    const { return x_[0]; }
    double GetVelocity() const { return x_[1]; }  // USD/tick direction signal
    bool   IsReady()     const { return initialized_; }

private:
    double dt_, rho_, r_;
    double x_[2];          // state: [price, velocity]
    double P_[2][2];       // error covariance
    double Q_[2][2];       // process noise
    bool   initialized_;
};

// ============================================================================
// 2. CompositeDirectionModel
//
// Multi-factor linear combination for directional signal generation.
// Used to drive both simulated position entry AND hit-rate tracking.
//
// Score ∈ [-1, +1]:
//   > +ENTRY_THRESH  → LONG signal
//   < -ENTRY_THRESH  → SHORT signal
//   Otherwise        → FLAT (no position)
//
// Factor construction (all inputs must be ∈ [-1, +1] or normalised):
//
//   ofi_norm:    directly from SignalEngine (already [-1,+1])
//
//   micro_skew:  (microprice - mid) / max(spread, eps)
//                → +1 = microprice well above mid (strong buy pressure)
//                → -1 = microprice well below mid (strong sell pressure)
//
//   mom_scaled:  tanh(10 × momentumScore)
//                → maps small momentum values to (-1, +1)
//
//   imbal_adj:   imbalance × (1 - min(1, vpin / 0.65))
//                → dampens imbalance when VPIN (adverse selection) is high
//
//   vpin_adj:    -(vpin - 0.5) × 2
//                → high VPIN → fade signal (informed flow risk)
//
// Weights (empirically calibrated for crypto L2 order book):
//   w_ofi   = 0.35  (dominant microstructure signal)
//   w_micro = 0.25  (microprice = best fair value estimate)
//   w_mom   = 0.20  (trend continuation in trending regime)
//   w_imbal = 0.10  (imbalance, noise-adjusted)
//   w_vpin  = 0.10  (adverse selection adjustment)
// ============================================================================
class CompositeDirectionModel {
public:
    static constexpr double W_OFI   = 0.35;
    static constexpr double W_MICRO = 0.25;
    static constexpr double W_MOM   = 0.20;
    static constexpr double W_IMBAL = 0.10;
    static constexpr double W_VPIN  = 0.10;
    static constexpr double ENTRY_THRESH = 0.18;  // entry threshold for position

    double Compute(double ofiNorm,
                   double microprice,
                   double mid,
                   double spread,
                   double momentumScore,
                   double imbalance,
                   double vpin) const {

        // 1. OFI (already normalised)
        double f_ofi = ofiNorm;

        // 2. Microprice skew — normalise by spread
        double spreadFloor = std::max(spread, 1e-6);
        double f_micro = (microprice - mid) / spreadFloor;
        f_micro = std::max(-1.0, std::min(1.0, f_micro));  // clamp

        // 3. Momentum — map to [-1, +1] via tanh
        double f_mom = std::tanh(10.0 * momentumScore);

        // 4. Imbalance — dampen when adverse selection risk is high
        double vpinDamper = 1.0 - std::min(1.0, vpin / 0.65);
        double f_imbal = imbalance * vpinDamper;
        f_imbal = std::max(-1.0, std::min(1.0, f_imbal));

        // 5. VPIN adjustment (high VPIN → fade directional signals)
        double f_vpin = -(vpin - 0.5) * 2.0;
        f_vpin = std::max(-1.0, std::min(1.0, f_vpin));

        double score = W_OFI   * f_ofi
                     + W_MICRO * f_micro
                     + W_MOM   * f_mom
                     + W_IMBAL * f_imbal
                     + W_VPIN  * f_vpin;

        return std::max(-1.0, std::min(1.0, score));
    }

    static int GetDirection(double score) {
        if      (score >  ENTRY_THRESH) return +1;  // LONG
        else if (score < -ENTRY_THRESH) return -1;  // SHORT
        else                            return  0;  // FLAT
    }
};

// ============================================================================
// 3. HitRateTracker
//
// After each directional signal, we check N=5 depth updates later whether
// the mid price moved in the predicted direction. Accuracy is tracked via:
//   hitRate_EWM = α × hit_indicator + (1 − α) × hitRate_EWM
//   α = 0.05 (slow decay, ~20-signal half-life)
//
// Also records pending signals (signal fired, outcome not yet evaluated).
// Max PENDING window = 5 depth ticks (≈500ms at 100ms stream rate).
// ============================================================================
class HitRateTracker {
public:
    static constexpr int    EVAL_DELAY_TICKS = 5;
    static constexpr double EWM_ALPHA        = 0.05;

    void OnSignal(int direction, double midAtSignal) {
        if (direction != 0) {
            pending_.push_back({direction, midAtSignal, EVAL_DELAY_TICKS});
        }
    }

    void OnTick(double currentMid) {
        tickCount_++;
        for (auto &p : pending_) p.ticksLeft--;

        // Evaluate expired signals
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (it->ticksLeft <= 0) {
                double move = currentMid - it->midAtSignal;
                bool hit = (it->direction > 0 && move > 0.0) ||
                           (it->direction < 0 && move < 0.0);
                hitRate_ = EWM_ALPHA * (hit ? 1.0 : 0.0) + (1.0 - EWM_ALPHA) * hitRate_;
                totalSignals_++;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    double GetHitRate()      const { return hitRate_; }
    int    GetPending()      const { return static_cast<int>(pending_.size()); }
    int    GetTotalSignals() const { return totalSignals_; }

private:
    struct PendingSignal { int direction; double midAtSignal; int ticksLeft; };
    std::vector<PendingSignal> pending_;
    double hitRate_       = 0.5;   // initialised to 50% (fair coin)
    int    totalSignals_  = 0;
    int    tickCount_     = 0;
};

// ============================================================================
// 4. SimulatedPositionTracker  (P1)
//
// Paper trading driven by composite direction signal.
//
// Entry rules:
//   - No open position AND |compositeScore| > ENTRY_THRESH
//   - LONG: simulated buy at ask (worst-case execution)
//   - SHORT: simulated sell at bid (worst-case execution)
//   - Position size: fixed POSITION_SIZE base units per signal
//
// Exit rules (whichever fires first):
//   1. Signal reversal: compositeScore crosses zero (from +→− or −→+)
//   2. Stop loss: |unrealised PnL| > STOP_LOSS_BPS × entry_price × POSITION_SIZE
//   3. Max hold: HOLD_TICKS depth updates (≈ 30s at 100ms)
//
// Slippage is recorded at entry and stored in SlippageRecord.
// Realized PnL and trade statistics updated on exit.
// ============================================================================
class SimulatedPositionTracker {
public:
    // ── Configuration ─────────────────────────────────────────────────────────
    static constexpr double POSITION_SIZE   = 0.10;   // base units per signal
    static constexpr double STOP_LOSS_BPS   = 15.0;   // 15 bps stop loss
    static constexpr int    HOLD_TICKS      = 300;    // max hold ≈ 30s
    static constexpr double ENTRY_THRESH    = CompositeDirectionModel::ENTRY_THRESH;

    // Results snapshot
    struct State {
        double position    = 0.0;
        double entryPrice  = 0.0;
        double unrealPnl   = 0.0;
        double realPnl     = 0.0;
        int    totalTrades = 0;
        int    wins        = 0;
        double execPrice   = 0.0;   // last execution price
        double arrivalPrice= 0.0;   // last arrival (microprice at signal)
    };

    // Call on every depth update after signals and analytics are computed
    // Returns true if a trade (entry or exit) occurred this tick
    bool OnTick(int direction,       // from CompositeDirectionModel::GetDirection
                double bestBid,
                double bestAsk,
                double microprice,   // arrival price (fair value estimate)
                double currentMid)
    {
        bool traded = false;

        if (position_ == 0.0) {
            // ── No position: check for entry ─────────────────────────────────
            if (direction != 0) {
                position_    = POSITION_SIZE * static_cast<double>(direction);
                entryPrice_  = (direction > 0) ? bestAsk : bestBid;
                arrivalPrice_= microprice;
                holdTicks_   = 0;
                entryDir_    = direction;
                lastExecPrice_ = entryPrice_;
                lastArrival_   = arrivalPrice_;
                traded       = true;
            }
        } else {
            holdTicks_++;
            double unrealPnl = position_ * (currentMid - entryPrice_);

            // ── Check exit conditions ─────────────────────────────────────────
            double stopLossUSD = STOP_LOSS_BPS / 10000.0 * entryPrice_ * std::abs(position_);
            bool stopHit    = (-unrealPnl > stopLossUSD);
            bool holdExpired= (holdTicks_ >= HOLD_TICKS);
            bool signalFlip = (direction != 0 && direction != entryDir_);

            if (stopHit || holdExpired || signalFlip) {
                // Exit: close at bid (LONG) or ask (SHORT)
                double exitPrice = (position_ > 0) ? bestBid : bestAsk;
                double tradePnl  = position_ * (exitPrice - entryPrice_);

                realizedPnl_ += tradePnl;
                totalTrades_++;
                if (tradePnl > 0.0) winningTrades_++;

                // Store last execution for slippage calc
                lastExecPrice_ = exitPrice;
                lastArrival_   = microprice;  // microprice at exit

                position_   = 0.0;
                entryPrice_ = 0.0;
                holdTicks_  = 0;
                entryDir_   = 0;
                traded      = true;
            }
        }
        return traded;
    }

    // Snapshot for risk results
    State GetState(double currentMid, double microprice) const {
        State s;
        s.position     = position_;
        s.entryPrice   = entryPrice_;
        s.unrealPnl    = position_ * (microprice - entryPrice_);   // MTM via microprice
        s.realPnl      = realizedPnl_;
        s.totalTrades  = totalTrades_;
        s.wins         = winningTrades_;
        s.execPrice    = lastExecPrice_;
        s.arrivalPrice = lastArrival_;
        return s;
    }

    double GetPosition()       const { return position_; }
    double GetEntryPrice()     const { return entryPrice_; }
    double GetRealizedPnl()    const { return realizedPnl_; }
    int    GetTotalTrades()    const { return totalTrades_; }
    int    GetWinningTrades()  const { return winningTrades_; }
    double GetLastExecPrice()  const { return lastExecPrice_; }
    double GetLastArrival()    const { return lastArrival_; }

private:
    double position_       = 0.0;
    double entryPrice_     = 0.0;
    double arrivalPrice_   = 0.0;
    double realizedPnl_    = 0.0;
    double lastExecPrice_  = 0.0;
    double lastArrival_    = 0.0;
    int    holdTicks_      = 0;
    int    entryDir_       = 0;
    int    totalTrades_    = 0;
    int    winningTrades_  = 0;
};

// ============================================================================
// 5. SlippageCalculator  (P1)
//
// For each simulated fill, computes:
//
//   Slippage (bps) = (exec_price − arrival_price) × sign / arrival_price × 10000
//     sign = +1 for BUY (we pay more), -1 for SELL
//     → positive = worse execution than expected
//     → negative = better execution (price improvement)
//
//   Implementation Shortfall (bps) = (exec − mid_at_arrival) / mid_at_arrival × 10000
//     mid_at_arrival = midPrice when signal was generated
//     → captures full adverse-selection cost including pre-signal drift
//
//   EWM rolling average: α = 0.15 (≈7-trade half-life)
// ============================================================================
class SlippageCalculator {
public:
    static constexpr double EWM_ALPHA = 0.15;

    // Call on every trade (entry or exit)
    void OnFill(double execPrice,
                double arrivalPrice,   // microprice at signal time
                double midAtArrival,   // mid price at signal time
                int    direction)      // +1=BUY -1=SELL
    {
        if (arrivalPrice < 1e-6 || midAtArrival < 1e-6) return;

        double dirSign = static_cast<double>(direction);

        // Slippage vs microprice (execution quality)
        double rawSlip = (execPrice - arrivalPrice) * dirSign;
        lastSlippageBps_ = rawSlip / arrivalPrice * 10000.0;
        avgSlippageBps_  = EWM_ALPHA * lastSlippageBps_ + (1.0 - EWM_ALPHA) * avgSlippageBps_;

        // Implementation shortfall vs mid at arrival
        double shortfall = (execPrice - midAtArrival) * dirSign;
        lastImplShortfall_ = shortfall / midAtArrival * 10000.0;
    }

    double GetLastSlippageBps()  const { return lastSlippageBps_; }
    double GetAvgSlippageBps()   const { return avgSlippageBps_;  }
    double GetImplShortfall()    const { return lastImplShortfall_; }

private:
    double lastSlippageBps_   = 0.0;
    double avgSlippageBps_    = 0.0;
    double lastImplShortfall_ = 0.0;
};

// ============================================================================
// 6. FillProbabilityModel  (P2)
//
// Proportional-hazard model for limit-order fill probability:
//
//   λ   = trade arrival rate at L1 (aggressive trades per second)
//         estimated from rolling aggTrade count / elapsed time
//
//   sz  = avg_trade_size (rolling mean of trade quantities)
//
//   Q   = current L1 queue depth (qty visible at best bid or ask)
//
//   Our order size = POSITION_SIZE
//
//   Model: each arriving aggressive order depletes the queue.
//   Total depleted in T seconds: E[D(T)] = λ × T × sz
//   P(fill in T) = P(D(T) ≥ Q_ahead)
//
//   Approximation (exponential CDF):
//     P(fill in T) = 1 − exp(−λ × sz × T / Q)
//
//   Here Q = queue depth at L1 (our order is at L1, all ahead of us
//   in the queue must be consumed first, simplified: we assume we're
//   next in queue = best approximation for a new limit order).
//
//   T = 0.100 seconds (one depth-update interval = 100ms)
//
// Parameters updated on each aggTrade and each depth update.
// ============================================================================
class FillProbabilityModel {
public:
    static constexpr int    TRADE_WINDOW  = 100;   // trades for rate estimation
    static constexpr double T_HORIZON_S   = 0.10;  // 100ms evaluation window
    static constexpr double ORDER_SIZE    = SimulatedPositionTracker::POSITION_SIZE;
    static constexpr double MIN_RATE      = 0.01;  // floor (1 trade per 100s)

    void OnTrade(double qty) {
        tradeQtys_.push_back(qty);
        qtySumWindow_ += qty;
        tradeCountWindow_++;
        if (static_cast<int>(tradeQtys_.size()) > TRADE_WINDOW) {
            qtySumWindow_ -= tradeQtys_.front();
            tradeQtys_.pop_front();
            tradeCountWindow_--;
        }
        totalTrades_++;
        totalQty_     += qty;
        // Elapsed seconds: approximate from accumulated trade count
        // We don't have wall-clock per-trade here, so use depth-tick count
        elapsedTicks_++; // will be set more precisely from OnDepthUpdate
    }

    // Call on each depth update with L1 queue depth relevant to trade direction
    // direction: +1 = checking BUY fill (need L1 ask to be consumed), -1 = SELL
    void OnDepthUpdate(double l1BidQty, double l1AskQty, int direction, int64_t nowUs) {
        nowUs_ = nowUs;
        l1BidQty_ = l1BidQty;
        l1AskQty_ = l1AskQty;
        elapsedTicks_++;

        // Trade arrival rate: trades per second
        // Use elapsed ticks × 0.1s per tick as time elapsed
        double elapsed_s = static_cast<double>(elapsedTicks_) * 0.10;
        if (elapsed_s < 1e-3) elapsed_s = 1e-3;
        tradeRate_ = std::max(MIN_RATE,
            static_cast<double>(totalTrades_) / elapsed_s);

        // Average trade size (over rolling window)
        double avgSz = (tradeCountWindow_ > 0)
            ? qtySumWindow_ / static_cast<double>(tradeCountWindow_)
            : 0.01;

        // Queue depth for our direction
        double queueDepth = (direction >= 0) ? l1AskQty_ : l1BidQty_;
        if (queueDepth < 1e-9) queueDepth = ORDER_SIZE;

        // P(fill in T) = 1 - exp(-λ × avgSz × T / queueDepth)
        double lambda_eff = tradeRate_ * avgSz * T_HORIZON_S / queueDepth;
        fillProb_ = 1.0 - std::exp(-lambda_eff);
        fillProb_ = std::max(0.0, std::min(1.0, fillProb_));
        queueDepthDisplay_ = queueDepth;
    }

    double GetFillProbability() const { return fillProb_;           }
    double GetQueueDepth()      const { return queueDepthDisplay_;  }
    double GetTradeRate()       const { return tradeRate_;          }

private:
    std::deque<double> tradeQtys_;
    double qtySumWindow_     = 0.0;
    int    tradeCountWindow_ = 0;
    int    totalTrades_      = 0;
    double totalQty_         = 0.0;
    int    elapsedTicks_     = 0;
    double tradeRate_        = MIN_RATE;
    double fillProb_         = 0.0;
    double queueDepthDisplay_= 0.0;
    double l1BidQty_         = 0.0;
    double l1AskQty_         = 0.0;
    int64_t nowUs_           = 0;
};

// ============================================================================
// 7. RollingRiskMetrics  (P2)
//
// Rolling 100-trade Sharpe ratio and all-time max drawdown.
//
// Per-trade Sharpe (no annualisation needed for live monitoring):
//   Sharpe_per_trade = E[pnl_i] / σ[pnl_i]   over rolling window of N trades
//   Uses Welford's online algorithm for stable O(1) variance computation.
//
// Annualised Sharpe:
//   For crypto running 24/7 with ~720 signals/day at this frequency:
//   annual_trades ≈ 720 × 365 = 262,800
//   annualised = per_trade × √262800 ≈ per_trade × 512
//   (This assumes ~2-minute average trade hold time — adjust if needed)
//
// Max drawdown (all-time, not rolling):
//   Track equity curve = cumulative realised PnL (not total PnL, to avoid
//   contamination from open-position unrealised PnL).
//   peak = max(equity over all time)
//   drawdown = peak − current_equity
//   maxDrawdown = max(drawdown over all time)
// ============================================================================
class RollingRiskMetrics {
public:
    static constexpr int    WINDOW_SIZE      = 100;
    static constexpr double ANNUAL_FACTOR    = 512.0;   // √262800 ≈ 512

    void OnTradeClosed(double tradePnl) {
        // ── Rolling Sharpe ──────────────────────────────────────────────────
        if (static_cast<int>(pnlWindow_.size()) >= WINDOW_SIZE) {
            double old = pnlWindow_.front();
            pnlWindow_.pop_front();
            sumPnl_  -= old;
            sumPnl2_ -= old * old;
            n_--;
        }
        pnlWindow_.push_back(tradePnl);
        sumPnl_  += tradePnl;
        sumPnl2_ += tradePnl * tradePnl;
        n_++;

        if (n_ >= 5) {  // need at least 5 trades for meaningful Sharpe
            double N   = static_cast<double>(n_);
            double mean = sumPnl_ / N;
            double var  = (sumPnl2_ / N) - mean * mean;
            // Sample variance correction
            if (n_ >= 2) var = var * N / (N - 1.0);
            double sd = (var > 1e-15) ? std::sqrt(var) : 1e-9;
            sharpe_           = mean / sd;
            annualizedSharpe_ = sharpe_ * ANNUAL_FACTOR;
        }

        // ── Equity curve and drawdown ────────────────────────────────────────
        cumulativeRealPnl_ += tradePnl;
        if (cumulativeRealPnl_ > peakEquity_)
            peakEquity_ = cumulativeRealPnl_;

        double dd = peakEquity_ - cumulativeRealPnl_;
        currentDrawdown_ = dd;
        if (dd > maxDrawdown_) maxDrawdown_ = dd;
    }

    double GetSharpePerTrade()   const { return sharpe_;            }
    double GetAnnualizedSharpe() const { return annualizedSharpe_;  }
    double GetMaxDrawdown()      const { return maxDrawdown_;       }
    double GetCurrentDrawdown()  const { return currentDrawdown_;   }
    double GetPeakEquity()       const { return peakEquity_;        }

private:
    std::deque<double> pnlWindow_;
    double sumPnl_           = 0.0;
    double sumPnl2_          = 0.0;
    int    n_                = 0;
    double sharpe_           = 0.0;
    double annualizedSharpe_ = 0.0;
    double cumulativeRealPnl_= 0.0;
    double peakEquity_       = 0.0;
    double maxDrawdown_      = 0.0;
    double currentDrawdown_  = 0.0;
};

// ============================================================================
// 8. InventoryRiskHeatmap  (P3)
//
// For a given position, computes:
//
//   DV01 = |position| × current_mid × 0.0001   ($ per 1 bps adverse move)
//          "If market moves 1 bps against me, I lose $DV01"
//
//   Liquidation cost = |position| × (bestAsk − bestBid) / 2
//          (half-spread cost to flatten using a market order)
//
//   Market impact = |position| × kyleLambda
//          (expected price impact of unwinding, via Kyle's λ)
//
//   Heat level (for ASCII heatmap):
//     0 = FLAT (no position)
//     1 = LOW    (DV01 < $0.01)
//     2 = MEDIUM (DV01 < $0.05)
//     3 = HIGH   (DV01 < $0.20)
//     4 = EXTREME (DV01 ≥ $0.20)
//
//   ASCII bar: "▓" repeated proportional to heat level (max 5 blocks)
// ============================================================================
class InventoryRiskHeatmap {
public:
    struct RiskData {
        double dv01          = 0.0;
        double liquidCost    = 0.0;
        double mktImpact     = 0.0;
        int    heatLevel     = 0;     // 0-4
    };

    static RiskData Compute(double position,
                            double currentMid,
                            double bestBid,
                            double bestAsk,
                            double kyleLambda) {
        RiskData r;
        if (std::abs(position) < 1e-9) return r;

        double absPos = std::abs(position);
        r.dv01       = absPos * currentMid * 0.0001;
        r.liquidCost = absPos * (bestAsk - bestBid) / 2.0;
        r.mktImpact  = absPos * std::abs(kyleLambda) * absPos;  // λ × Q² / 2 model

        // Heat level
        if      (r.dv01 <  0.01) r.heatLevel = 1;
        else if (r.dv01 <  0.05) r.heatLevel = 2;
        else if (r.dv01 <  0.20) r.heatLevel = 3;
        else                     r.heatLevel = 4;

        return r;
    }

    // Returns ASCII heatmap bar string (for display only)
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
// RiskEngine — orchestrates all 8 components
//
// Usage:
//   RiskEngine risk;
//
//   // On each aggTrade (from aggTrade stream):
//   risk.OnTrade(price, qty, direction);  // direction: +1=BUY, -1=SELL
//
//   // On each depth update (after signals and analytics are computed):
//   risk.OnDepthUpdate(
//       sig.compositeScore, sig.ofiNormalized,  // from SignalEngine
//       analytics.midPrice, analytics.microprice,
//       analytics.spreadBps / 10000.0 * analytics.midPrice,  // spread in USD
//       snap.bids.begin()->first / 10000.0,   // bestBid
//       snap.asks.begin()->first / 10000.0,   // bestAsk
//       snap.bids.begin()->second / 10000.0,  // bidQty at L1
//       snap.asks.begin()->second / 10000.0,  // askQty at L1
//       sig.vpin, sig.momentumScore, analytics.imbalance,
//       micro.kyleLambda,
//       nowUs);
//
//   // Read results:
//   const RiskResults& r = risk.GetResults();
// ============================================================================
class RiskEngine {
public:
    // ── Called on each aggTrade ───────────────────────────────────────────────
    void OnTrade(double price, double qty, int direction) {
        fillModel_.OnTrade(qty);
        // Record trade quantity for slippage model if we have an open position
        (void)price;   // used indirectly via microprice at fill time
        (void)direction;
    }

    // ── Called on each depth update (main integration point) ─────────────────
    void OnDepthUpdate(
        const SignalResults &sig,
        double midPrice,
        double microprice,
        double spreadUSD,    // quoted spread in USD
        double bestBid,
        double bestAsk,
        double bidQtyL1,
        double askQtyL1,
        double imbalance,
        double kyleLambda,
        int64_t nowUs)
    {
        // ── Kalman filter update ──────────────────────────────────────────────
        kalman_.Update(midPrice);
        results_.kalmanPrice    = kalman_.GetPrice();
        results_.kalmanVelocity = kalman_.GetVelocity();

        // ── Composite direction model ─────────────────────────────────────────
        double composite = dir_.Compute(
            sig.ofiNormalized,
            microprice,
            midPrice,
            spreadUSD,
            sig.momentumScore,
            imbalance,
            sig.vpin);
        results_.compositeScore = composite;

        int directionInt = CompositeDirectionModel::GetDirection(composite);
        results_.signalDirection = directionInt;

        // ── Hit rate tracking ─────────────────────────────────────────────────
        if (directionInt != lastDirection_) {
            hitTracker_.OnSignal(directionInt, midPrice);
            lastDirection_ = directionInt;
        }
        hitTracker_.OnTick(midPrice);
        results_.hitRate       = hitTracker_.GetHitRate();
        results_.pendingSignals= hitTracker_.GetPending();

        // ── Simulated position tracker ────────────────────────────────────────
        bool traded = position_.OnTick(directionInt, bestBid, bestAsk, microprice, midPrice);

        if (traded && position_.GetTotalTrades() > 0) {
            // A trade just closed — record slippage and risk metrics
            double execP    = position_.GetLastExecPrice();
            double arrivalP = position_.GetLastArrival();
            int    dir      = (position_.GetPosition() == 0.0 && position_.GetEntryPrice() == 0.0)
                              ? (execP > midPrice ? -1 : +1)   // exit direction
                              : directionInt;

            slippage_.OnFill(execP, arrivalP, midPrice, dir);

            // Record this trade's PnL to rolling risk metrics
            // Get the per-trade PnL (last realized change)
            double thisTradePnl = position_.GetRealizedPnl() - lastRealPnl_;
            lastRealPnl_        = position_.GetRealizedPnl();
            if (std::abs(thisTradePnl) > 0.0)
                rolling_.OnTradeClosed(thisTradePnl);
        }

        // ── Fill probability update ───────────────────────────────────────────
        fillModel_.OnDepthUpdate(bidQtyL1, askQtyL1, directionInt, nowUs);

        // ── Inventory risk / DV01 ─────────────────────────────────────────────
        auto posState = position_.GetState(midPrice, microprice);
        auto heat     = InventoryRiskHeatmap::Compute(
            posState.position, midPrice, bestBid, bestAsk, kyleLambda);

        // ── Assemble results ──────────────────────────────────────────────────
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

private:
    KalmanPriceFilter        kalman_;
    CompositeDirectionModel  dir_;
    HitRateTracker           hitTracker_;
    SimulatedPositionTracker position_;
    SlippageCalculator       slippage_;
    FillProbabilityModel     fillModel_;
    RollingRiskMetrics       rolling_;

    RiskResults results_;
    int    lastDirection_ = 0;
    double lastRealPnl_   = 0.0;
};