#pragma once
// ============================================================================
// BookDynamicsEngine.h  —  HFT Book Dynamics & Order Flow Analytics  v1
//
// Dependencies (must be included before this header in every TU):
//   MarketMicrostructure.h  → TradeSide
//   MarketTypes.h           → PriceLevelSnapshot, Price, Quantity
//   SignalEngine.h          → TopOfBook
//
// Feature                      Algorithm / Method                    Cost
// ─────────────────────────────────────────────────────────────────────────
// P1. BookPressureHeatmap       2D rolling matrix: time×price         O(W×B)
//     time=rolling 30 ticks, price=±40 buckets at BUCKET_USD granularity
//     Cell value = EWM-smoothed qty; display as ASCII intensity chars
//
// P1. LevelLifetimeTracker      Per-level birth/death timestamps       O(L)
//     birth  = first tick price P appears in snap
//     death  = first tick price P disappears from snap
//     lifetime histograms: <100ms / 100ms-1s / 1s-10s / 10s-60s / >60s
//     Short-lived (<1s) = HFT quote spam
//     Long-lived  (>60s) = institutional resting order
//
// P2. DepthMigrationVelocity    Bid/ask "wall" (peak-qty level)        O(L)
//     tracking via circular buffer + OLS velocity regression
//     v_wall = dP_wall/dt  (USD/s)
//     Compression: bidWall ↑ AND askWall ↓ → directional squeeze
//
// P2. OrderBookGradient         dQ/dP via finite differences           O(L)
//     Cumulative qty C[i] = Σ q[0..i], dC/dP[i] = ΔC[i]/ΔP[i]
//     Steepness index = max(|dQ/dP|) / mean(|dQ/dP|)
//     Cliff detection: single level holds disproportionate qty
//
// P3. HiddenLiquidityEstimator  Trade-vs-book exceedance detection     O(1)
//     hidden_vol = max(0, trade_qty - visible_qty_at_P)
//     Phantom ratio = cumulative_hidden / cumulative_visible
//     Aggregated per price bucket (±0.1 USD grid)
//
// Thread-safety: NONE — single processing thread.
// All prices/quantities in USD / base-asset (NOT ×10000 encoded).
// ============================================================================

#include "MarketTypes.h"     // PriceLevelSnapshot, Price, Quantity
#include "SignalEngine.h"    // TopOfBook
#include "SIMDMath.h"        // SIMD-optimized math operations

#include <deque>
#include <vector>
#include <unordered_map>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <string>
#include <sstream>

// ─── All book dynamics results in one flat struct ─────────────────────────────
struct BookDynamicsResults {
    // ── P1: Book Pressure Heatmap ──────────────────────────────────────────────
    // 5×5 compressed display matrix (row=time recent→old, col=price low→high)
    // Each entry is qty intensity normalised to [0,4] for ASCII palette
    static constexpr int HM_ROWS = 5;    // time buckets (recent to old)
    static constexpr int HM_COLS = 10;   // price buckets around mid
    uint8_t  heatmap[HM_ROWS][HM_COLS] = {};  // 0-4 intensity
    double   heatmapMidPrice = 0.0;            // mid price at last snapshot
    double   heatmapBucketUSD = 0.0;           // USD width of each price bucket

    // ── P1: Level Lifetime Statistics ─────────────────────────────────────────
    double avgBidLifetimeMs    = 0.0;
    double avgAskLifetimeMs    = 0.0;
    // Histogram buckets: <100ms, 100ms-1s, 1-10s, 10-60s, >60s
    int    bidLifeHist[5]      = {};
    int    askLifeHist[5]      = {};
    int    shortLivedBid       = 0;   // <1s bid levels (total observed)
    int    shortLivedAsk       = 0;
    int    longLivedBid        = 0;   // >60s bid levels (total observed)
    int    longLivedAsk        = 0;
    int    totalDeadBid        = 0;
    int    totalDeadAsk        = 0;

