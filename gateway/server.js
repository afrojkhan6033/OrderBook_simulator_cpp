// =============================================================================
// gateway/server.js  —  Node.js Binary Gateway
//
// Sits between your C++ engine and the Svelte UI.
// • TCP server on port 9001  → receives binary frames from C++
// • WebSocket server on 9002 → broadcasts JSON to Svelte clients
//
// Protocol: parses the exact binary wire format from BinarySerializer.h
// Price decode: raw_int / 10000.0  (matches your existing * 10000 encoding)
//
// Start:  node server.js
// =============================================================================

'use strict';

const net       = require('net');
const http      = require('http');
const WebSocket = require('ws');

// ── Config ────────────────────────────────────────────────────────────────────
const BINARY_PORT = 9001;   // C++ TcpSender connects here
const WS_PORT     = 9002;   // Svelte connects here
const BOOK_DEPTH  = 50;     // max levels forwarded to UI
const PRICE_SCALE = 10000;  // matches C++ raw * 10000 encoding

// ── Wire constants (must match BinarySerializer.h) ────────────────────────────
const MAGIC_0       = 0x48;  // 'H'
const MAGIC_1       = 0x46;  // 'F'
const FRAME_HDR_LEN = 8;     // 2 magic + 1 version + 1 type + 4 len

const MSG_SNAPSHOT         = 0;
const MSG_DELTA            = 1;
const MSG_TRADE            = 2;
const MSG_STATS            = 3;
const MSG_MICROSTRUCTURE   = 4;
const MSG_SIGNALS          = 5;
const MSG_RISK             = 6;
const MSG_BOOK_DYNAMICS    = 7;
const MSG_REGIME           = 8;
const MSG_STRATEGY         = 9;
const MSG_CROSS_EXCHANGE   = 10;

// ── State ─────────────────────────────────────────────────────────────────────
let latestSnapshot = null;   // cached for new WS clients
let latestStats    = null;
const wsClients    = new Set();

// ── WebSocket server (for Svelte UI) ─────────────────────────────────────────
const httpServer = http.createServer();
const wss = new WebSocket.Server({ server: httpServer });

wss.on('connection', (ws) => {
    console.log(`[WS] Client connected  (${wsClients.size + 1} total)`);
    wsClients.add(ws);

    // Send cached state immediately so the UI doesn't start empty
    if (latestSnapshot) ws.send(JSON.stringify(latestSnapshot));
    if (latestStats)    ws.send(JSON.stringify(latestStats));

    ws.on('close', () => {
        wsClients.delete(ws);
        console.log(`[WS] Client left  (${wsClients.size} remaining)`);
    });
    ws.on('error', () => wsClients.delete(ws));
});

function broadcast(msg) {
    const json = JSON.stringify(msg);
    for (const client of wsClients) {
        if (client.readyState === WebSocket.OPEN) {
            client.send(json);
        }
    }
}

httpServer.listen(WS_PORT, () =>
    console.log(`[WS]  Listening on ws://localhost:${WS_PORT}`)
);

// ── Binary frame parser ───────────────────────────────────────────────────────
// We accumulate raw TCP bytes in a carry buffer and parse complete frames.

function parseFrames(carry, newData) {
    // Merge carry + newData
    let buf;
    if (carry.length === 0) {
        buf = newData;
    } else {
        buf = Buffer.allocUnsafe(carry.length + newData.length);
        carry.copy(buf, 0);
        newData.copy(buf, carry.length);
    }

    let offset = 0;

    while (offset < buf.length) {
        // Need at least a full frame header
        if (buf.length - offset < FRAME_HDR_LEN) break;

        // Validate magic bytes
        if (buf[offset] !== MAGIC_0 || buf[offset + 1] !== MAGIC_1) {
            console.error('[Parser] Bad magic — re-syncing');
            offset++;
            continue;
        }

        const version    = buf[offset + 2];
        const msgType    = buf[offset + 3];
        const payloadLen = buf.readUInt32LE(offset + 4);

        const frameEnd = offset + FRAME_HDR_LEN + payloadLen;
        if (buf.length < frameEnd) break;  // wait for more data

        const payload = buf.slice(offset + FRAME_HDR_LEN, frameEnd);
        dispatchFrame(msgType, payload);
        offset = frameEnd;
    }

    return buf.slice(offset);  // remaining bytes (partial frame)
}

