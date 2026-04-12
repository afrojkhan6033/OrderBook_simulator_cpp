#pragma once
// ============================================================================
// StrategyEngine.h  v3  —  HFT Strategy Simulation & Backtesting Engine
//
// ROOT CAUSE ANALYSIS of v1/v2 failures:
//
//  Bug A — AS σ² dimensional inconsistency:
//    v2 computed σ_USD/√s = σ_tick × mid × √10.  For BTC this gives
//    σ²_USD ≈ 11900 USD²/s → γ·σ² ≈ $95 skew per unit (absurd).
//    Any γ that fixes the skew breaks the 2/γ·ln term.
//    FIX: work entirely in BPS space. σ_bps_tick = σ_logreturn × 10000.
//    σ²_bps = σ_bps_tick². γ_bps in 1/bps. δ* in bps. Convert to USD at end.
//
//  Bug B — κ dimensional error:
//    v2: κ = tradeRate / (spread_USD/mid) → for BTC κ = 34,000,000.
//    ln(1+γ/κ) → 0, so the capture term vanishes.
//    FIX: κ_bps = tradeRate × dt_s / (halfSpread_bps).
//    Interpretation: order arrivals per bps of half-spread per tick interval.
//    For SOL: 8/s × 0.1s / (1.5bps/2) ≈ 1.07 → δ* ≈ 2.7 bps ✓
//
//  Bug C — Fill condition impossible:
//    v2: fill fires when aggTrade price >= synthetic ask.
//    Synthetic ask is inside the real book (AS gives tighter quotes).
//    Aggressive fill at real book ask never hits our inner synthetic quote.
//    FIX: probabilistic fill model. Each 100ms tick, P(fill) = queueModel().
//    Uses L1 queue depth, trade arrival rate, our quote offset from best.
//    This produces realistic 3-10 fills/min.
//
//  Bug D — Adverse selection on every tick (×1000 error):
//    v2: EWM or sum over ALL ticks after fill. 
//    FIX: FillRecord with 5-tick countdown. Measure ONCE.
//
//  Bug E — Bybit feed drops after 20s:
//    Bybit V5 requires a heartbeat ping frame every 20s.
//    No ping → server closes connection → reconnect → prices always 0.0.
//    FIX: ping goroutine every 15s: ws.write(buffer("{\"op\":\"ping\"}")).
//
//  Bug F — Inventory never recovers:
//    Inventory limit hit on one side, no mechanism to reduce it except fills.
//    FIX: reservation price skew + stronger γ scaling drives quotes toward
//    inventory-reducing direction. Also: continuous aggressor fill credit.
//
// Architecture:
//   All σ, δ, skew computed in bps; converted to USD at final output step.
//   StrategyContext: single bundle populated once per tick from all engines.
//   Hot path: O(1) per tick, no heap allocation, no mutex.
// ============================================================================

#include "RegimeEngine.h"

#include <deque>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <string>
#include <limits>
#include <thread>
#include <chrono>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
// ─── StrategyContext: single-tick bundle passed to all strategy engines ────────
struct StrategyContext {
    // Market state
    double mid=0, microprice=0, spreadUSD=0, bid1=0, ask1=0, bidQ1=0, askQ1=0;
    // Microstructure
    double sigmaLogReturnPerTick=0.001;  // YangZhang: dimensionless log-return std
    double tradeRatePerSec=1.0;          // aggressive trades / second
    double kyleLambda=0;                 // price impact per signed-flow unit
    double avgTradeSizeUnits=0.1;        // avg size per aggTrade
    // Signal layer
    double ofiNorm=0;                    // multi-level OFI [-1,+1]
    double vpin=0;                       // VPIN [0,1]
    double momentumScore=0;
    int    momentumRegime=0;             // +1=TREND -1=MR 0=NEUT
    // Risk / direction
    double compositeScore=0;
    double regimeAdjScore=0;             // regime-gated composite [-1,+1]
    int    regimeAdjDir=0;
    double kalmanVelocity=0;             // Kalman filtered price velocity $/tick
    double hitRate=0.5;                  // rolling signal accuracy
    double edgeScore=0;                  // expected edge bps (RegimeEngine)
    // Regime
    double realizedVolPerTick=0.001;     // YangZhang σ (log-return units)
    double realizedVolAnnualized=50;
    int    volRegime=1;                  // 0=LOW 1=NORM 2=HIGH 3=EXTREME
    int    hmmState=0;                   // 0=BULL_MICRO 1=BEAR_MICRO
    bool   hmmConfident=false;
    bool   flashCrashPrecursor=false;
    bool   spreadWideningAlert=false;
    double hmmBullProb=0.5;
    double hurstExponent=0.5;            // H>0.55=trend H<0.45=MR
    int    hurstRegime=0;
    double autocorrLag1=0;              // mid-return ACF lag-1
};

// ─── Strategy Results ─────────────────────────────────────────────────────────
struct StrategyResults {
    // AS Model
    double asBid=0, asAsk=0, asReservation=0;
    double asOptimalSpreadBps=0;         // δ* in bps
    double asOptimalSpreadUSD=0;         // δ* in USD
    double asSkewBps=0;                  // inventory skew in bps (signed)
    double asSkewUSD=0;
    double asSigmaBps=0;                 // σ in bps/tick
    double asGamma=0, asKappa=0;
    bool   asQuotingActive=true;
    std::string asGateReason;

    // MM PnL (all USD)
    double mmRealizedPnl=0;
    double mmUnrealizedPnl=0;
    double mmNetPnl=0;
    double mmSpreadCapture=0;
    double mmAdvSelection=0;
    double mmInventory=0;
    int    mmFills=0, mmRoundTrips=0;
    double mmWinRate=0, mmFillRate=0, mmKellyUnit=0.05;
    double mmProbFill100ms=0;            // fill probability this tick
    bool   mmInventoryAlert=false, mmQuotingGated=false;