    // ── P2: Depth Migration Velocity ──────────────────────────────────────────
    double bidWallPrice        = 0.0;  // current price of max-qty bid level
    double bidWallQty          = 0.0;
    double askWallPrice        = 0.0;
    double askWallQty          = 0.0;
    double bidWallVelocity     = 0.0;  // USD/s (>0 = wall moving up/away from mid)
    double askWallVelocity     = 0.0;  // USD/s (<0 = wall moving down/toward mid)
    bool   compressionAlert    = false; // both walls compressing mid
    double compressionRateUSD  = 0.0;  // |bidVel| + |askVel| when compressing

    // ── P2: Order Book Gradient dQ/dP ─────────────────────────────────────────
    double bidGradientMean     = 0.0;  // mean |dQ/dP| over bid side (units/$)
    double askGradientMean     = 0.0;
    double bidGradientMax      = 0.0;  // max single-level gradient (cliff detection)
    double askGradientMax      = 0.0;
    double bidSteepnessIdx     = 0.0;  // max/mean ratio: >3 = cliff present
    double askSteepnessIdx     = 0.0;
    int    bidCliffLevel       = -1;   // index of bid cliff (-1 = none)
    int    askCliffLevel       = -1;
    bool   thinBookAlert       = false; // mean gradient above THIN_THRESH

    // ── P3: Hidden Liquidity Estimator ────────────────────────────────────────
    double hiddenVolCumulative = 0.0;  // all-time cumulative hidden vol (base units)
    double visibleVolCumulative= 0.0;  // all-time visible vol at matched levels
    double phantomRatio        = 0.0;  // hidden / visible (rolling)
    double lastHiddenPrice     = 0.0;
    double lastExceedanceRatio = 0.0;  // trade_qty / visible_qty at P
    double lastExceedanceVol   = 0.0;  // hidden volume on last detection
    int    hiddenDetectCount   = 0;
    bool   hiddenAlert         = false;
};

// ============================================================================
// P1. BookPressureHeatmap
//
// Maintains a rolling 2D matrix: rows = time (last N_TICKS snapshots),
// cols = price buckets centred on mid price.
//
// Price bucketing:
//   bucket_index = round((price - mid_anchor) / BUCKET_USD) + N_HALF_COLS
//   N_HALF_COLS = 20 → 40 total columns → display compressed to HM_COLS
//
// Cell value = EWM of qty at that (time,price) cell:
//   cell = EWM_ALPHA × raw_qty + (1-EWM_ALPHA) × prev_cell
//
// At each tick:
//   1. Rotate rows (push new row, pop oldest).
//   2. For every level in snap, map to column index.
//   3. Fill new row with qty values.
//   4. Build compressed 5×10 display matrix by:
//      - grouping full matrix rows into 5 temporal buckets
//      - grouping full matrix cols into 10 spatial buckets
//      - normalise each cell to [0,4] using 80th percentile as max
// ============================================================================
class BookPressureHeatmap {
public:
    static constexpr int    N_TICKS      = 30;   // rolling time window
    static constexpr int    N_FULL_COLS  = 40;   // ±20 buckets around mid
    static constexpr int    N_HALF_COLS  = 20;
    static constexpr double EWM_ALPHA    = 0.40;
    static constexpr double BUCKET_USD   = 0.10; // $0.10 per price bucket
    static constexpr double THIN_THRESH  = 3.0;  // steepness index threshold

    BookPressureHeatmap() {
        for (auto &row : matrix_) row.fill(0.0);
    }

    void OnSnapshot(const PriceLevelSnapshot &snap, double midPrice) {
        if (snap.bids.empty() && snap.asks.empty()) return;

        midAnchor_ = midPrice;

        // Build new row from snapshot
        std::array<double, N_FULL_COLS> newRow;
        newRow.fill(0.0);

        // Bid levels (qty on buy side)
        for (const auto &[px, qty] : snap.bids) {
            double priceUSD = static_cast<double>(px) / 10000.0;
            int col = ColIndex(priceUSD);
            if (col >= 0 && col < N_FULL_COLS)
                newRow[col] += static_cast<double>(qty) / 10000.0;
        }
        // Ask levels (qty on sell side) — stored in same columns
        for (const auto &[px, qty] : snap.asks) {
            double priceUSD = static_cast<double>(px) / 10000.0;
            int col = ColIndex(priceUSD);
            if (col >= 0 && col < N_FULL_COLS)
                newRow[col] += static_cast<double>(qty) / 10000.0;
        }

        // Rotate: apply EWM to new row vs previous newest row
        if (tickCount_ > 0) {
            const auto &prev = matrix_[0];
            // SIMD-optimized EWM update (process 4 doubles at once)
            SIMD::EWMUpdateAVX2(newRow.data(), prev.data(), EWM_ALPHA, N_FULL_COLS);
        }

        // Push new row to front, discard oldest
        if (tickCount_ < N_TICKS) {
            matrix_[tickCount_] = newRow;
            tickCount_++;
        } else {
            // Rotate backward (oldest at index N_TICKS-1)
            for (int r = N_TICKS - 1; r > 0; --r) matrix_[r] = matrix_[r-1];
            matrix_[0] = newRow;
        }
    }