function dispatchFrame(msgType, payload) {
    try {
        switch (msgType) {
            case MSG_SNAPSHOT:
            case MSG_DELTA:
                handleSnapshotFrame(payload);
                break;
            case MSG_TRADE:
                handleTradeFrame(payload);
                break;
            case MSG_STATS:
                handleStatsFrame(payload);
                break;
            case MSG_MICROSTRUCTURE:
                handleMicrostructureFrame(payload);
                break;
            case MSG_SIGNALS:
                handleSignalsFrame(payload);
                break;
            case MSG_RISK:
                handleRiskFrame(payload);
                break;
            case MSG_BOOK_DYNAMICS:
                handleBookDynamicsFrame(payload);
                break;
            case MSG_REGIME:
                handleRegimeFrame(payload);
                break;
            case MSG_STRATEGY:
                handleStrategyFrame(payload);
                break;
            case MSG_CROSS_EXCHANGE:
                handleCrossExchangeFrame(payload);
                break;
            default:
                console.warn(`[Parser] Unknown msg_type: ${msgType}`);
        }
    } catch (err) {
        console.error('[Parser] Frame error:', err.message);
    }
}

// ── Snapshot / Delta frame ────────────────────────────────────────────────────
// Layout: SnapshotHeader (24 bytes) + bid_count * WirePriceLevel (8 bytes each)
//                                   + ask_count * WirePriceLevel (8 bytes each)
function handleSnapshotFrame(payload) {
    if (payload.length < 24) return;

    let off = 0;

    // SnapshotHeader
    const seqNum      = readUInt64LE(payload, off);     off += 8;
    const eventTimeUs = readUInt64LE(payload, off);     off += 8;
    const bidCount    = payload.readUInt32LE(off);       off += 4;
    const askCount    = payload.readUInt32LE(off);       off += 4;

    const bids = [];
    for (let i = 0; i < bidCount && off + 8 <= payload.length; i++) {
        const price = payload.readInt32LE(off) / PRICE_SCALE;   off += 4;
        const qty   = payload.readUInt32LE(off) / PRICE_SCALE;  off += 4;
        bids.push({ price: round4(price), qty: round4(qty) });
    }

    const asks = [];
    for (let i = 0; i < askCount && off + 8 <= payload.length; i++) {
        const price = payload.readInt32LE(off) / PRICE_SCALE;   off += 4;
        const qty   = payload.readUInt32LE(off) / PRICE_SCALE;  off += 4;
        asks.push({ price: round4(price), qty: round4(qty) });
    }

    // Derive analytics from the book (mirrors MarketAnalytics::Calculate)
    const bestBid  = bids[0]?.price  ?? 0;
    const bestAsk  = asks[0]?.price  ?? 0;
    const mid      = (bestBid + bestAsk) / 2;
    const spread   = bestAsk - bestBid;
    const spreadBps= mid > 0 ? round2((spread / mid) * 10000) : 0;
    const bidLiq   = bids.reduce((s, l) => s + l.qty, 0);
    const askLiq   = asks.reduce((s, l) => s + l.qty, 0);
    const imbal    = (bidLiq + askLiq) > 0
        ? round4((bidLiq - askLiq) / (bidLiq + askLiq))
        : 0;

    const msg = {
        type:       'snapshot',
        seqNum,
        eventTimeUs: Number(eventTimeUs),
        bids:        bids.slice(0, BOOK_DEPTH),
        asks:        asks.slice(0, BOOK_DEPTH),
        analytics: {
            bestBid:   round4(bestBid),
            bestAsk:   round4(bestAsk),
            mid:       round4(mid),
            spread:    round4(spread),
            spreadBps,
            bidLiq:    round2(bidLiq),
            askLiq:    round2(askLiq),
            imbal,
        },
    };

    latestSnapshot = msg;
    broadcast(msg);
}