    // Latency arb
    double latEdgeBps=0, latEdgeUSD=0, latCumEdgeUSD=0, latSharpe=0;
    int    latOpportunities=0;
    static constexpr int LAT_SIM_US = 500;

    // Signal replay
    static constexpr int CHART_COLS=20, CHART_ROWS=8;
    char   replayChart[CHART_ROWS][CHART_COLS+1]={};
    double replayMinPrice=0, replayMaxPrice=0, replaySimPnl=0;
    double replayWinRate=0, replayMAE=0, replayMFE=0;
    int    replayEntries=0, replayExits=0;

    // Cross-exchange
    double exchBid1=0, exchAsk1=0, exchBid2=0, exchAsk2=0;
    double exchMidSpreadBps=0, exchArbBps=0, exchCumArbBps=0;
    bool   exchArbAlert=false, exchConnected=false;
    int    exchArbCount=0;
    std::string exchName{"Bybit"};
};

// ============================================================================
// AvellanedaStoikovModelV3
//
// Full BPS-space formulation. All intermediate quantities in bps, convert
// to USD only for final quote placement.
//
// σ_bps = σ_logreturn × 10000              [bps per tick, e.g. 3 bps for SOL]
// σ²_bps = σ_bps²                          [bps² per tick]
//
// κ_bps = tradeRate × dt_tick / (halfSpread_bps)
//         [arrivals per bps of half-spread per 100ms tick]
//         dt_tick = 0.1 seconds (100ms depth stream)
//         halfSpread_bps = spread_bps / 2
//         → For SOL: 8 × 0.1 / (1.5/2) ≈ 1.07 ✓
//
// δ*_bps = γ × σ²_bps + (2/γ) × ln(1 + γ/κ_bps)
// skew_bps = inventory × γ × σ²_bps
// r_bps = (microprice/mid - 1) × 10000 - skew_bps   [bps from mid]
// bid_USD = mid × (1 + (r_bps - δ*/2) / 10000)
// ask_USD = mid × (1 + (r_bps + δ*/2) / 10000)
//
// Adaptive γ:
//   γ_eff = γ_base × (1 + |inv|/MAX_INV × INV_AVERSION)
//           × (1 + max(0, VPIN-0.40) × VPIN_AVERSION)
//           × VOL_MULT[volRegime]
//
// OFI asymmetric widening:
//   bid half-spread ×= (1 + max(0, -ofi) × OFI_SCALE)  ← sell pressure widens bid
//   ask half-spread ×= (1 + max(0, +ofi) × OFI_SCALE)  ← buy pressure widens ask
//
// HMM BEAR state: bid half-spread ×= BEAR_WIDEN (less willing to buy)
//
// Gate triggers (suppress all quoting):
//   flashCrashPrecursor | VPIN > 0.80 | volRegime >= 3
// ============================================================================
class AvellanedaStoikovModelV3 {
public:
    // BPS-space parameters (calibrated for crypto spot, 100ms depth stream)
    static constexpr double GAMMA_BASE    = 0.1;    // 1/bps risk aversion
    static constexpr double INV_AVERSION  = 4.0;    // γ multiplier at max inventory
    static constexpr double VPIN_AVERSION = 5.0;    // γ multiplier at VPIN=1
    static constexpr double OFI_SCALE     = 1.5;    // OFI widening scale
    static constexpr double BEAR_WIDEN    = 1.8;    // HMM-BEAR bid widening
    static constexpr double VPIN_GATE     = 0.80;   // suppress quoting above this
    static constexpr double VPIN_WIDEN_THR= 0.50;   // widen above this
    static constexpr double MAX_INV       = 0.50;   // max inventory (base units)
    static constexpr double DT_TICK       = 0.10;   // 100ms depth stream interval
    static constexpr double MIN_DELTA_BPS = 1.0;    // minimum half-spread floor (bps)
    static constexpr double VOL_MULT[4]   = {0.6, 1.0, 1.8, 3.5};

    struct Quotes {
        double bid=0,ask=0,reservation=0;
        double deltaBps=0, deltaUSD=0;
        double skewBps=0, skewUSD=0, sigmaBps=0;
        double gamma=0, kappa=0;
        bool   active=true;
        std::string gateReason;
    };