    // Build compressed BookDynamicsResults heatmap display (5×10)
    void FillDisplayMatrix(BookDynamicsResults &res) const {
        res.heatmapMidPrice  = midAnchor_;
        res.heatmapBucketUSD = BUCKET_USD;

        if (tickCount_ == 0) return;

        static constexpr int DR = BookDynamicsResults::HM_ROWS;
        static constexpr int DC = BookDynamicsResults::HM_COLS;

        // Rows: compress tickCount_ time slices into DR=5 groups
        // Cols: compress N_FULL_COLS=40 price buckets into DC=10 groups
        double compressed[DR][DC] = {};
        int    rowsUsed = std::min(tickCount_, N_TICKS);
        int    rowsPerGrp = std::max(1, rowsUsed / DR);
        int    colsPerGrp = N_FULL_COLS / DC;

        for (int dr = 0; dr < DR; ++dr) {
            int r0 = dr * rowsPerGrp;
            int r1 = std::min(r0 + rowsPerGrp, rowsUsed);
            for (int dc = 0; dc < DC; ++dc) {
                int c0 = dc * colsPerGrp;
                int c1 = c0 + colsPerGrp;

                // SIMD-optimized 2D sum for each [dr][dc] block
                // Sum across rows using SIMD
                double sum = 0.0;
                int cnt = 0;
                for (int r = r0; r < r1; ++r) {
                    // Use SIMD sum for the column range
                    sum += SIMD::SumAVX2(&matrix_[r][c0], c1 - c0);
                    cnt += (c1 - c0);
                }
                compressed[dr][dc] = (cnt > 0) ? sum / cnt : 0.0;
            }
        }

        // Find 80th percentile for normalisation
        std::vector<double> vals;
        vals.reserve(DR * DC);
        for (int r = 0; r < DR; ++r)
            for (int c = 0; c < DC; ++c)
                if (compressed[r][c] > 1e-9) vals.push_back(compressed[r][c]);

        double norm = 1.0;
        if (!vals.empty()) {
            std::sort(vals.begin(), vals.end());
            size_t p80 = static_cast<size_t>(vals.size() * 0.80);
            norm = vals[std::min(p80, vals.size()-1)];
            if (norm < 1e-9) norm = 1.0;
        }

        for (int r = 0; r < DR; ++r)
            for (int c = 0; c < DC; ++c) {
                double ratio = compressed[r][c] / norm;
                res.heatmap[r][c] = static_cast<uint8_t>(
                    std::min(4.0, std::floor(ratio * 4.0)));
            }
    }

private:
    int    ColIndex(double priceUSD) const {
        if (midAnchor_ < 1e-6) return -1;
        int idx = static_cast<int>(std::round(
            (priceUSD - midAnchor_) / BUCKET_USD)) + N_HALF_COLS;
        return idx;
    }

    std::array<std::array<double, N_FULL_COLS>, N_TICKS> matrix_;
    double midAnchor_ = 0.0;
    int    tickCount_ = 0;
};

// ============================================================================
// P1. LevelLifetimeTracker
//
// Each price level P has:
//   birthUs  = timestamp_us when P first appeared
//   lastSeenUs = last tick P was present (used to detect death)
//
// On each snapshot:
//   1. For every P in currentSnap: if not in births_ → record birth
//   2. For every P in prevSnap NOT in currentSnap → level died:
//      lifetime = nowUs - birthUs[P]
//      update histogram, running mean
//
// Histogram bins (microseconds):
//   [0]  < 100ms   (< 100,000 μs)
//   [1]  100ms-1s
//   [2]  1s-10s
//   [3]  10s-60s
//   [4]  > 60s
//
// Memory: capped at MAX_TRACKED levels per side to prevent unbounded growth.
// ============================================================================
class LevelLifetimeTracker {
public:
    static constexpr int MAX_TRACKED = 2000;  // per side

