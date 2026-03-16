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

const MSG_SNAPSHOT  = 0;
const MSG_DELTA     = 1;
const MSG_TRADE     = 2;
const MSG_STATS     = 3;

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

    const spreadBps   = payload.readDoubleBE(off); off += 8;
    const midPrice    = payload.readDoubleBE(off); off += 8;
    const imbalance   = payload.readDoubleBE(off); off += 8;
    const ofi         = payload.readDoubleBE(off); off += 8;
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