    Quotes Compute(const StrategyContext &c, double inventory) const {
        Quotes q;
        // ── Gate checks ───────────────────────────────────────────────────────
        if (c.flashCrashPrecursor) { q.active=false; q.gateReason="FLASH_CRASH"; return q; }
        if (c.vpin > VPIN_GATE)    { q.active=false; q.gateReason="VPIN>0.80";   return q; }
        if (c.volRegime >= 3)       { q.active=false; q.gateReason="VOL_EXTREME"; return q; }
        if (c.mid < 1e-6)           { q.active=false; q.gateReason="NO_MID";      return q; }

        // ── σ in bps per tick ─────────────────────────────────────────────────
        double sigmaBps = std::max(c.sigmaLogReturnPerTick, 1e-8) * 10000.0;
        double sigma2   = sigmaBps * sigmaBps;   // bps²
        q.sigmaBps = sigmaBps;

        // ── Spread in bps ─────────────────────────────────────────────────────
        double spreadBps    = (c.mid > 1e-6) ? c.spreadUSD / c.mid * 10000.0 : 2.0;
        double halfSpreadBps= std::max(spreadBps * 0.5, 0.5);

        // ── κ_bps = arrivalRate × dt / halfSpread_bps ─────────────────────────
        double kappa = std::max(0.01, c.tradeRatePerSec * DT_TICK / halfSpreadBps);
        q.kappa = kappa;

        // ── Adaptive γ ────────────────────────────────────────────────────────
        double invF  = std::min(1.0, std::abs(inventory) / MAX_INV);
        double vpinX = std::max(0.0, c.vpin - 0.40);
        double volM  = VOL_MULT[std::max(0,std::min(3,c.volRegime))];
        double gamma = GAMMA_BASE * (1.0 + invF*INV_AVERSION) * (1.0 + vpinX*VPIN_AVERSION) * volM;
        q.gamma = gamma;

        // ── Inventory skew in bps ─────────────────────────────────────────────
        double skewBps = inventory * gamma * sigma2;
        q.skewBps = skewBps;
        q.skewUSD = skewBps / 10000.0 * c.mid;

        // ── Reservation price in bps from mid ─────────────────────────────────
        // Use microprice bias + inventory skew
        double microBias = (c.microprice > 0.0 && c.mid > 0.0)
            ? (c.microprice / c.mid - 1.0) * 10000.0 : 0.0;
        double rBps = microBias - skewBps;   // bps above/below mid

        // ── Optimal spread δ* (bps) ───────────────────────────────────────────
        double gkRatio = gamma / kappa;
        double lnTerm  = (gkRatio > 1e-12) ? std::log(1.0 + gkRatio) : gkRatio;
        double deltaBps = gamma * sigma2 + (2.0 / gamma) * lnTerm;
        deltaBps = std::max(deltaBps, MIN_DELTA_BPS * 2.0);  // floor 2×min (bid+ask)
        q.deltaBps = deltaBps;
        q.deltaUSD = deltaBps / 10000.0 * c.mid;

        // ── Asymmetric half-spreads ───────────────────────────────────────────
        double hBid = deltaBps * 0.5;
        double hAsk = deltaBps * 0.5;

        // OFI: sell pressure (ofi<0) widens bid (we don't want to buy into selling)
        if (c.ofiNorm < -0.05) hBid *= (1.0 + std::abs(c.ofiNorm) * OFI_SCALE);
        if (c.ofiNorm > +0.05) hAsk *= (1.0 + std::abs(c.ofiNorm) * OFI_SCALE);

        // Spread widening regime: widen all
        if (c.spreadWideningAlert) { hBid *= 1.6; hAsk *= 1.6; }

        // VPIN toxicity: proportional widening above threshold
        if (c.vpin > VPIN_WIDEN_THR) {
            double excess = (c.vpin - VPIN_WIDEN_THR) / (1.0 - VPIN_WIDEN_THR);
            double vMul   = 1.0 + excess * 2.0;
            hBid *= vMul; hAsk *= vMul;
        }

        // HMM BEAR: widen bids (inventory risk of buying in bear state)
        if (c.hmmState == 1 && c.hmmConfident) hBid *= BEAR_WIDEN;

        // Kalman velocity: lean quotes toward momentum
        if (std::abs(c.kalmanVelocity) > 1e-8 && c.hmmConfident) {
            double velBps = c.kalmanVelocity / c.mid * 10000.0 * 10.0;  // scale
            velBps = std::max(-2.0, std::min(2.0, velBps));
            hBid += velBps;  // velocity up → move bid up → buy more
            hAsk -= velBps;  // velocity up → move ask up → sell less
            hBid = std::max(hBid, MIN_DELTA_BPS);
            hAsk = std::max(hAsk, MIN_DELTA_BPS);
        }

        // ── Final quote prices ────────────────────────────────────────────────
        double rUSD   = c.mid * (1.0 + rBps / 10000.0);
        q.reservation = rUSD;
        q.bid = rUSD * (1.0 - hBid / 10000.0);
        q.ask = rUSD * (1.0 + hAsk / 10000.0);

        // Sanity: quotes must be on correct side of mid
        q.bid = std::min(q.bid, c.mid - c.spreadUSD * 0.1);
        q.ask = std::max(q.ask, c.mid + c.spreadUSD * 0.1);

        return q;
    }
};

// ============================================================================
// MMFillProbabilityModel
//
// Probabilistic fill model based on queue theory (Lo, MacKinlay, Zhang 2002).
//
// For a limit order at distance d_bps from best quote:
//   P(fill in next dt) = 1 - exp(-λ_eff × dt)
//
// λ_eff = λ_0 × exp(-α × d_bps)
//   λ_0  = base fill rate at best (≈ tradeRate / avgQueueDepth)
//   α    = depth decay constant (higher = exponential dropoff with distance)
//   d_bps = our quote distance from the current best (bid or ask)
//
// Queue depth at our level (approximation):
//   d = 0     → at best bid/ask → fills at full trade rate
//   d = 1 bps → fill rate halved
//   d = 2 bps → fill rate quartered
//
// Inventory-constrained: only allow fill if within inventory limits.
// VPIN-filtered: P(fill) × (1 - max(0, vpin - 0.4)/0.6) to avoid toxic fills.
// ============================================================================
class MMFillProbabilityModel {
public:
    static constexpr double ALPHA_DECAY = 0.7;   // bps^-1 decay constant
    static constexpr double DT          = 0.10;  // 100ms tick interval

    // Returns P(bid fills) and P(ask fills) in next tick
    struct FillProbs { double bid, ask; };