    struct SideStats {
        double   sumLifetimeMs   = 0.0;
        int      totalDead       = 0;
        int      shortLived      = 0;   // <1s
        int      longLived       = 0;   // >60s
        int      hist[5]         = {};  // lifetime histogram

        void RecordDeath(int64_t lifetimeUs) {
            double ms = static_cast<double>(lifetimeUs) / 1000.0;
            sumLifetimeMs += ms;
            totalDead++;
            if (lifetimeUs < 1'000'000LL)   shortLived++;
            if (lifetimeUs > 60'000'000LL)  longLived++;
            // Histogram
            if      (lifetimeUs < 100'000LL)     hist[0]++;
            else if (lifetimeUs < 1'000'000LL)   hist[1]++;
            else if (lifetimeUs < 10'000'000LL)  hist[2]++;
            else if (lifetimeUs < 60'000'000LL)  hist[3]++;
            else                                  hist[4]++;
        }
        double AvgLifetimeMs() const {
            return totalDead > 0 ? sumLifetimeMs / totalDead : 0.0;
        }
    };

    // Call every depth tick with the latest snapshot
    void OnSnapshot(const PriceLevelSnapshot &snap, int64_t nowUs) {
        // Bid side
        UpdateSide(snap.bids, nowUs, bidBirths_, bidPrev_, bidStats_);
        // Ask side
        UpdateSide(snap.asks, nowUs, askBirths_, askPrev_, askStats_);
    }

    void FillResults(BookDynamicsResults &res) const {
        res.avgBidLifetimeMs = bidStats_.AvgLifetimeMs();
        res.avgAskLifetimeMs = askStats_.AvgLifetimeMs();
        res.shortLivedBid    = bidStats_.shortLived;
        res.shortLivedAsk    = askStats_.shortLived;
        res.longLivedBid     = bidStats_.longLived;
        res.longLivedAsk     = askStats_.longLived;
        res.totalDeadBid     = bidStats_.totalDead;
        res.totalDeadAsk     = askStats_.totalDead;
        for (int i = 0; i < 5; ++i) {
            res.bidLifeHist[i] = bidStats_.hist[i];
            res.askLifeHist[i] = askStats_.hist[i];
        }
    }

private:
    template<typename MapT>
    void UpdateSide(const MapT &currentLevels, int64_t nowUs,
                    std::unordered_map<int64_t,int64_t> &births,
                    std::unordered_map<int64_t,int64_t> &prev,
                    SideStats &stats)
    {
        // Build current set
        std::unordered_map<int64_t,int64_t> current;
        current.reserve(currentLevels.size());
        for (const auto &[px, qty] : currentLevels)
            current[static_cast<int64_t>(px)] = nowUs;

        // Deaths: in prev but not in current
        for (const auto &[px, _] : prev) {
            if (current.find(px) == current.end()) {
                auto bit = births.find(px);
                if (bit != births.end()) {
                    int64_t lifetime = nowUs - bit->second;
                    if (lifetime >= 0) stats.RecordDeath(lifetime);
                    births.erase(bit);
                }
            }
        }

        // Births: in current but not in births (first seen)
        for (const auto &[px, _] : current) {
            if (births.find(px) == births.end()) {
                if (static_cast<int>(births.size()) < MAX_TRACKED)
                    births[px] = nowUs;
            }
        }

        prev = std::move(current);
    }