// ── Trade frame ───────────────────────────────────────────────────────────────
// Layout: TradePayload — seqNum(8) + eventTimeUs(8) + price(4) + qty(4) + side(1) + pad(3)
function handleTradeFrame(payload) {
    if (payload.length < 28) return;

    const seqNum      = readUInt64LE(payload, 0);
    const eventTimeUs = readUInt64LE(payload, 8);
    const price       = payload.readInt32LE(16) / PRICE_SCALE;
    const qty         = payload.readUInt32LE(20) / PRICE_SCALE;
    const side        = payload[24] === 0 ? 'buy' : 'sell';

    const msg = {
        type: 'trade',
        seqNum,
        eventTimeUs: Number(eventTimeUs),
        price:  round4(price),
        qty:    round4(qty),
        side,
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Stats frame ───────────────────────────────────────────────────────────────
// Layout: StatsPayload — see BinarySerializer.h
function handleStatsFrame(payload) {
    if (payload.length < 88) return;  // 8*6 + 8*4 + 8 = 88 bytes

    let off = 0;
    const msgsReceived    = readUInt64LE(payload, off); off += 8;
    const msgsProcessed   = readUInt64LE(payload, off); off += 8;
    const msgsDropped     = readUInt64LE(payload, off); off += 8;
    const totalProcUs     = readUInt64LE(payload, off); off += 8;
    const maxProcUs       = readUInt64LE(payload, off); off += 8;
    const minProcUs       = readUInt64LE(payload, off); off += 8;

    const spreadBps   = payload.readDoubleLE(off); off += 8;
    const midPrice    = payload.readDoubleLE(off); off += 8;
    const imbalance   = payload.readDoubleLE(off); off += 8;
    const ofi         = payload.readDoubleLE(off); off += 8;
    const seqNum      = readUInt64LE(payload, off);

    const avgLatUs = Number(msgsProcessed) > 0
        ? round2(Number(totalProcUs) / Number(msgsProcessed))
        : 0;

    const msg = {
        type: 'stats',
        msgsReceived:  Number(msgsReceived),
        msgsProcessed: Number(msgsProcessed),
        msgsDropped:   Number(msgsDropped),
        avgLatUs,
        maxLatUs:      Number(maxProcUs),
        minLatUs:      Number(minProcUs) === 0x7FFFFFFFFFFFFFFF ? 0 : Number(minProcUs),
        spreadBps:     round2(spreadBps),
        midPrice:      round4(midPrice),
        imbalance:     round4(imbalance),
        ofi:           round2(ofi),
        seqNum:        Number(seqNum),
        ts: Date.now(),
    };

    latestStats = msg;
    broadcast(msg);
}

// ── Microstructure frame ──────────────────────────────────────────────────────
// Layout: MicrostructurePayload — 7 doubles (56) + int32 (4) + 2 doubles (16) + uint64 (8) = 84 bytes
function handleMicrostructureFrame(payload) {
    if (payload.length < 84) return;  // FIX: was 144, actual struct is 84 bytes

    let off = 0;
    const vwap          = payload.readDoubleLE(off); off += 8;
    const twap          = payload.readDoubleLE(off); off += 8;
    const microprice    = payload.readDoubleLE(off); off += 8;
    const kyleLambda    = payload.readDoubleLE(off); off += 8;
    const amihudIlliq   = payload.readDoubleLE(off); off += 8;
    const rollSpread    = payload.readDoubleLE(off); off += 8;
    const signedFlow    = payload.readDoubleLE(off); off += 8;
    const lastTradeSide = payload.readInt32LE(off);  off += 4;
    const lastTradePrice= payload.readDoubleLE(off); off += 8;
    const lastTradeQty  = payload.readDoubleLE(off); off += 8;
    const tradeCount    = readUInt64LE(payload, off);

    const msg = {
        type: 'microstructure',
        vwap:         round4(vwap),
        twap:         round4(twap),
        microprice:   round4(microprice),
        kyleLambda:   round4(kyleLambda),
        amihudIlliq:  round4(amihudIlliq),
        rollSpread:   round4(rollSpread),
        signedFlow:   round2(signedFlow),
        lastTradeSide,
        lastTradePrice: round4(lastTradePrice),
        lastTradeQty:   round4(lastTradeQty),
        tradeCount:     Number(tradeCount),
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Signals frame ─────────────────────────────────────────────────────────────
// Layout: SignalPayload — 8 doubles (64) + 5 int32 (20) = 84 bytes (approximate)
function handleSignalsFrame(payload) {
    if (payload.length < 80) return;

    let off = 0;
    const ofiNormalized = payload.readDoubleLE(off); off += 8;
    const vpin           = payload.readDoubleLE(off); off += 8;
    const vpinBuckets    = payload.readInt32LE(off);   off += 4;
    const momentumScore  = payload.readDoubleLE(off); off += 8;
    const ar1Coeff       = payload.readDoubleLE(off); off += 8;
    const zScore         = payload.readDoubleLE(off); off += 8;
    const regime         = payload.readInt32LE(off);   off += 4;
    const icebergDetected= payload.readInt32LE(off);   off += 4;
    const icebergPrice   = payload.readDoubleLE(off); off += 8;
    const icebergRefills = payload.readInt32LE(off);   off += 4;
    const spoofingAlert  = payload.readInt32LE(off);   off += 4;
    const spoofPrice     = payload.readDoubleLE(off); off += 8;
    const stuffingRatio  = payload.readDoubleLE(off);

    const msg = {
        type: 'signals',
        ofiNormalized: round4(ofiNormalized),
        vpin:          round4(vpin),
        vpinBuckets,
        momentumScore: round4(momentumScore),
        ar1Coeff:      round4(ar1Coeff),
        zScore:        round4(zScore),
        regime,
        icebergDetected: icebergDetected !== 0,
        icebergPrice:  round4(icebergPrice),
        icebergRefills,
        spoofingAlert:  spoofingAlert !== 0,
        spoofPrice:    round4(spoofPrice),
        stuffingRatio: round4(stuffingRatio),
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Risk frame ────────────────────────────────────────────────────────────────
// Layout: RiskPayload — 17 doubles (136 bytes)
function handleRiskFrame(payload) {
    if (payload.length < 136) return;

    let off = 0;
    const positionSize     = payload.readDoubleLE(off); off += 8;
    const entryPrice       = payload.readDoubleLE(off); off += 8;
    const positionNotional = payload.readDoubleLE(off); off += 8;
    const totalPnl         = payload.readDoubleLE(off); off += 8;
    const realizedPnl      = payload.readDoubleLE(off); off += 8;
    const unrealizedPnl    = payload.readDoubleLE(off); off += 8;
    const winRate          = payload.readDoubleLE(off); off += 8;
    const fillProb100ms    = payload.readDoubleLE(off); off += 8;
    const queueDepthAtL1   = payload.readDoubleLE(off); off += 8;
    const tradeArrivalRate = payload.readDoubleLE(off); off += 8;
    const sharpePerTrade   = payload.readDoubleLE(off); off += 8;
    const maxDrawdown      = payload.readDoubleLE(off); off += 8;
    const compositeScore   = payload.readDoubleLE(off); off += 8;
    const kalmanPrice      = payload.readDoubleLE(off); off += 8;
    const kalmanVelocity   = payload.readDoubleLE(off); off += 8;
    const hitRate          = payload.readDoubleLE(off); off += 8;
    
    // v9 Risk extensions
    let slipLast = 0, slipAvg = 0, slipImpl = 0, slipArrival = 0, dv01 = 0, liquidCost = 0, mktImpact = 0;
    if (off + 56 <= payload.length) {
        slipLast         = payload.readDoubleLE(off); off += 8;
        slipAvg          = payload.readDoubleLE(off); off += 8;
        slipImpl         = payload.readDoubleLE(off); off += 8;
        slipArrival      = payload.readDoubleLE(off); off += 8;
        dv01             = payload.readDoubleLE(off); off += 8;
        liquidCost       = payload.readDoubleLE(off); off += 8;
        mktImpact        = payload.readDoubleLE(off); off += 8;
    }

    const msg = {
        type: 'risk',
        positionSize:    round4(positionSize),
        entryPrice:      round4(entryPrice),
        positionNotional: round4(positionNotional),
        totalPnl:        round4(totalPnl),
        realizedPnl:     round4(realizedPnl),
        unrealizedPnl:   round4(unrealizedPnl),
        winRate:         round4(winRate),
        fillProb100ms:   round4(fillProb100ms),
        queueDepthAtL1:  round4(queueDepthAtL1),
        tradeArrivalRate: round4(tradeArrivalRate),
        sharpePerTrade:  round4(sharpePerTrade),
        maxDrawdown:     round4(maxDrawdown),
        compositeScore:  round4(compositeScore),
        kalmanPrice:     round4(kalmanPrice),
        kalmanVelocity:  round4(kalmanVelocity),
        hitRate:         round4(hitRate),
        slipLast:        round4(slipLast),
        slipAvg:         round4(slipAvg),
        slipImpl:        round4(slipImpl),
        slipArrival:     round4(slipArrival),
        dv01:            round4(dv01),
        liquidCost:      round4(liquidCost),
        mktImpact:       round4(mktImpact),
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Book Dynamics frame ──────────────────────────────────────────────────────
// Layout: BookDynamicsPayload — heatmap[50 floats] (200) + 11 doubles (88) + uint64 = 292 bytes
function handleBookDynamicsFrame(payload) {
    if (payload.length < 290) return;

    let off = 0;
    // Read heatmap: 50 floats
    const heatmap = [];
    for (let i = 0; i < 50; i++) {
        heatmap.push(payload.readFloatLE(off));
        off += 4;
    }
    const heatmapMidPrice  = payload.readDoubleLE(off); off += 8;
    const heatmapBucketUSD = payload.readDoubleLE(off); off += 8;
    const bidWallVelocity  = payload.readDoubleLE(off); off += 8;
    const askWallVelocity  = payload.readDoubleLE(off); off += 8;
    const bidGradientMean  = payload.readDoubleLE(off); off += 8;
    const askGradientMean  = payload.readDoubleLE(off); off += 8;
    const phantomRatio     = payload.readDoubleLE(off); off += 8;
    const hiddenDetectCount= readUInt64LE(payload, off); off += 8;
    const avgBidLifetimeMs = payload.readDoubleLE(off); off += 8;
    const avgAskLifetimeMs = payload.readDoubleLE(off); off += 8;
    const compressionRateUSD= payload.readDoubleLE(off); off += 8;
    
    // v9 BookDyn extensions
    let bidSl = 0, askSl = 0, bidLl = 0, askLl = 0;
    let bidGrad = 0, askGrad = 0, bidSteep = 0, askSteep = 0;
    const bidLifeHist = [0,0,0,0,0];
    const askLifeHist = [0,0,0,0,0];

    if (off + 88 <= payload.length) {
        bidSl = payload.readInt32LE(off); off += 4;
        askSl = payload.readInt32LE(off); off += 4;
        bidLl = payload.readInt32LE(off); off += 4;
        askLl = payload.readInt32LE(off); off += 4;
        bidGrad = payload.readDoubleLE(off); off += 8;
        askGrad = payload.readDoubleLE(off); off += 8;
        bidSteep = payload.readDoubleLE(off); off += 8;
        askSteep = payload.readDoubleLE(off); off += 8;
        for(let i=0; i<5; i++) { bidLifeHist[i] = payload.readInt32LE(off); off += 4; }
        for(let i=0; i<5; i++) { askLifeHist[i] = payload.readInt32LE(off); off += 4; }
    }

    const msg = {
        type: 'bookDynamics',
        heatmap,
        heatmapMidPrice:   round4(heatmapMidPrice),
        heatmapBucketUSD:  round4(heatmapBucketUSD),
        bidWallVelocity:   round4(bidWallVelocity),
        askWallVelocity:   round4(askWallVelocity),
        bidGradientMean:   round4(bidGradientMean),
        askGradientMean:   round4(askGradientMean),
        phantomRatio:      round4(phantomRatio),
        hiddenDetectCount: Number(hiddenDetectCount),
        avgBidLifetimeMs:  round2(avgBidLifetimeMs),
        avgAskLifetimeMs:  round2(avgAskLifetimeMs),
        compressionRateUSD: round4(compressionRateUSD),
        bidSl, askSl, bidLl, askLl,
        bidGrad: round4(bidGrad), askGrad: round4(askGrad),
        bidSteep: round4(bidSteep), askSteep: round4(askSteep),
        bidLifeHist, askLifeHist,
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Regime frame ──────────────────────────────────────────────────────────────
// Layout: RegimePayload — 7 doubles (56) + 2 int32 (8) = 64 bytes
function handleRegimeFrame(payload) {
    if (payload.length < 64) return;

    let off = 0;
    const realizedVolAnnualized = payload.readDoubleLE(off); off += 8;
    const volRegime             = payload.readInt32LE(off);  off += 4;
    const hurstExponent         = payload.readDoubleLE(off); off += 8;
    const hurstRegime           = payload.readInt32LE(off);  off += 4;
    const hmmBullProb           = payload.readDoubleLE(off); off += 8;
    const hmmBearProb           = payload.readDoubleLE(off); off += 8;
    const autocorrLag1          = payload.readDoubleLE(off); off += 8;
    const regimeAdjustedScore   = payload.readDoubleLE(off); off += 8;
    const edgeScore             = payload.readDoubleLE(off); off += 8;
    
    // v9 Regime extensions
    let srTicks = 0, srMean = 0, srStd = 0, srZ = 0, midAcfZ = 0, tickVol = 0;
    const spreadHist = [0,0,0,0,0,0];

    if (off + 72 <= payload.length) {
        srTicks = payload.readDoubleLE(off); off += 8;
        srMean = payload.readDoubleLE(off); off += 8;
        srStd = payload.readDoubleLE(off); off += 8;
        srZ = payload.readDoubleLE(off); off += 8;
        midAcfZ = payload.readDoubleLE(off); off += 8;
        tickVol = payload.readDoubleLE(off); off += 8;
        for(let i=0; i<6; i++) { spreadHist[i] = payload.readInt32LE(off); off += 4; }
    }

    const msg = {
        type: 'regime',
        realizedVolAnnualized: round4(realizedVolAnnualized),
        volRegime,
        hurstExponent:    round4(hurstExponent),
        hurstRegime,
        hmmBullProb:      round4(hmmBullProb),
        hmmBearProb:      round4(hmmBearProb),
        autocorrLag1:     round4(autocorrLag1),
        regimeAdjustedScore: round4(regimeAdjustedScore),
        edgeScore:        round4(edgeScore),
        srTicks: round2(srTicks),
        srMean:  round2(srMean),
        srStd:   round2(srStd),
        srZ:     round4(srZ),
        midAcfZ: round4(midAcfZ),
        tickVol: round4(tickVol),
        spreadHist,
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Strategy frame ────────────────────────────────────────────────────────────
// Layout: StrategyPayload — 18 doubles (144) + int (4) + 2 bools (2) = ~150 bytes
function handleStrategyFrame(payload) {
    if (payload.length < 140) return;

    let off = 0;
    const asBid               = payload.readDoubleLE(off); off += 8;
    const asAsk               = payload.readDoubleLE(off); off += 8;
    const asReservation       = payload.readDoubleLE(off); off += 8;
    const asOptimalSpreadBps  = payload.readDoubleLE(off); off += 8;
    const asSkewBps           = payload.readDoubleLE(off); off += 8;
    const asSigmaBps          = payload.readDoubleLE(off); off += 8;
    const mmNetPnl            = payload.readDoubleLE(off); off += 8;
    const mmRealPnl           = payload.readDoubleLE(off); off += 8;
    const mmUnrealPnl         = payload.readDoubleLE(off); off += 8;
    const mmInventory         = payload.readDoubleLE(off); off += 8;
    const mmWinRate           = payload.readDoubleLE(off); off += 8;
    const mmFillRate          = payload.readDoubleLE(off); off += 8;
    const mmInventoryAlert    = payload.readInt8(off);       off += 1;
    const mmQuotingGated      = payload.readInt8(off);       off += 1;
    const latEdgeBps          = payload.readDoubleLE(off); off += 8;
    const latCumEdgeUSD       = payload.readDoubleLE(off); off += 8;
    const latSharpe           = payload.readDoubleLE(off); off += 8;
    const latOpportunities    = payload.readInt32LE(off); off += 4;
    
    // v9 Strategy & Cross Exchange extensions
    let replaySimPnl = 0, replayWinRate = 0, replayMAE = 0, replayMFE = 0, replayMin = 0, replayMax = 0;
    let exchBid1 = 0, exchAsk1 = 0, exchBid2 = 0, exchAsk2 = 0, exchMidBps = 0, exchArbBps = 0, exchConn = 0;

    if (off + 100 <= payload.length) {
        replaySimPnl = payload.readDoubleLE(off); off += 8;
        replayWinRate= payload.readDoubleLE(off); off += 8;
        replayMAE    = payload.readDoubleLE(off); off += 8;
        replayMFE    = payload.readDoubleLE(off); off += 8;
        replayMin    = payload.readDoubleLE(off); off += 8;
        replayMax    = payload.readDoubleLE(off); off += 8;
        exchBid1     = payload.readDoubleLE(off); off += 8;
        exchAsk1     = payload.readDoubleLE(off); off += 8;
        exchBid2     = payload.readDoubleLE(off); off += 8;
        exchAsk2     = payload.readDoubleLE(off); off += 8;
        exchMidBps   = payload.readDoubleLE(off); off += 8;
        exchArbBps   = payload.readDoubleLE(off); off += 8;
        exchConn     = payload.readInt32LE(off);  off += 4;
    }

    const msg = {
        type: 'strategy',
        asBid:               round4(asBid),
        asAsk:               round4(asAsk),
        asReservation:       round4(asReservation),
        asOptimalSpreadBps:  round4(asOptimalSpreadBps),
        asSkewBps:           round4(asSkewBps),
        asSigmaBps:          round4(asSigmaBps),
        mmNetPnl:            round4(mmNetPnl),
        mmRealPnl:           round4(mmRealPnl),
        mmUnrealPnl:         round4(mmUnrealPnl),
        mmInventory:         round4(mmInventory),
        mmWinRate:           round4(mmWinRate),
        mmFillRate:          round4(mmFillRate),
        mmInventoryAlert:    mmInventoryAlert !== 0,
        mmQuotingGated:      mmQuotingGated !== 0,
        latEdgeBps:          round4(latEdgeBps),
        latCumEdgeUSD:       round4(latCumEdgeUSD),
        latSharpe:           round4(latSharpe),
        latOpportunities,
        replaySimPnl:        round4(replaySimPnl),
        replayWinRate:       round4(replayWinRate),
        replayMAE:           round4(replayMAE),
        replayMFE:           round4(replayMFE),
        replayMin:           round4(replayMin),
        replayMax:           round4(replayMax),
        exchBid1:            round4(exchBid1),
        exchAsk1:            round4(exchAsk1),
        exchBid2:            round4(exchBid2),
        exchAsk2:            round4(exchAsk2),
        exchMidBps:          round4(exchMidBps),
        exchArbBps:          round4(exchArbBps),
        exchConn:            exchConn !== 0,
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── Cross-Exchange Feed frame ─────────────────────────────────────────────────
// Layout: CrossExchangePayload — 5 doubles(40) + 2 int32(8) + 4 doubles(32) = 80 bytes
function handleCrossExchangeFrame(payload) {
    if (payload.length < 80) return;

    let off = 0;
    const bid         = payload.readDoubleLE(off); off += 8;
    const ask         = payload.readDoubleLE(off); off += 8;
    const mid         = payload.readDoubleLE(off); off += 8;
    const spread      = payload.readDoubleLE(off); off += 8;
    const spreadBps   = payload.readDoubleLE(off); off += 8;
    const connected   = payload.readInt32LE(off);  off += 4;
    const isSpot      = payload.readInt32LE(off);  off += 4;
    const binanceMid  = payload.readDoubleLE(off); off += 8;
    const driftUSD    = payload.readDoubleLE(off); off += 8;
    const driftBps    = payload.readDoubleLE(off); off += 8;
    const arbNetBps   = payload.readDoubleLE(off);

    const msg = {
        type: 'crossExchange',
        bid:        round4(bid),
        ask:        round4(ask),
        mid:        round4(mid),
        spread:     round4(spread),
        spreadBps:  round2(spreadBps),
        connected:  connected !== 0,
        isSpot:     isSpot !== 0,
        binanceMid: round4(binanceMid),
        driftUSD:   round4(driftUSD),
        driftBps:   round2(driftBps),
        arbNetBps:  round2(arbNetBps),
        ts: Date.now(),
    };

    broadcast(msg);
}

// ── TCP server (for C++ TcpSender) ───────────────────────────────────────────
const tcpServer = net.createServer((socket) => {
    console.log(`[TCP] C++ engine connected from ${socket.remoteAddress}`);

    // Performance tuning — TCP_NODELAY on accepted socket
    socket.setNoDelay(true);

    let carry = Buffer.alloc(0);

    socket.on('data', (chunk) => {
        carry = parseFrames(carry, chunk);
    });

    socket.on('close', () => console.log('[TCP] C++ engine disconnected'));
    socket.on('error', (err) => console.error('[TCP] Socket error:', err.message));
});

tcpServer.listen(BINARY_PORT, '0.0.0.0', () =>
    console.log(`[TCP] Listening for C++ on tcp://0.0.0.0:${BINARY_PORT}`)
);

// ── Helpers ───────────────────────────────────────────────────────────────────

// Read a uint64 as a JS Number (safe up to 2^53 — fine for seq numbers & μs timestamps)
function readUInt64LE(buf, offset) {
    const lo = buf.readUInt32LE(offset);
    const hi = buf.readUInt32LE(offset + 4);
    return hi * 0x100000000 + lo;
}

function round2(v) { return Math.round(v * 100)   / 100; }
function round4(v) { return Math.round(v * 10000) / 10000; }

// ── Startup banner ────────────────────────────────────────────────────────────
console.log('');
console.log('┌─────────────────────────────────────────────┐');
console.log('│  HFT Binary Gateway  —  Node.js             │');
console.log('├─────────────────────────────────────────────┤');
console.log(`│  TCP  (C++ binary)  → port ${BINARY_PORT}          │`);
console.log(`│  WS   (Svelte UI)   → port ${WS_PORT}          │`);
console.log('│  Price scale: / 10000  (matches C++ engine) │');
console.log('└─────────────────────────────────────────────┘');
console.log('');