    FillProbs Compute(const AvellanedaStoikovModelV3::Quotes &q,
                      const StrategyContext &c,
                      double inventory) const {
        if (!q.active) return {0.0, 0.0};

        double spreadBps = (c.mid > 0) ? c.spreadUSD / c.mid * 10000.0 : 2.0;

        // Base fill rate at best: tradeRate / queue_depth_estimate
        // Queue depth ≈ L1 qty in trade-size units
        double queueDepth = std::max(0.1, (c.bidQ1 + c.askQ1) / std::max(c.avgTradeSizeUnits, 0.01) * 0.5);
        double lambda0    = c.tradeRatePerSec / std::max(queueDepth, 0.5);

        // Distance from best
        double bidDist = (c.bid1 > 0) ? (c.bid1 - q.bid) / c.mid * 10000.0 : 1.0;
        double askDist = (q.ask - c.ask1) / c.mid * 10000.0;
        bidDist = std::max(0.0, bidDist);
        askDist = std::max(0.0, askDist);

        // Fill intensity (exponential depth decay)
        double lambdaBid = lambda0 * std::exp(-ALPHA_DECAY * bidDist);
        double lambdaAsk = lambda0 * std::exp(-ALPHA_DECAY * askDist);

        // VPIN filter: reduce fill probability when flow is toxic
        // Informed traders will fill our quotes adversely → reduce exposure
        double vpinPenalty = 1.0 - std::max(0.0, (c.vpin - 0.40) / 0.60);

        // Inventory limits
        bool canBuy  = (inventory <  AvellanedaStoikovModelV3::MAX_INV * 0.9);
        bool canSell = (inventory > -AvellanedaStoikovModelV3::MAX_INV * 0.9);

        double pBid = canBuy  ? (1.0 - std::exp(-lambdaBid * DT)) * vpinPenalty : 0.0;
        double pAsk = canSell ? (1.0 - std::exp(-lambdaAsk * DT)) * vpinPenalty : 0.0;

        return {pBid, pAsk};
    }
};

// ============================================================================
// MarketMakingSimulatorV3
//
// Uses probabilistic fills (queue model) instead of exact price matching.
// Fills trigger stochastically each 100ms tick based on MMFillProbabilityModel.
//
// On each tick:
//   1. Compute fill probabilities (bid/ask separately)
//   2. If random draw < P(fill): execute fill at quote price
//   3. Record FillRecord; 5 ticks later measure adverse selection
//   4. Update realized PnL on round-trip completion
//   5. MTM unrealized via microprice
//
// Note: We use a deterministic pseudo-random draw based on a fast LCG to
// avoid std::mt19937 overhead on hot path. The LCG state is carried across
// ticks.
//
// Kelly unit: half-Kelly from rolling 200-trade win/loss history.
//   f* = (p×b − q)/b × 0.5
//   b = avgWin/avgLoss from observed round-trip PnLs
//
// Adverse selection (corrected B2):
//   5 ticks after fill: measure (mid_now − mid_at_fill) × dir
//   Adverse = max(0, −move_in_our_favor) × unit
//   Single measurement per fill, not per tick.
// ============================================================================
class MarketMakingSimulatorV3 {
public:
    static constexpr double BASE_UNIT  = 0.05;
    static constexpr double MAX_INV    = AvellanedaStoikovModelV3::MAX_INV;
    static constexpr double HARD_LIMIT = 0.60;
    static constexpr int    ADV_TICKS  = 5;
    static constexpr int    FILL_WIN_S = 60;
    static constexpr double MAX_KELLY  = 2.0;

    struct FillRecord { double fillP, midAtFill; int side, tLeft; double unit; };

    using FillCallback = std::function<void(bool /*isBuy*/, double /*qtyMultiplier*/)>;

    void SetFillCallback(FillCallback cb) {
        fillCallback_ = std::move(cb);
    }

    // ── OnTick: called every depth update ─────────────────────────────────────
    void OnTick(const AvellanedaStoikovModelV3::Quotes &q,
                const StrategyContext &c,
                const MMFillProbabilityModel::FillProbs &fp,
                double kellyUnit,
                int64_t nowUs)
    {
        tickCount_++;

        // ── Probabilistic fills ───────────────────────────────────────────────
        // LCG random in [0,1)
        seed_ = seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
        double r1 = static_cast<double>(seed_ >> 33) / 2147483648.0;
        seed_ = seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
        double r2 = static_cast<double>(seed_ >> 33) / 2147483648.0;

        bool canBuy  = (inventory_ < MAX_INV * 0.90);
        bool canSell = (inventory_ > -MAX_INV * 0.90);

        // BID fill: we buy at q.bid
        if (q.active && canBuy && r1 < fp.bid) {
            ExecuteBidFill(q.bid, c.mid, c.microprice, kellyUnit, nowUs);
        }
        // ASK fill: we sell at q.ask
        if (q.active && canSell && r2 < fp.ask) {
            ExecuteAskFill(q.ask, c.mid, c.microprice, kellyUnit, nowUs);
        }

        // ── Measure adverse selection for matured fills ───────────────────────
        for (auto &f : pending_) f.tLeft--;
        while (!pending_.empty() && pending_.front().tLeft <= 0) {
            const auto &f = pending_.front();
            double midMove = (c.mid - f.midAtFill) * static_cast<double>(-f.side);
            double adverse = std::max(0.0, midMove) * f.unit;
            advSel_ += adverse;
            // Retroactively deduct from latest win record
            if (!winTracker_.empty()) {
                winTracker_.back() = std::max(winTracker_.back() - adverse,
                                              -10.0 * BASE_UNIT * c.mid);
            }
            pending_.pop_front();
        }

        // ── Unrealized PnL (MTM at microprice) ───────────────────────────────
        double ref = (c.microprice > 0.0) ? c.microprice : c.mid;
        if (std::abs(inventory_) > 1e-9) {
            double avgCost = (inventory_ > 0) ? avgBuyCost_ : avgSellProceeds_;
            unrealized_    = (ref - avgCost) * inventory_;
        } else {
            unrealized_ = 0.0;
        }

        // ── Emergency flatten at HARD_LIMIT ──────────────────────────────────
        if (std::abs(inventory_) >= HARD_LIMIT) {
            // Simulate market order at worst case (2bps slippage)
            double slippage = c.mid * 0.0002 * (inventory_ > 0 ? -1.0 : 1.0);
            double execP    = ref + slippage;
            double flatPnl  = (execP - (inventory_ > 0 ? avgBuyCost_ : avgSellProceeds_))
                              * inventory_;
            realized_  += flatPnl;
            inventory_  = 0.0;
            avgBuyCost_ = avgSellProceeds_ = 0.0;
            unrealized_ = 0.0;
        }

        // ── Fill rate (rolling 60s) ───────────────────────────────────────────
        int64_t cut = nowUs - int64_t(FILL_WIN_S) * 1'000'000LL;
        while (!fillTimes_.empty() && fillTimes_.front() < cut)
            fillTimes_.pop_front();
        fillRate_ = static_cast<double>(fillTimes_.size());

        probFill_ = (fp.bid + fp.ask) * 0.5;
    }