    std::unordered_map<int64_t,int64_t> bidBirths_, bidPrev_;
    std::unordered_map<int64_t,int64_t> askBirths_, askPrev_;
    SideStats bidStats_, askStats_;
};

// ============================================================================
// P2. DepthMigrationVelocity
//
// "Wall" = the price level holding maximum visible quantity on each side.
//
//   bidWall = argmax(qty) over all bid levels
//   askWall = argmax(qty) over all ask levels
//
// Velocity estimation via rolling OLS over last WINDOW ticks:
//
//   For the bid wall: {(t_i, P_wall_i)} for i = 0..N-1
//   v = [N·Σ(t·P) − Σt·ΣP] / [N·Σt² − (Σt)²]   (slope from OLS)
//
//   v_bid > 0 → bid wall moving up (away from mid) → ask pressure
//   v_bid < 0 → bid wall moving toward mid → compression
//
//   v_ask < 0 → ask wall moving down (toward mid) → compression
//   v_ask > 0 → ask wall moving up (away from mid) → bid pressure
//
// Compression alert: v_bid < 0 AND v_ask < 0 simultaneously
//   compression_rate = |v_bid| + |v_ask| (USD/s squeeze rate)
//
// WINDOW = 20 ticks = 2 seconds at 100ms depth stream.
// ============================================================================
class DepthMigrationVelocity {
public:
    static constexpr int WINDOW = 20;

    void OnSnapshot(const PriceLevelSnapshot &snap, int64_t nowUs) {
        if (snap.bids.empty() || snap.asks.empty()) return;

        // Find bid wall (max qty level)
        double bidWallP = 0.0, bidWallQ = 0.0;
        for (const auto &[px, qty] : snap.bids) {
            double q = static_cast<double>(qty) / 10000.0;
            if (q > bidWallQ) {
                bidWallQ = q;
                bidWallP = static_cast<double>(px) / 10000.0;
            }
        }

        // Find ask wall (max qty level)
        double askWallP = 0.0, askWallQ = 0.0;
        for (const auto &[px, qty] : snap.asks) {
            double q = static_cast<double>(qty) / 10000.0;
            if (q > askWallQ) {
                askWallQ = q;
                askWallP = static_cast<double>(px) / 10000.0;
            }
        }

        bidWallPrice_ = bidWallP;
        bidWallQty_   = bidWallQ;
        askWallPrice_ = askWallP;
        askWallQty_   = askWallQ;

        // Push to rolling buffers
        double t = static_cast<double>(nowUs) * 1e-6;  // convert to seconds
        bidBuf_.push_back({t, bidWallP});
        askBuf_.push_back({t, askWallP});
        if (static_cast<int>(bidBuf_.size()) > WINDOW) bidBuf_.pop_front();
        if (static_cast<int>(askBuf_.size()) > WINDOW) askBuf_.pop_front();

        bidVel_ = OLSSlope(bidBuf_);
        askVel_ = OLSSlope(askBuf_);
    }

    void FillResults(BookDynamicsResults &res) const {
        res.bidWallPrice     = bidWallPrice_;
        res.bidWallQty       = bidWallQty_;
        res.askWallPrice     = askWallPrice_;
        res.askWallQty       = askWallQty_;
        res.bidWallVelocity  = bidVel_;
        res.askWallVelocity  = askVel_;
        // Compression: bid wall moving up (≥0), ask wall moving down (≤0)
        // Note: bid wall moving up = compressing mid from below
        // Ask wall moving down = compressing mid from above
        bool bidCompress = (bidVel_ > 0.0);
        bool askCompress = (askVel_ < 0.0);
        res.compressionAlert   = bidCompress && askCompress;
        res.compressionRateUSD = res.compressionAlert
            ? std::abs(bidVel_) + std::abs(askVel_)
            : 0.0;
    }

private:
    struct TVal { double t, v; };

    static double OLSSlope(const std::deque<TVal> &buf) {
        int n = static_cast<int>(buf.size());
        if (n < 3) return 0.0;

        // Extract data into contiguous arrays for SIMD
        std::vector<double> t(n), v(n);
        int i = 0;
        for (const auto &e : buf) {
            t[i] = e.t;
            v[i] = e.v;
            i++;
        }

        // Use SIMD-optimized OLS (4x speedup)
        return SIMD::OLSSlopeAVX2(t.data(), v.data(), n);
    }