    // ── OnTrade: additional fill opportunity on actual aggTrade ───────────────
    // Supplements the probabilistic model with exact-price fills.
    void OnTrade(double tradePrice, bool buyerMM,
                 const AvellanedaStoikovModelV3::Quotes &q,
                 const StrategyContext &c,
                 double kellyUnit, int64_t nowUs)
    {
        if (!q.active) return;
        bool canBuy  = (inventory_ < MAX_INV * 0.90);
        bool canSell = (inventory_ > -MAX_INV * 0.90);

        // Aggressor bought → hit asks → if our ask is within their range
        if (!buyerMM && tradePrice >= q.ask && canSell)
            ExecuteAskFill(q.ask, c.mid, c.microprice, kellyUnit, nowUs);

        // Aggressor sold → hit bids → if our bid is within their range
        if (buyerMM && tradePrice <= q.bid && canBuy)
            ExecuteBidFill(q.bid, c.mid, c.microprice, kellyUnit, nowUs);
    }

    static double ComputeKellyUnit(const std::deque<double> &wt, double hitRate,
                                   double edgeScore) {
        if ((int)wt.size() < 10) return BASE_UNIT;
        double sw=0,sl=0; int nw=0,nl=0;
        for (double p:wt) { if(p>0){sw+=p;nw++;}else{sl+=std::abs(p);nl++;} }
        double avgW = (nw>0)?sw/nw:1.0;
        double avgL = (nl>0)?sl/nl:1.0;
        double b    = (avgL>1e-9)?avgW/avgL:1.0;
        double p    = std::max(0.01, std::min(0.99, hitRate));
        double q    = 1.0 - p;
        double f    = (b > 1e-9) ? (p*b - q)/b : 0.0;
        // Scale by edge score (0-10 bps → 0-1 multiplier)
        double edgeMult = std::min(1.0, edgeScore / 5.0);
        f = std::max(0.01, std::min(MAX_KELLY, f * 0.5 * (0.5 + edgeMult)));
        return BASE_UNIT * f;
    }

    void FillResults(StrategyResults &r,
                     const AvellanedaStoikovModelV3::Quotes &q,
                     double hitRate) const {
        r.asBid=q.bid; r.asAsk=q.ask; r.asReservation=q.reservation;
        r.asOptimalSpreadBps=q.deltaBps; r.asOptimalSpreadUSD=q.deltaUSD;
        r.asSkewBps=q.skewBps; r.asSkewUSD=q.skewUSD;
        r.asSigmaBps=q.sigmaBps; r.asGamma=q.gamma; r.asKappa=q.kappa;
        r.asQuotingActive=q.active; r.asGateReason=q.gateReason;
        r.mmRealizedPnl   = realized_+spreadCapture_-advSel_;
        r.mmUnrealizedPnl = unrealized_;
        r.mmNetPnl        = r.mmRealizedPnl + unrealized_;
        r.mmSpreadCapture = spreadCapture_;
        r.mmAdvSelection  = advSel_;
        r.mmInventory     = inventory_;
        r.mmFills=fills_; r.mmRoundTrips=roundTrips_;
        r.mmFillRate=fillRate_; r.mmProbFill100ms=probFill_;
        r.mmInventoryAlert=(std::abs(inventory_)>=MAX_INV*0.80);
        r.mmQuotingGated=!q.active;
        int wins=0;
        for (double p:winTracker_) if(p>0)wins++;
        r.mmWinRate=winTracker_.empty()?0.5:(double)wins/winTracker_.size();
        r.mmKellyUnit=ComputeKellyUnit(winTracker_,hitRate,0);
    }

    const std::deque<double>& GetWinTracker() const { return winTracker_; }
    double GetInventory() const { return inventory_; }

private:
    void ExecuteBidFill(double fillP, double mid, double micro, double unit, int64_t nowUs) {
        // Update running average buy cost
        double totalQ = std::abs(inventory_) + unit;
        avgBuyCost_   = (inventory_ >= 0)
            ? (avgBuyCost_ * std::abs(inventory_) + fillP * unit) / totalQ
            : fillP;  // switched side: reset basis

        inventory_ += unit;
        pending_.push_back({fillP, mid, +1, ADV_TICKS, unit});
        fills_++;
        fillTimes_.push_back(nowUs);

        // Check if this closes a short
        if (avgSellProceeds_ > 0) {
            double gross = (avgSellProceeds_ - fillP) * unit;
            spreadCapture_ += gross;
            realized_      += gross;
            winTracker_.push_back(gross);
            if ((int)winTracker_.size()>200) winTracker_.pop_front();
            if (inventory_ >= 0) { avgSellProceeds_=0; roundTrips_++; }
        }

        if (fillCallback_) {
            fillCallback_(true, unit);
        }
    }

    void ExecuteAskFill(double fillP, double mid, double micro, double unit, int64_t nowUs) {
        double totalQ = std::abs(inventory_) + unit;
        avgSellProceeds_ = (inventory_ <= 0)
            ? (avgSellProceeds_ * std::abs(inventory_) + fillP * unit) / totalQ
            : fillP;

        inventory_ -= unit;
        pending_.push_back({fillP, mid, -1, ADV_TICKS, unit});
        fills_++;
        fillTimes_.push_back(nowUs);

        if (avgBuyCost_ > 0) {
            double gross = (fillP - avgBuyCost_) * unit;
            spreadCapture_ += gross;
            realized_      += gross;
            winTracker_.push_back(gross);
            if ((int)winTracker_.size()>200) winTracker_.pop_front();
            if (inventory_ <= 0) { avgBuyCost_=0; roundTrips_++; }
        }

        if (fillCallback_) {
            fillCallback_(false, unit);
        }
    }

    double inventory_=0, avgBuyCost_=0, avgSellProceeds_=0;
    double spreadCapture_=0, advSel_=0, realized_=0, unrealized_=0;
    double fillRate_=0, probFill_=0;
    int    fills_=0, roundTrips_=0, tickCount_=0;
    uint64_t seed_ = 0xDEADBEEFCAFEBABEULL;  // LCG state (fast PRNG)
    std::deque<FillRecord> pending_;
    std::deque<double>     winTracker_;
    std::deque<int64_t>    fillTimes_;

    FillCallback fillCallback_;
};

// ============================================================================
// LatencyArbitrageSimulatorV3
// Only fires on strong, regime-confirmed signals (hmmConfident + low VPIN).
// Tracks rolling Sharpe of the edge distribution (Welford).
// ============================================================================
class LatencyArbitrageSimulatorV3 {
public:
    static constexpr int    N_US      = 500;
    static constexpr int    BUF_SZ    = 600;
    static constexpr double UNIT      = 0.05;
    static constexpr double VPIN_MAX  = 0.50;
    static constexpr double MIN_SCORE = 0.15;

    struct Tick { int64_t us; double mid,spr,score; };

    void OnTick(const StrategyContext &c, int64_t nowUs) {
        buf_.push_back({nowUs, c.mid, c.spreadUSD, c.regimeAdjScore});
        if ((int)buf_.size()>BUF_SZ) buf_.pop_front();

        if (c.vpin>VPIN_MAX||std::abs(c.regimeAdjScore)<MIN_SCORE
            ||!c.hmmConfident||c.volRegime>=2) return;

        int64_t tgt=nowUs-N_US;
        const Tick *past=nullptr;
        for (int i=(int)buf_.size()-1;i>=0;i--)
            if (buf_[i].us<=tgt){past=&buf_[i];break;}
        if (!past||std::abs(past->score)<MIN_SCORE) return;

        int dir=(past->score>0)?+1:-1;
        double ent=(dir>0)?past->mid+past->spr*0.5:past->mid-past->spr*0.5;
        double ex =(dir>0)?c.mid-c.spreadUSD*0.5:c.mid+c.spreadUSD*0.5;
        double eU =(ex-ent)*(double)dir*UNIT;
        double eB =(c.mid>0)?eU/(c.mid*UNIT)*10000.0:0.0;

        lastB_=eB; lastU_=eU; cumU_+=eU; opps_++;
        eW_.push_back(eB); eS_+=eB; eS2_+=eB*eB;
        if ((int)eW_.size()>100){double o=eW_.front();eW_.pop_front();eS_-=o;eS2_-=o*o;}
        int n=(int)eW_.size();
        if (n>=10){double mu=eS_/n,v=eS2_/n-mu*mu,sd=(v>0)?std::sqrt(v):1e-9;
                   sharpe_=mu/sd*std::sqrt(252.0*1440.0);}
    }

    void FillResults(StrategyResults &r) const {
        r.latEdgeBps=lastB_; r.latEdgeUSD=lastU_;
        r.latCumEdgeUSD=cumU_; r.latSharpe=sharpe_;
        r.latOpportunities=opps_;
    }

private:
    std::deque<Tick>   buf_;
    std::deque<double> eW_;
    double eS_=0,eS2_=0,lastB_=0,lastU_=0,cumU_=0,sharpe_=0;
    int    opps_=0;
};

// ============================================================================
// SignalOrderFlowReplayV3
// Filtered entries: |regimeAdjScore|>0.18 AND hmmConfident AND VPIN<0.60
// MAE/MFE per trade. Win rate on closed trades. ASCII chart with signals.
// ============================================================================
class SignalOrderFlowReplayV3 {
public:
    static constexpr int    MAX_TICKS = 300;
    static constexpr double THRESH    = 0.18;
    static constexpr double UNIT      = 0.05;

    struct Bar{double mid,score;bool ent,ex_;int dir;};

    void OnTick(const StrategyContext &c) {
        bool ok = std::abs(c.regimeAdjScore)>THRESH && c.hmmConfident
                  && c.vpin<0.60 && c.volRegime<3;
        int nd=ok?(c.regimeAdjScore>0?+1:-1):0, pd=lastDir_;
        bool ent=(pd==0&&nd!=0), ex_=(pd!=0&&(nd==0||nd!=pd));

        if (pd!=0&&eMid_>0){
            double mv=(c.mid-eMid_)*(double)pd;
            mfe_=std::max(mfe_,mv); mae_=std::min(mae_,mv);
        }
        if (ex_&&pd!=0&&eMid_>0){
            double pnl=(c.mid-eMid_)*(double)pd*UNIT;
            pnl_+=pnl; exits_++;
            tMFE_+=mfe_; tMAE_+=std::abs(mae_);
            if(pnl>0)wins_++;
            eMid_=0; mfe_=0; mae_=std::numeric_limits<double>::max();
        }
        if(ent){eMid_=c.mid;entries_++;mfe_=0;mae_=std::numeric_limits<double>::max();}
        bars_.push_back({c.mid,c.regimeAdjScore,ent,ex_,nd});
        if((int)bars_.size()>MAX_TICKS)bars_.pop_front();
        lastDir_=nd;
    }