    std::deque<TVal> bidBuf_, askBuf_;
    double bidWallPrice_ = 0.0, bidWallQty_ = 0.0;
    double askWallPrice_ = 0.0, askWallQty_ = 0.0;
    double bidVel_       = 0.0;
    double askVel_       = 0.0;
};

// ============================================================================
// P2. OrderBookGradient  (dQ/dP)
//
// Cumulative quantity from mid outward:
//   Bid side (prices descending from mid):
//     C_bid[0] = q_bid[0]
//     C_bid[i] = C_bid[i-1] + q_bid[i]
//
//   Ask side (prices ascending from mid):
//     C_ask[0] = q_ask[0]
//     C_ask[i] = C_ask[i-1] + q_ask[i]
//
// Gradient at level i (units: base_asset / USD):
//   dC_bid/dP[i] = (C_bid[i] - C_bid[i-1]) / |P_bid[i] - P_bid[i-1]|
//                = q_bid[i] / |ΔP|   (since ΔC = q_bid[i])
//
//   dC_ask/dP[i] = q_ask[i] / |ΔP|
//
// This is the marginal liquidity density at each price step.
//
// Metrics:
//   mean    = (1/N) Σ dC/dP[i]           (average depth density)
//   max     = max(dC/dP[i])              (cliff level)
//   steepness = max / mean                (cliff concentration ratio)
//     >3: one level holds 3× average → cliff present
//     >5: extreme cliff (large resting order or iceberg anchor)
//
//   thinBookAlert: mean < THIN_THRESHOLD  (very few units per dollar of depth)
//
// Uses top N_LEVELS from TopOfBook (L1-L5); also scans deeper if available.
// ============================================================================
class OrderBookGradient {
public:
    static constexpr int    N_LEVELS      = 20;    // scan up to 20 levels
    static constexpr double THIN_THRESH   = 10.0;  // units/$: below = thin book
    static constexpr double CLIFF_THRESH  = 3.0;   // steepness > 3 = cliff

    void OnSnapshot(const PriceLevelSnapshot &snap) {
        ComputeSide(snap.bids, bidGradMean_, bidGradMax_, bidSteepness_, bidCliff_,
                    /*ascending=*/false);
        ComputeSide(snap.asks, askGradMean_, askGradMax_, askSteepness_, askCliff_,
                    /*ascending=*/true);
    }

    void FillResults(BookDynamicsResults &res) const {
        res.bidGradientMean  = bidGradMean_;
        res.askGradientMean  = askGradMean_;
        res.bidGradientMax   = bidGradMax_;
        res.askGradientMax   = askGradMax_;
        res.bidSteepnessIdx  = bidSteepness_;
        res.askSteepnessIdx  = askSteepness_;
        res.bidCliffLevel    = bidCliff_;
        res.askCliffLevel    = askCliff_;
        res.thinBookAlert    = (bidGradMean_ < THIN_THRESH || askGradMean_ < THIN_THRESH);
    }

private:
    // ascending=false for bids (prices decrease with index)
    // ascending=true  for asks (prices increase with index)
    template<typename MapT>
    void ComputeSide(const MapT &levels, double &meanOut, double &maxOut,
                     double &steepnessOut, int &cliffOut, bool ascending)
    {
        // Extract top N_LEVELS levels in correct order
        std::vector<std::pair<double,double>> lv;
        lv.reserve(N_LEVELS);
        int cnt = 0;
        for (const auto &[px, qty] : levels) {
            if (cnt >= N_LEVELS) break;
            lv.push_back({static_cast<double>(px)/10000.0,
                          static_cast<double>(qty)/10000.0});
            cnt++;
        }

        if (lv.size() < 2) { meanOut=0; maxOut=0; steepnessOut=0; cliffOut=-1; return; }

        int n = static_cast<int>(lv.size());
        std::vector<double> grad(n-1);
        for (int i = 1; i < n; ++i) {
            double dp = std::abs(lv[i].first - lv[i-1].first);
            if (dp < 1e-9) dp = 1e-9;
            // dQ/dP: qty at this level / price step to previous level
            grad[i-1] = lv[i].second / dp;
        }

        // SIMD-optimized sum and max finding
        double mx = 0.0;
        int cliffIdx = -1;

        if (grad.size() >= 4) {
            // Use SIMD to find max and its index (4x speedup)
            SIMD::FindMaxAVX2(grad.data(), static_cast<int>(grad.size()), mx, cliffIdx);
            cliffIdx++;  // Adjust index (grad[0] corresponds to level 1)

            // SIMD sum
            double sum = SIMD::SumAVX2(grad.data(), static_cast<int>(grad.size()));
            meanOut     = sum / static_cast<double>(grad.size());
        } else {
            // Scalar for small arrays
            double sum = 0.0;
            for (int i = 0; i < (int)grad.size(); ++i) {
                sum += grad[i];
                if (grad[i] > mx) { mx = grad[i]; cliffIdx = i+1; }
            }
            meanOut     = sum / static_cast<double>(grad.size());
        }

        maxOut      = mx;
        steepnessOut= (meanOut > 1e-9) ? mx / meanOut : 0.0;
        cliffOut    = (steepnessOut > CLIFF_THRESH) ? cliffIdx : -1;
    }