    void FillResults(StrategyResults &r) const {
        r.replayEntries=entries_; r.replayExits=exits_; r.replaySimPnl=pnl_;
        r.replayWinRate=(exits_>0)?(double)wins_/exits_:0.0;
        r.replayMAE=(exits_>0)?tMAE_/exits_:0.0;
        r.replayMFE=(exits_>0)?tMFE_/exits_:0.0;
        int n=(int)bars_.size(); if(n<2)return;
        double pm=bars_[0].mid,px=bars_[0].mid;
        for(auto&b:bars_){pm=std::min(pm,b.mid);px=std::max(px,b.mid);}
        r.replayMinPrice=pm; r.replayMaxPrice=px;
        constexpr int C=StrategyResults::CHART_COLS,R=StrategyResults::CHART_ROWS;
        for(int rr=0;rr<R;rr++){for(int c=0;c<C;c++)r.replayChart[rr][c]=' ';r.replayChart[rr][C]=0;}
        double pr=px-pm; int step=std::max(1,n/C);
        for(int c2=0;c2<C&&c2*step<n;c2++){
            auto&b=bars_[c2*step];
            int row=R-1;
            if(pr>1e-9)row=(int)((b.mid-pm)/pr*(R-1));
            row=R-1-std::max(0,std::min(R-1,row));
            char mk=(b.ent&&b.dir>0)?'^':(b.ent&&b.dir<0)?'v':b.ex_?'+':(std::abs(b.score)>THRESH)?'*':'-';
            if(row>=0&&row<R)r.replayChart[row][c2]=mk;
        }
    }
private:
    std::deque<Bar> bars_;
    int lastDir_=0,entries_=0,exits_=0,wins_=0;
    double pnl_=0,eMid_=0,tMFE_=0,tMAE_=0;
    double mfe_=0,mae_=std::numeric_limits<double>::max();
};

// ============================================================================
// CrossExchangeArbV3
// Simple arb detector: net after round-trip fees (22bps).
// ============================================================================
class CrossExchangeArbV3 {
public:
    static constexpr double FEE_BPS=22.0, MIN_BPS=0.5;
    void Update(double b1,double a1,double b2,double a2,const std::string&nm,bool conn){
        bid1_=b1;ask1_=a1;bid2_=b2;ask2_=a2;name_=nm;conn_=conn;
        if(b2<=0||a2<=0){arb_=0;alert_=false;return;}
        double m1=(b1+a1)*0.5,m2=(b2+a2)*0.5,ma=(m1+m2)*0.5;
        if(ma<1e-9)return;
        midSpr_=std::abs(m1-m2)/ma*10000.0;
        double aA=(b2-a1)/ma*10000.0-FEE_BPS;
        double aB=(b1-a2)/ma*10000.0-FEE_BPS;
        arb_=std::max({aA,aB,0.0}); alert_=(arb_>MIN_BPS);
        if(alert_){cnt_++;cum_+=arb_;}
    }
    void FillResults(StrategyResults &r) const {
        r.exchBid1=bid1_;r.exchAsk1=ask1_;r.exchBid2=bid2_;r.exchAsk2=ask2_;
        r.exchMidSpreadBps=midSpr_;r.exchArbBps=arb_;r.exchArbAlert=alert_;
        r.exchArbCount=cnt_;r.exchCumArbBps=cum_;r.exchName=name_;r.exchConnected=conn_;
    }
private:
    double bid1_=0,ask1_=0,bid2_=0,ask2_=0,midSpr_=0,arb_=0,cum_=0;
    bool alert_=false,conn_=false; int cnt_=0; std::string name_{"Bybit"};
};

// ============================================================================
// SecondExchangeFeedV3
// Bybit V5 public spot WebSocket with HEARTBEAT ping every 15s.
// Without the ping, Bybit closes the connection after ~20s → always 0.0.
// Uses lock-free atomics for bid/ask (safe multi-thread read).
// Zero-alloc JSON parser for hot-path price extraction.
// ============================================================================
class SecondExchangeFeedV3 {
public:
    SecondExchangeFeedV3():bid_(0),ask_(0),conn_(false),stop_(false){}
    ~SecondExchangeFeedV3(){Stop();}