    double bidGradMean_=0, bidGradMax_=0, bidSteepness_=0;
    double askGradMean_=0, askGradMax_=0, askSteepness_=0;
    int    bidCliff_=-1, askCliff_=-1;
};

// ============================================================================
// P3. HiddenLiquidityEstimator
//
// Principle: if a trade prints with quantity Q_trade at price P, but the
// visible book at P showed only Q_visible < Q_trade, then the excess:
//
//   Q_hidden = Q_trade − Q_visible
//
// must have come from hidden/reserve orders (iceberg or dark-pool).
//
// Exceedance ratio:
//   ratio = Q_trade / max(Q_visible, ε)
//   ratio > 1 → more traded than was visible → HIDDEN LIQUIDITY
//
// Implementation:
//   On each aggTrade(price, qty):
//     1. Look up last known qty at that price in prevBidSnap/prevAskSnap.
//        (Buy aggressor → consumed from ask side; Sell → from bid side)
//     2. If trade_qty > visible_qty_at_P → record hidden event.
//
// Aggregation:
//   Rolling 500-trade window of (visible, hidden) pairs.
//   phantom_ratio = Σhidden / (Σvisible + ε)
//
// Price bucketing: ±$0.05 grid to group nearby levels.
// ============================================================================
class HiddenLiquidityEstimator {
public:
    static constexpr int    WINDOW_TRADES = 500;
    static constexpr double BUCKET_USD    = 0.05;

    // Call on each snapshot to update visible qty reference
    void OnSnapshot(const PriceLevelSnapshot &snap) {
        // Copy ask quantities (aggressor buys consume asks)
        askQtys_.clear();
        for (const auto &[px, qty] : snap.asks)
            askQtys_[PriceBucket(static_cast<double>(px)/10000.0)] =
                static_cast<double>(qty)/10000.0;
        // Copy bid quantities (aggressor sells consume bids)
        bidQtys_.clear();
        for (const auto &[px, qty] : snap.bids)
            bidQtys_[PriceBucket(static_cast<double>(px)/10000.0)] =
                static_cast<double>(qty)/10000.0;
    }

    // Call on each aggTrade
    // isBuyerMaker=false → buyer is aggressor → consumed ask at tradePrice
    // isBuyerMaker=true  → seller is aggressor → consumed bid at tradePrice
    void OnTrade(double tradePrice, double tradeQty, bool isBuyerMaker) {
        int64_t bucket = PriceBucket(tradePrice);
        const auto &sideMap = isBuyerMaker ? bidQtys_ : askQtys_;

        double visibleQty = 0.0;
        auto it = sideMap.find(bucket);
        if (it != sideMap.end()) visibleQty = it->second;

        double hiddenQty = std::max(0.0, tradeQty - visibleQty);

        // Update rolling window
        WindowEntry e{visibleQty, hiddenQty};
        if (static_cast<int>(window_.size()) >= WINDOW_TRADES) {
            const auto &old = window_.front();
            sumVisible_ -= old.visible;
            sumHidden_  -= old.hidden;
            window_.pop_front();
        }
        window_.push_back(e);
        sumVisible_ += visibleQty;
        sumHidden_  += hiddenQty;

        // Update results
        hiddenVolCum_  += hiddenQty;
        visibleVolCum_ += visibleQty;

        if (hiddenQty > 1e-9) {
            detectCount_++;
            lastHiddenPrice_    = tradePrice;
            lastExceedRatio_    = (visibleQty > 1e-9) ? tradeQty / visibleQty : 99.0;
            lastExceedVol_      = hiddenQty;
            lastDetectTrade_    = totalTrades_;
            alert_              = true;
        } else {
            // Auto-clear alert after 20 trades without detection
            if (totalTrades_ - lastDetectTrade_ > 20) alert_ = false;
        }
        totalTrades_++;
        phantomRatio_ = (sumVisible_ > 1e-9) ? sumHidden_ / sumVisible_ : 0.0;
    }