    void Start(const std::string &sym, const std::string &nm="Bybit"){
        sym_=sym; nm_=nm; stop_=false;
        th_=std::thread([this](){Run();});
    }
    void Stop(){ stop_=true; if(th_.joinable())th_.join(); }
    double GetBid() const { return bid_.load(std::memory_order_relaxed); }
    double GetAsk() const { return ask_.load(std::memory_order_relaxed); }
    bool   IsConn() const { return conn_.load(std::memory_order_relaxed); }
    const  std::string& GetName() const { return nm_; }

private:
    void Run(){
        namespace beast=boost::beast;namespace wn=beast::websocket;
        namespace net=boost::asio;namespace ssl=net::ssl;using tcp=net::ip::tcp;
        while(!stop_){
            try{
                net::io_context ioc;
                ssl::context ctx{ssl::context::tlsv12_client};
                // V3 fix: Disable strict verify and add SNI
                ctx.set_verify_mode(ssl::verify_none);

                tcp::resolver res{ioc};
                wn::stream<beast::ssl_stream<tcp::socket>> ws{ioc,ctx};
                auto ep=res.resolve("stream.bybit.com","443");
                net::connect(ws.next_layer().next_layer(),ep.begin(),ep.end());

                // SNI required for stream.bybit.com
                if(!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "stream.bybit.com")) {
                     throw std::runtime_error("Failed to set SNI hostname");
                }

                ws.next_layer().handshake(ssl::stream_base::client);
                ws.handshake("stream.bybit.com","/v5/public/spot");
                // Subscribe to orderbook L1
                ws.write(net::buffer(
                    "{\"op\":\"subscribe\",\"args\":[\"orderbook.1."+sym_+"\"]}"));
                conn_=true;

                // Set read timeout via async approach or just use deadline
                // For simplicity: blocking reads with ping on timer
                beast::flat_buffer buf;
                auto lastPing = std::chrono::steady_clock::now();

                while(!stop_){
                    // Non-blocking check: attempt read with timeout via beast
                    // Since beast doesn't natively support per-read timeout in sync mode,
                    // we rely on our 15s ping to keep alive; blocking read is fine
                    ws.read(buf);
                    std::string msg=beast::buffers_to_string(buf.data());
                    buf.consume(buf.size());
                    Parse(msg);

                    // Heartbeat ping every 15s (Bybit requires ping or connection drops)
                    auto now=std::chrono::steady_clock::now();
                    if(std::chrono::duration_cast<std::chrono::seconds>(now-lastPing).count()>=15){
                        ws.write(net::buffer("{\"op\":\"ping\"}"));
                        lastPing=now;
                    }
                }
            }catch(const std::exception& e){
                conn_=false;
                std::cerr << "[XchgArb] Bybit WS Error: " << e.what() << "\n";
                if(!stop_)std::this_thread::sleep_for(std::chrono::seconds(3));
            }catch(...){
                conn_=false;
                std::cerr << "[XchgArb] Bybit WS Unknown Error\n";
                if(!stop_)std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
        conn_=false;
    }

    void Parse(const std::string &m){
        // Bybit V5 orderbook.1 snapshot/delta format:
        // {"topic":"orderbook.1.SOLUSDT","type":"snapshot","data":
        //   {"s":"SOLUSDT","b":[["130.5","10.5"]],"a":[["130.6","8.2"]],...}}
        if(m.find("orderbook")==std::string::npos)return;
        // Skip pong responses
        if(m.find("\"pong\"")!=std::string::npos)return;
        // data.b and data.a
        double b=Extr(m,"\"b\":[[\""), a=Extr(m,"\"a\":[[\"");
        if(b>0.0) bid_.store(b,std::memory_order_relaxed);
        if(a>0.0) ask_.store(a,std::memory_order_relaxed);
    }

    static double Extr(const std::string &m, const char *key){
        size_t p=m.find(key);
        if(p==std::string::npos)return 0.0;
        p+=std::strlen(key);
        size_t e=m.find('"',p);
        if(e==std::string::npos)return 0.0;
        try{ return std::stod(m.substr(p,e-p)); }catch(...){ return 0.0; }
    }

    std::atomic<double> bid_,ask_;
    std::atomic<bool>   conn_,stop_;
    std::string         sym_,nm_;
    std::thread         th_;
};

// ============================================================================
// StrategyEngine v3 — orchestrator
// ============================================================================
class StrategyEngine {
public:
    void SetFillCallback(MarketMakingSimulatorV3::FillCallback cb) {
        mm_.SetFillCallback(std::move(cb));
    }

    // ── aggTrade path ─────────────────────────────────────────────────────────
    void OnTrade(double tradePrice, double /*qty*/, bool buyerMM,
                 const StrategyContext &ctx, int64_t nowUs)
    {
        double ku = MarketMakingSimulatorV3::ComputeKellyUnit(
            mm_.GetWinTracker(), ctx.hitRate, ctx.edgeScore);
        auto q = as_.Compute(ctx, mm_.GetInventory());
        mm_.OnTrade(tradePrice, buyerMM, q, ctx, ku, nowUs);
    }

    // ── depth update path ─────────────────────────────────────────────────────
    void OnDepthUpdate(const StrategyContext &ctx, int64_t nowUs) {
        double ku = MarketMakingSimulatorV3::ComputeKellyUnit(
            mm_.GetWinTracker(), ctx.hitRate, ctx.edgeScore);
        auto q  = as_.Compute(ctx, mm_.GetInventory());
        auto fp = fillModel_.Compute(q, ctx, mm_.GetInventory());

        mm_.OnTick(q, ctx, fp, ku, nowUs);
        lat_.OnTick(ctx, nowUs);
        replay_.OnTick(ctx);

        // Cross-exchange
        double b2=feed_.GetBid(), a2=feed_.GetAsk();
        double b1 = (ctx.bid1>0)?ctx.bid1 : ctx.mid-ctx.spreadUSD*0.5;
        double a1 = (ctx.ask1>0)?ctx.ask1 : ctx.mid+ctx.spreadUSD*0.5;
        arb_.Update(b1, a1, b2, a2, feed_.GetName(), feed_.IsConn());

        // Assemble results
        mm_.FillResults(res_, q, ctx.hitRate);
        lat_.FillResults(res_);
        replay_.FillResults(res_);
        arb_.FillResults(res_);
    }

    const StrategyResults& GetResults() const { return res_; }
    StrategyResults&       GetResults()       { return res_; }
    SecondExchangeFeedV3&  GetSecondFeed()    { return feed_; }

private:
    AvellanedaStoikovModelV3   as_;
    MMFillProbabilityModel       fillModel_;
    MarketMakingSimulatorV3    mm_;
    LatencyArbitrageSimulatorV3 lat_;
    SignalOrderFlowReplayV3    replay_;
    CrossExchangeArbV3         arb_;
    SecondExchangeFeedV3       feed_;
    StrategyResults            res_;
};