    void FillResults(BookDynamicsResults &res) const {
        res.hiddenVolCumulative  = hiddenVolCum_;
        res.visibleVolCumulative = visibleVolCum_;
        res.phantomRatio         = phantomRatio_;
        res.lastHiddenPrice      = lastHiddenPrice_;
        res.lastExceedanceRatio  = lastExceedRatio_;
        res.lastExceedanceVol    = lastExceedVol_;
        res.hiddenDetectCount    = detectCount_;
        res.hiddenAlert          = alert_;
    }

private:
    static int64_t PriceBucket(double price) {
        return static_cast<int64_t>(std::round(price / BUCKET_USD));
    }

    struct WindowEntry { double visible, hidden; };

    std::unordered_map<int64_t,double> askQtys_, bidQtys_;
    std::deque<WindowEntry> window_;
    double sumVisible_     = 0.0;
    double sumHidden_      = 0.0;
    double hiddenVolCum_   = 0.0;
    double visibleVolCum_  = 0.0;
    double phantomRatio_   = 0.0;
    double lastHiddenPrice_= 0.0;
    double lastExceedRatio_= 0.0;
    double lastExceedVol_  = 0.0;
    int    detectCount_    = 0;
    int    totalTrades_    = 0;
    int    lastDetectTrade_= -100;
    bool   alert_          = false;
};

// ============================================================================
// BookDynamicsEngine — orchestrates all 5 sub-engines
//
// Usage:
//   BookDynamicsEngine bookDyn;
//
//   // On each depth update (after analytics.Calculate):
//   int64_t nowUs = MicrostructureEngine::CurrentUsEpoch();
//   bookDyn.OnDepthUpdate(snap, nowUs);
//
//   // On each aggTrade:
//   bookDyn.OnTrade(tradePrice, tradeQty, isBuyerMaker);
//
//   // Read results:
//   const BookDynamicsResults& bdy = bookDyn.GetResults();
// ============================================================================
class BookDynamicsEngine {
public:
    // ── Call on each confirmed depth update ───────────────────────────────────
    void OnDepthUpdate(const PriceLevelSnapshot &snap, int64_t nowUs) {
        if (snap.bids.empty() || snap.asks.empty()) return;

        double midPrice = 0.0;
        if (!snap.bids.empty() && !snap.asks.empty())
            midPrice = (static_cast<double>(snap.bids.begin()->first) +
                        static_cast<double>(snap.asks.begin()->first)) / 20000.0;

        // Update all sub-engines
        heatmap_.OnSnapshot(snap, midPrice);
        lifetime_.OnSnapshot(snap, nowUs);
        wallVel_.OnSnapshot(snap, nowUs);
        gradient_.OnSnapshot(snap);
        hidden_.OnSnapshot(snap);   // update visible reference for next trade

        // Assemble results
        heatmap_.FillDisplayMatrix(results_);
        lifetime_.FillResults(results_);
        wallVel_.FillResults(results_);
        gradient_.FillResults(results_);
        // hidden_ results updated in OnTrade and carried over
    }

    // ── Call on each confirmed aggTrade ───────────────────────────────────────
    // isBuyerMaker: directly from Binance aggTrade `m` field
    void OnTrade(double tradePrice, double tradeQty, bool isBuyerMaker) {
        hidden_.OnTrade(tradePrice, tradeQty, isBuyerMaker);
        hidden_.FillResults(results_);
    }

    const BookDynamicsResults& GetResults() const { return results_; }
    BookDynamicsResults&       GetResults()       { return results_; }

private:
    BookPressureHeatmap      heatmap_;
    LevelLifetimeTracker     lifetime_;
    DepthMigrationVelocity   wallVel_;
    OrderBookGradient        gradient_;
    HiddenLiquidityEstimator hidden_;
    BookDynamicsResults      results_;
};