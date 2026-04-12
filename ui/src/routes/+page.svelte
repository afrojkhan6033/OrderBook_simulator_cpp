<script>
  // =============================================================================
  // +page.svelte  —  HFT OrderBook Dashboard (Professional Ultra-Wide Edition)
  //
  // 4-column layout matching institutional HFT terminal aesthetics:
  //   Col 1: Order Book with animated depth bars
  //   Col 2: Cumulative Depth Chart (PixiJS) + Trade Tape
  //   Col 3: Analytics (OFI, stats, sparklines, latency timeline)
  //   Col 4: Super Engine Insights (tabbed: ALL/SIGNALS/MICRO/RISK/REGIME/STRAT)
  //          + Position & Risk sidebar
  //
  // Consumes ALL gateway message types:
  //   snapshot, trade, stats, microstructure, signals, risk,
  //   bookDynamics, regime, strategy
  // =============================================================================
  import { onMount, onDestroy } from 'svelte';

  // ── Config ──────────────────────────────────────────────────────────────────
  const WS_URL        = 'ws://localhost:9002';
  const BOOK_DEPTH    = 20;
  const MAX_TRADES    = 50;
  const RECONNECT_MS  = 1500;
  const SPARKLINE_LEN = 200;
  const LAT_HISTORY   = 80;
  const OFI_THRESHOLD = 0.2;

  // ── Reactive state ───────────────────────────────────────────────────────────
  let connected  = $state(false);
  let wsStatus   = $state('CONNECTING');
  let backendSymbol = $state('SOLUSDT');
  let bids       = $state([]);
  let asks       = $state([]);

  let analytics  = $state({
    bestBid: 0, bestAsk: 0, mid: 0,
    spread: 0, spreadBps: 0,
    bidLiq: 0, askLiq: 0,
    imbal: 0, ofi: 0
  });

  let stats = $state({
    msgsReceived: 0, msgsProcessed: 0, msgsDropped: 0,
    avgLatUs: 0, maxLatUs: 0, minLatUs: 0,
    spreadBps: 0, midPrice: 0, imbalance: 0,
    ofi: 0, seqNum: 0, netLatUs: 0
  });

  // Engine data states
  let micro = $state({
    vwap: 0, twap: 0, microprice: 0, kyleLambda: 0,
    amihudIlliq: 0, rollSpread: 0, signedFlow: 0,
    lastTradeSide: 0, lastTradePrice: 0, lastTradeQty: 0, tradeCount: 0
  });

  let signals = $state({
    ofiNormalized: 0, vpin: 0, vpinBuckets: 0,
    momentumScore: 0, ar1Coeff: 0, zScore: 0, regime: 0,
    icebergDetected: false, icebergPrice: 0, icebergRefills: 0,
    spoofingAlert: false, spoofPrice: 0, stuffingRatio: 0
  });

  let risk = $state({
    positionSize: 0, entryPrice: 0, positionNotional: 0,
    totalPnl: 0, realizedPnl: 0, unrealizedPnl: 0,
    winRate: 0, fillProb100ms: 0, queueDepthAtL1: 0,
    tradeArrivalRate: 0, sharpePerTrade: 0, maxDrawdown: 0,
    compositeScore: 0, kalmanPrice: 0, kalmanVelocity: 0,
    hitRate: 0, slipLast: 0, slipAvg: 0, slipImpl: 0,
    slipArrival: 0, dv01: 0, liquidCost: 0, mktImpact: 0
  });

  let bookDyn = $state({
    heatmap: [], heatmapMidPrice: 0, heatmapBucketUSD: 0,
    bidWallVelocity: 0, askWallVelocity: 0,
    bidGradientMean: 0, askGradientMean: 0,
    phantomRatio: 0, hiddenDetectCount: 0,
    avgBidLifetimeMs: 0, avgAskLifetimeMs: 0,
    compressionRateUSD: 0
  });

  let regimeData = $state({
    realizedVolAnnualized: 0, volRegime: 0,
    hurstExponent: 0, hurstRegime: 0,
    hmmBullProb: 0, hmmBearProb: 0,
    autocorrLag1: 0, regimeAdjustedScore: 0, edgeScore: 0,
    srTicks: 0, srMean: 0, srStd: 0, srZ: 0,
    midAcfZ: 0, tickVol: 0, spreadHist: [0,0,0,0,0,0]
  });

  let strategy = $state({
    asBid: 0, asAsk: 0, asReservation: 0,
    asOptimalSpreadBps: 0, asSkewBps: 0, asSigmaBps: 0,
    mmNetPnl: 0, mmRealPnl: 0, mmUnrealPnl: 0,
    mmInventory: 0, mmWinRate: 0, mmFillRate: 0,
    mmInventoryAlert: false, mmQuotingGated: false,
    latEdgeBps: 0, latCumEdgeUSD: 0, latSharpe: 0,
    latOpportunities: 0, exchBid1: 0, exchAsk1: 0,
    exchBid2: 0, exchAsk2: 0, exchMidBps: 0,
    exchArbBps: 0, exchConn: false,
    replaySimPnl: 0, replayWinRate: 0, replayMAE: 0, replayMFE: 0
  });

  let crossExchange = $state({
    bid: 0, ask: 0, mid: 0, spread: 0, spreadBps: 0,
    connected: false, isSpot: true,
    binanceMid: 0, driftUSD: 0, driftBps: 0, arbNetBps: 0
  });

  let trades     = $state([]);
  let midHistory = $state([]);
  let spreadHist = $state([]);
  let latHistory = $state([]);

  let mps        = $state(0);
  let midChange  = $state(0);
  let maxBidQty  = $state(1);
  let maxAskQty  = $state(1);
  let ofiSignal  = $state('SIDEWAYS');
  let seqGap     = $state(false);
  let lastSeq    = $state(0);
  let flashMid   = $state('');
  let flashTimer = null;

  // Tab state for Super Engine panel
  let activeTab = $state('ALL');
  const tabs = ['ALL', 'SIGNALS', 'MICRO', 'RISK', 'REGIME', 'STRAT'];

  // ── Internal ─────────────────────────────────────────────────────────────────
  let ws, reconnectTimer, mpsInterval;
  let msgsThisSec = 0;
  let lastMid = 0;
  let prevSeq = 0;

  // ── Canvas refs ──────────────────────────────────────────────────────────────
  let pixiCanvas, sparkCanvas, spreadCanvas, latCanvas;
  let pixiApp, depthGfx;

  // ── WebSocket ────────────────────────────────────────────────────────────────
  function connect() {
    wsStatus = 'CONNECTING';
    ws = new WebSocket(WS_URL);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      connected = true;
      wsStatus  = 'LIVE';
    };

    ws.onmessage = (evt) => {
      msgsThisSec++;
      try { handleMessage(JSON.parse(evt.data)); } catch {}
    };

    ws.onclose = () => {
      connected = false;
      wsStatus  = 'OFFLINE';
      reconnectTimer = setTimeout(connect, RECONNECT_MS);
    };

    ws.onerror = () => { ws.close(); };
  }

  function handleMessage(msg) {
    if (msg.type === 'snapshot') {
      bids = (msg.bids || []).slice(0, BOOK_DEPTH);
      asks = (msg.asks || []).slice(0, BOOK_DEPTH);
      analytics = { ...analytics, ...(msg.analytics || {}) };

      const seq = msg.analytics?.seqNum || 0;
      seqGap = (prevSeq > 0 && seq > 0 && seq - prevSeq > 1);
      prevSeq = seq;
      lastSeq = seq;

      const mid = analytics.mid || 0;
      if (mid !== 0) {
        const delta = mid - lastMid;
        midChange = delta;
        if (lastMid !== 0) {
          const dir = delta >= 0 ? 'up' : 'dn';
          flashMid = dir;
          clearTimeout(flashTimer);
          flashTimer = setTimeout(() => { flashMid = ''; }, 350);
        }
        lastMid = mid;
        midHistory = [...midHistory, mid].slice(-SPARKLINE_LEN);
      }

      if (analytics.spreadBps) {
        spreadHist = [...spreadHist, analytics.spreadBps].slice(-SPARKLINE_LEN);
      }

      const ofiNorm = analytics.ofi || 0;
      if      (ofiNorm >  OFI_THRESHOLD) ofiSignal = 'UP';
      else if (ofiNorm < -OFI_THRESHOLD) ofiSignal = 'DOWN';
      else                               ofiSignal = 'SIDEWAYS';

      maxBidQty = Math.max(...bids.map(b => b.qty || b.quantity || 1), 1);
      maxAskQty = Math.max(...asks.map(a => a.qty || a.quantity || 1), 1);

      drawDepthChart(bids, asks);
      drawSparkline();
      drawSpreadSparkline();

    } else if (msg.type === 'trade') {
      const t = {
        ...msg,
        ts: new Date(msg.ts || Date.now()).toISOString().slice(11, 23)
      };
      trades = [t, ...trades].slice(0, MAX_TRADES);

    } else if (msg.type === 'stats') {
      stats = { ...stats, ...msg };
<<<<<<< HEAD
=======
      if (msg.symbol) backendSymbol = msg.symbol;
>>>>>>> f815a5b (Updated engine, UI, and project configuration)
      latHistory = [...latHistory, {
        proc: msg.avgLatUs || 0,
        net:  msg.netLatUs || 0
      }].slice(-LAT_HISTORY);
      drawLatencyTimeline();

    } else if (msg.type === 'microstructure') {
      micro = { ...micro, ...msg };

    } else if (msg.type === 'signals') {
      signals = { ...signals, ...msg };

    } else if (msg.type === 'risk') {
      risk = { ...risk, ...msg };

    } else if (msg.type === 'bookDynamics') {
      bookDyn = { ...bookDyn, ...msg };

    } else if (msg.type === 'regime') {
      regimeData = { ...regimeData, ...msg };

    } else if (msg.type === 'strategy') {
      strategy = { ...strategy, ...msg };

    } else if (msg.type === 'crossExchange') {
      crossExchange = { ...crossExchange, ...msg };
    }
  }

  // ── PixiJS depth chart ────────────────────────────────────────────────────
  async function initPixi() {
    const PIXI = await import(
      'https://cdn.jsdelivr.net/npm/pixi.js@7.3.2/dist/pixi.min.mjs'
    );
    if (!pixiCanvas) return;

    pixiApp = new PIXI.Application({
      view:            pixiCanvas,
      width:           pixiCanvas.clientWidth  || 800,
      height:          pixiCanvas.clientHeight || 240,
      backgroundColor: 0x070809,
      antialias:       true,
      autoDensity:     true,
      resolution:      window.devicePixelRatio || 1,
    });

    depthGfx = new PIXI.Graphics();
    pixiApp.stage.addChild(depthGfx);

    const ro = new ResizeObserver(() => {
      if (!pixiApp?.renderer) return;
      pixiApp.renderer.resize(pixiCanvas.clientWidth, pixiCanvas.clientHeight);
    });
    ro.observe(pixiCanvas);
  }

  function drawDepthChart(rawBids, rawAsks) {
    if (!depthGfx || !pixiApp) return;
    const W = pixiApp.renderer.width;
    const H = pixiApp.renderer.height;
    const g = depthGfx;
    g.clear();
    if (!rawBids.length || !rawAsks.length) return;

    const getPrice = r => r.price ?? r.p ?? 0;
    const getQty   = r => r.qty   ?? r.q ?? 0;

    const bestBid  = getPrice(rawBids[0]);
    const bestAsk  = getPrice(rawAsks[0]);
    const mid      = (bestBid + bestAsk) / 2;
    const pWindow  = mid * 0.006;
    const pMin     = mid - pWindow;
    const pMax     = mid + pWindow;
    const toX      = p => ((p - pMin) / (pMax - pMin)) * W;

    const bidDepth = [];
    let cumBid = 0;
    for (const r of rawBids) {
      const p = getPrice(r);
      if (p < pMin) break;
      cumBid += getQty(r);
      bidDepth.unshift({ x: toX(p), y: cumBid });
    }

    const askDepth = [];
    let cumAsk = 0;
    for (const r of rawAsks) {
      const p = getPrice(r);
      if (p > pMax) break;
      cumAsk += getQty(r);
      askDepth.push({ x: toX(p), y: cumAsk });
    }

    const maxD  = Math.max(cumBid, cumAsk, 1);
    const yS    = v => H - (v / maxD) * (H * 0.82) - 8;
    const midX  = toX(mid);
    const bidX  = toX(bestBid);
    const askX  = toX(bestAsk);

    // Grid lines
    g.lineStyle(0.5, 0x1a2035, 1);
    for (let i = 1; i <= 4; i++) {
      const y = H - (i / 5) * H * 0.82 - 8;
      g.moveTo(0, y); g.lineTo(W, y);
    }
    for (let i = 1; i <= 6; i++) {
      const x = (i / 7) * W;
      g.moveTo(x, 0); g.lineTo(x, H);
    }

    // Bid fill
    if (bidDepth.length > 0) {
      g.lineStyle(0);
      g.beginFill(0x00e5a0, 0.13);
      g.moveTo(bidDepth[0].x, H);
      for (const { x, y } of bidDepth) g.lineTo(x, yS(y));
      g.lineTo(midX, H);
      g.closePath();
      g.endFill();

      g.lineStyle(1.5, 0x00e5a0, 0.95);
      g.moveTo(bidDepth[0].x, H);
      for (const { x, y } of bidDepth) g.lineTo(x, yS(y));
    }

    // Ask fill
    if (askDepth.length > 0) {
      g.lineStyle(0);
      g.beginFill(0xff3d5a, 0.13);
      g.moveTo(midX, H);
      for (const { x, y } of askDepth) g.lineTo(x, yS(y));
      g.lineTo(askDepth[askDepth.length - 1].x, H);
      g.closePath();
      g.endFill();

      g.lineStyle(1.5, 0xff3d5a, 0.95);
      g.moveTo(midX, H);
      for (const { x, y } of askDepth) g.lineTo(x, yS(y));
    }

    // Spread zone
    g.lineStyle(0);
    g.beginFill(0xf5c518, 0.05);
    g.drawRect(bidX, 0, askX - bidX, H);
    g.endFill();

    // Mid line
    g.lineStyle(1, 0x3d8fff, 0.7);
    g.moveTo(midX, 0); g.lineTo(midX, H);

    g.lineStyle(0.5, 0x00e5a0, 0.5);
    g.moveTo(bidX, 0); g.lineTo(bidX, H);
    g.lineStyle(0.5, 0xff3d5a, 0.5);
    g.moveTo(askX, 0); g.lineTo(askX, H);
  }

  // ── Sparklines ────────────────────────────────────────────────────────────
  function drawLine(canvas, data, color, fillColor) {
    if (!canvas || data.length < 2) return;
    const ctx = canvas.getContext('2d');
    canvas.width  = canvas.offsetWidth;
    canvas.height = canvas.offsetHeight;
    const W = canvas.width, H = canvas.height;
    const mn = Math.min(...data), mx = Math.max(...data);
    const rng = mx - mn || 0.001;
    ctx.clearRect(0, 0, W, H);
    ctx.strokeStyle = color;
    ctx.lineWidth   = 1.5;
    ctx.lineJoin    = 'round';
    ctx.lineCap     = 'round';
    ctx.beginPath();
    data.forEach((v, i) => {
      const x = (i / (data.length - 1)) * W;
      const y = H - ((v - mn) / rng) * (H - 6) - 3;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
    ctx.lineTo(W, H); ctx.lineTo(0, H);
    ctx.closePath();
    ctx.fillStyle = fillColor;
    ctx.fill();
  }

  const drawSparkline  = () => drawLine(sparkCanvas,  midHistory, '#3d8fff', 'rgba(61,143,255,0.09)');
  const drawSpreadSparkline = () => drawLine(spreadCanvas, spreadHist, '#f5c518', 'rgba(245,197,24,0.09)');

  // ── Latency timeline ──────────────────────────────────────────────────────
  function drawLatencyTimeline() {
    if (!latCanvas || latHistory.length < 2) return;
    const ctx = latCanvas.getContext('2d');
    latCanvas.width  = latCanvas.offsetWidth;
    latCanvas.height = latCanvas.offsetHeight;
    const W = latCanvas.width, H = latCanvas.height;
    const procVals = latHistory.map(l => l.proc);
    const netVals  = latHistory.map(l => l.net);
    const mx = Math.max(...procVals, ...netVals, 1);

    ctx.clearRect(0, 0, W, H);

    ctx.strokeStyle = 'rgba(26,32,53,0.8)';
    ctx.lineWidth = 0.5;
    for (let i = 1; i <= 3; i++) {
      const y = (i / 4) * H;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    }

    const bw = Math.max(2, W / latHistory.length - 1);
    latHistory.forEach((l, i) => {
      const x = (i / latHistory.length) * W;
      const h = (l.net / mx) * H * 0.85;
      ctx.fillStyle = 'rgba(167,139,250,0.25)';
      ctx.fillRect(x, H - h, bw - 1, h);
    });

    ctx.strokeStyle = '#22d3ee';
    ctx.lineWidth   = 1.5;
    ctx.lineJoin    = 'round';
    ctx.beginPath();
    procVals.forEach((v, i) => {
      const x = (i / (procVals.length - 1)) * W;
      const y = H - (v / mx) * H * 0.85 - 2;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────
  onMount(async () => {
    connect();
    mpsInterval = setInterval(() => { mps = msgsThisSec; msgsThisSec = 0; }, 1000);
    await initPixi();
  });

  onDestroy(() => {
    ws?.close();
    clearTimeout(reconnectTimer);
    clearTimeout(flashTimer);
    clearInterval(mpsInterval);
    pixiApp?.destroy(true, { children: true });
  });

  // ── Helpers ───────────────────────────────────────────────────────────────
  const fmt   = (v, d = 2) => Number(v || 0).toFixed(d);
  const fmtK  = (v) => {
    v = Number(v || 0);
    if (v >= 1e6) return (v / 1e6).toFixed(2) + 'M';
    if (v >= 1e3) return (v / 1e3).toFixed(1) + 'K';
    return v.toFixed(2);
  };
  const fmtUs = (v) => {
    v = Number(v || 0);
    return v < 1000 ? v.toFixed(2) + 'μs' : (v / 1000).toFixed(3) + 'ms';
  };
  const fmtNum = (v) => Number(v || 0).toLocaleString();

  const qty   = r => r.qty ?? r.quantity ?? 0;
  const price = r => r.price ?? r.p ?? 0;

  function cumBidTotal(idx) {
    return bids.slice(0, idx + 1).reduce((s, r) => s + qty(r), 0);
  }
  function cumAskTotal(idx) {
    return asks.slice(idx).reduce((s, r) => s + qty(r), 0);
  }

  const ofiColor = $derived(
    ofiSignal === 'UP' ? 'var(--bid)' :
    ofiSignal === 'DOWN' ? 'var(--ask)' : 'var(--text-mid)'
  );
  const ofiArrow = $derived(
    ofiSignal === 'UP' ? '↑' : ofiSignal === 'DOWN' ? '↓' : '↔'
  );
  const ofiLabel = $derived(
    ofiSignal === 'UP' ? 'BUY PRESSURE' :
    ofiSignal === 'DOWN' ? 'SELL PRESSURE' : 'BALANCED'
  );

  const imbalPct = $derived(
    Math.min(100, Math.abs((analytics.imbal || 0) * 100))
  );
  const imbalPositive = $derived((analytics.imbal || 0) >= 0);
  const midUp = $derived(midChange >= 0);

  // Regime label helpers
  const volRegimeLabel = $derived(
    ['LOW', 'NORMAL', 'HIGH', 'EXTREME'][regimeData.volRegime] ?? 'N/A'
  );
  const hurstLabel = $derived(
    regimeData.hurstRegime === -1 ? 'MEAN-REV' :
    regimeData.hurstRegime === 1  ? 'TRENDING' : 'RANDOM'
  );
  const hmmLabel = $derived(
    regimeData.hmmBullProb > 0.6 ? 'BULLISH' :
    regimeData.hmmBearProb > 0.6 ? 'BEARISH' : 'NEUTRAL'
  );
  const sigRegimeLabel = $derived(
    signals.regime === 1  ? 'TREND' :
    signals.regime === -1 ? 'MEAN-REV' : 'NEUTRAL'
  );

  // Risk heat level for the DV01 bar
  const riskHeatPct = $derived(Math.min(100, Math.abs(risk.dv01) * 10000));
  const riskHeatColor = $derived(
    riskHeatPct > 70 ? 'var(--ask)' :
    riskHeatPct > 40 ? 'var(--yellow)' : 'var(--bid)'
  );
</script>

<!-- ══ Markup ═══════════════════════════════════════════════════════════════ -->
<div class="shell">

  <!-- ── Top bar ─────────────────────────────────────────────────────────── -->
  <header>
    <div class="brand">
      <span class="brand-hex">◆</span>
      <span class="brand-text">HFT ENGINE</span>
      <span class="brand-version">v3.0</span>
    </div>

    <div class="h-sep"></div>

    <div class="hstat">
      <span class="hl">SYMBOL</span>
<<<<<<< HEAD
      <span class="hv accent">SOLUSDT</span>
=======
      <span class="hv accent">{backendSymbol}</span>
>>>>>>> f815a5b (Updated engine, UI, and project configuration)
    </div>

    <div class="hstat">
      <span class="hl">STATUS</span>
      <span class="hv" class:green={connected} class:red={!connected} class:dim={wsStatus === 'CONNECTING'}>
        {wsStatus}
      </span>
    </div>

    <div class="hstat">
      <span class="hl">SEQ #</span>
      <span class="hv" class:red={seqGap}>{fmtNum(lastSeq)}</span>
    </div>

    <div class="h-sep"></div>

    <div class="hstat">
      <span class="hl">MID PRICE</span>
      <span class="hv mid-hv"
        class:flash-up={flashMid === 'up'}
        class:flash-dn={flashMid === 'dn'}
        class:green={midUp}
        class:red={!midUp}>
        ${fmt(analytics.mid, 4)}
      </span>
    </div>

    <div class="hstat">
      <span class="hl">SPREAD</span>
      <span class="hv yellow">{fmt(analytics.spreadBps, 2)} <span class="unit">bps</span></span>
    </div>

    <div class="hstat">
      <span class="hl">OFI SIGNAL</span>
      <span class="hv" style="color:{ofiColor}">{ofiArrow} {ofiSignal}</span>
    </div>

    <div class="h-sep"></div>

    <div class="hstat">
      <span class="hl">VPIN</span>
      <span class="hv" class:red={signals.vpin > 0.65} class:yellow={signals.vpin > 0.4 && signals.vpin <= 0.65}>{fmt(signals.vpin, 3)}</span>
    </div>

    <div class="hstat">
      <span class="hl">REGIME</span>
      <span class="hv" style="color:{regimeData.volRegime >= 2 ? 'var(--ask)' : regimeData.volRegime === 1 ? 'var(--yellow)' : 'var(--bid)'}">{volRegimeLabel}</span>
    </div>

    <div class="hstat">
      <span class="hl">MSG/s</span>
      <span class="hv purple">{fmtNum(mps)}</span>
    </div>

    <div class="hstat">
      <span class="hl">AVG LAT</span>
      <span class="hv teal">{fmtUs(stats.avgLatUs)}</span>
    </div>

    <span class="spacer"></span>

    {#if signals.spoofingAlert}
      <span class="alert-badge red-bg">SPOOF</span>
    {/if}
    {#if signals.icebergDetected}
      <span class="alert-badge blue-bg">ICEBERG</span>
    {/if}
    {#if seqGap}
      <span class="gap-badge">SEQ GAP</span>
    {/if}

    <div class="conn-dot" class:active={connected} class:connecting={wsStatus === 'CONNECTING'}></div>
    <span class="conn-lbl" class:green={connected}>{wsStatus}</span>
  </header>

  <!-- ── Main grid ──────────────────────────────────────────────────────── -->
  <div class="grid">

    <!-- ─── Col 1: OrderBook ─────────────────────────────────────────────── -->
    <section class="panel ob-panel" id="orderbook-panel">
      <div class="ph">
        <span class="panel-dot" style="background:var(--bid)"></span>
        <span class="ptitle">Order Book</span>
        <span class="pbadge">DEPTH {BOOK_DEPTH}</span>
        <span class="pbadge ml-auto">{bids.length + asks.length} levels</span>
      </div>

      <div class="ob-hdr">
        <span>PRICE</span><span></span><span>QTY</span><span>TOTAL</span>
      </div>

      <!-- Asks -->
      <div class="ob-side asks-side">
        {#each [...asks].reverse() as row, i (price(row))}
          {@const q = qty(row)}
          {@const cumAsk = cumAskTotal(asks.length - 1 - i)}
          <div class="ob-row ask-row">
            <div class="depth-bar ask-bar" style="width:{(q / maxAskQty * 100).toFixed(1)}%"></div>
            <span class="ob-price ask-price">{fmt(price(row), 4)}</span>
            <span class="ob-change"></span>
            <span class="ob-qty">{fmtK(q)}</span>
            <span class="ob-total">{fmtK(cumAsk)}</span>
          </div>
        {/each}
      </div>

      <!-- Spread bar -->
      <div class="spread-bar">
        <div class="spread-inner">
          <span class="sl">BID</span>
          <span class="sb-price green">{fmt(analytics.bestBid, 4)}</span>
          <div class="spread-mid">
            <span class="mid-val"
              class:flash-up={flashMid === 'up'}
              class:flash-dn={flashMid === 'dn'}>
              {fmt(analytics.mid, 4)}
            </span>
            <span class="spread-bps">{fmt(analytics.spreadBps, 2)} bps</span>
          </div>
          <span class="sb-price red">{fmt(analytics.bestAsk, 4)}</span>
          <span class="sl">ASK</span>
        </div>
      </div>

      <!-- Bids -->
      <div class="ob-side bids-side">
        {#each bids as row, i (price(row))}
          {@const q = qty(row)}
          {@const cumBid = cumBidTotal(i)}
          <div class="ob-row bid-row">
            <div class="depth-bar bid-bar" style="width:{(q / maxBidQty * 100).toFixed(1)}%"></div>
            <span class="ob-price bid-price">{fmt(price(row), 4)}</span>
            <span class="ob-change"></span>
            <span class="ob-qty">{fmtK(q)}</span>
            <span class="ob-total">{fmtK(cumBid)}</span>
          </div>
        {/each}
      </div>

      <!-- Imbalance meter -->
      <div class="imbal-meter">
        <span class="im-lbl green">BID</span>
        <div class="im-track">
          <div class="im-fill"
            style="
              width:{imbalPct}%;
              {imbalPositive ? 'left:50%;' : 'right:50%;'}
              background:{imbalPositive ? 'var(--bid)' : 'var(--ask)'};
            ">
          </div>
        </div>
        <span class="im-lbl red">ASK</span>
        <span class="im-val" style="color:{imbalPositive ? 'var(--bid)' : 'var(--ask)'}">
          {((analytics.imbal || 0) * 100).toFixed(1)}%
        </span>
      </div>
    </section>

    <!-- ─── Col 2 top: Depth Chart ────────────────────────────────────────── -->
    <section class="panel depth-panel" id="depth-panel">
      <div class="ph">
        <span class="panel-dot" style="background:var(--accent)"></span>
        <span class="ptitle">Cumulative Market Depth</span>
        <span class="pbadge">WebGL · PixiJS</span>
        <div class="chart-legend">
          <span class="leg green">▲ Bids</span>
          <span class="leg yellow">Spread</span>
          <span class="leg red">▼ Asks</span>
          <span class="leg accent">— Mid</span>
        </div>
      </div>
      <canvas bind:this={pixiCanvas} class="pixi-canvas"></canvas>
    </section>

    <!-- ─── Col 3: Analytics ────────────────────────────────────────────── -->
    <section class="panel stats-panel" id="analytics-panel">
      <div class="ph">
        <span class="panel-dot" style="background:var(--purple)"></span>
        <span class="ptitle">Analytics</span>
      </div>

      <!-- OFI Signal card -->
      <div class="ofi-card" style="--ofi-color:{ofiColor}">
        <div class="ofi-arrow" style="color:{ofiColor}">{ofiArrow}</div>
        <div class="ofi-body">
          <span class="ofi-signal" style="color:{ofiColor}">{ofiSignal}</span>
          <span class="ofi-label">{ofiLabel}</span>
          <span class="ofi-val">OFI {fmt(analytics.ofi, 4)}</span>
        </div>
      </div>

      <div class="stat-list">
        <div class="sr"><span class="sk">Best Bid</span><span class="sv green">{fmt(analytics.bestBid, 4)}</span></div>
        <div class="sr"><span class="sk">Best Ask</span><span class="sv red">{fmt(analytics.bestAsk, 4)}</span></div>
        <div class="sr"><span class="sk">Mid Price</span>
          <span class="sv" class:green={midUp} class:red={!midUp}>
            {fmt(analytics.mid, 4)}
            <span class="delta" class:green={midUp} class:red={!midUp}>
              {midChange >= 0 ? '+' : ''}{fmt(midChange, 4)}
            </span>
          </span>
        </div>
        <div class="sr"><span class="sk">Spread</span><span class="sv yellow">{fmt(analytics.spreadBps, 2)} bps</span></div>
        <div class="sr"><span class="sk">Imbalance</span>
          <span class="sv" class:green={imbalPositive} class:red={!imbalPositive}>
            {((analytics.imbal || 0) * 100).toFixed(2)}%
          </span>
        </div>

        <div class="stat-divider"></div>

        <div class="sr"><span class="sk">Msgs Rx</span><span class="sv">{fmtNum(stats.msgsReceived)}</span></div>
        <div class="sr"><span class="sk">Msgs OK</span><span class="sv teal">{fmtNum(stats.msgsProcessed)}</span></div>
        <div class="sr"><span class="sk">Dropped</span>
          <span class="sv" class:red={stats.msgsDropped > 0}>{fmtNum(stats.msgsDropped)}</span>
        </div>

        <div class="stat-divider"></div>

        <div class="sr"><span class="sk">Avg Proc Lat</span><span class="sv teal">{fmtUs(stats.avgLatUs)}</span></div>
        <div class="sr"><span class="sk">Max Proc Lat</span><span class="sv yellow">{fmtUs(stats.maxLatUs)}</span></div>
        <div class="sr"><span class="sk">Min Proc Lat</span><span class="sv green">{fmtUs(stats.minLatUs)}</span></div>
        <div class="sr"><span class="sk">Net Latency</span><span class="sv purple">{fmtUs(stats.netLatUs)}</span></div>

        <div class="stat-divider"></div>

        <div class="spark-row">
          <span class="sk">Mid Price</span>
          <canvas bind:this={sparkCanvas} class="spark"></canvas>
        </div>
        <div class="spark-row">
          <span class="sk">Spread bps</span>
          <canvas bind:this={spreadCanvas} class="spark"></canvas>
        </div>
      </div>
    </section>

    <!-- ─── Col 4: Super Engine Insights ────────────────────────────────── -->
    <section class="panel engine-panel" id="engine-panel">
      <div class="ph">
        <span class="panel-dot" style="background:var(--orange)"></span>
        <span class="ptitle">Super Engine Insights</span>
        <span class="pbadge ml-auto">LIVE</span>
      </div>

      <!-- Tab bar -->
      <div class="tab-bar">
        {#each tabs as tab}
          <button
            class="tab-btn"
            class:active={activeTab === tab}
            onclick={() => activeTab = tab}
          >
            {tab}
          </button>
        {/each}
      </div>

      <!-- Tab content: scrollable -->
      <div class="engine-scroll">

        <!-- ─── POSITION & RISK (always visible) ───────────────────────── -->
        <div class="engine-section">
          <div class="es-header">POSITION & RISK</div>
          <div class="sr"><span class="sk">Current PnL</span>
            <span class="sv" class:green={risk.totalPnl >= 0} class:red={risk.totalPnl < 0}>
              {fmt(risk.totalPnl, 4)}
            </span>
          </div>
          <div class="sr"><span class="sk">Realized PnL</span>
            <span class="sv" class:green={risk.realizedPnl >= 0} class:red={risk.realizedPnl < 0}>
              {fmt(risk.realizedPnl, 4)}
            </span>
          </div>
          <div class="sr"><span class="sk">Position Size</span><span class="sv">{fmt(risk.positionSize, 4)}</span></div>
          <div class="sr"><span class="sk">Slippage</span><span class="sv yellow">{fmt(risk.slipAvg, 2)} bps</span></div>
          <div class="sr"><span class="sk">Win Rate</span><span class="sv teal">{fmt(risk.winRate * 100, 1)}%</span></div>
          <div class="sr"><span class="sk">Total Fill Prob</span><span class="sv">{fmt(risk.fillProb100ms * 100, 1)}%</span></div>
          <div class="sr"><span class="sk">Composite Edge</span>
            <span class="sv" class:green={risk.compositeScore > 0.1} class:red={risk.compositeScore < -0.1}>
              {fmt(risk.compositeScore, 3)}
            </span>
          </div>
          <div class="sr"><span class="sk">Risk Heat Level</span>
            <div class="heat-bar-wrap">
              <div class="heat-bar" style="width:{riskHeatPct}%; background:{riskHeatColor}"></div>
            </div>
          </div>
        </div>

        <!-- ─── SIGNALS tab ─────────────────────────────────────────────── -->
        {#if activeTab === 'ALL' || activeTab === 'SIGNALS'}
        <div class="engine-section">
          <div class="es-header">SIGNAL ENGINE</div>
          <div class="sr"><span class="sk">OFI (L1-L5)</span>
            <span class="sv" class:green={signals.ofiNormalized > 0.05} class:red={signals.ofiNormalized < -0.05}>
              {fmt(signals.ofiNormalized, 4)}
            </span>
          </div>
          <div class="sr"><span class="sk">VPIN Toxicity</span>
            <span class="sv" class:red={signals.vpin > 0.65} class:yellow={signals.vpin > 0.4 && signals.vpin <= 0.65}>
              {fmt(signals.vpin, 4)} <span class="unit">({signals.vpinBuckets} bkt)</span>
            </span>
          </div>
          <div class="sr"><span class="sk">Momentum</span>
            <span class="sv">{fmt(signals.momentumScore, 4)} <span class="unit">z={fmt(signals.zScore, 2)}</span></span>
          </div>
          <div class="sr"><span class="sk">AR(1) Coeff</span><span class="sv">{fmt(signals.ar1Coeff, 4)}</span></div>
          <div class="sr"><span class="sk">Regime</span>
            <span class="sv" style="color:{signals.regime === 1 ? 'var(--bid)' : signals.regime === -1 ? 'var(--ask)' : 'var(--text-mid)'}">
              {sigRegimeLabel}
            </span>
          </div>
          <div class="sr"><span class="sk">Quote Stuffing</span>
            <span class="sv" class:red={signals.stuffingRatio > 10}>
              ratio={fmt(signals.stuffingRatio, 1)}
            </span>
          </div>
          {#if signals.icebergDetected}
          <div class="sr alert-row"><span class="sk">Iceberg</span>
            <span class="sv red">DETECTED @ {fmt(signals.icebergPrice, 2)} ({signals.icebergRefills} refills)</span>
          </div>
          {/if}
          {#if signals.spoofingAlert}
          <div class="sr alert-row"><span class="sk">Spoofing</span>
            <span class="sv red">ALERT @ {fmt(signals.spoofPrice, 2)}</span>
          </div>
          {/if}
        </div>
        {/if}

        <!-- ─── MICRO tab ────────────────────────────────────────────────── -->
        {#if activeTab === 'ALL' || activeTab === 'MICRO'}
        <div class="engine-section">
          <div class="es-header">MICROSTRUCTURE</div>
          <div class="sr"><span class="sk">VWAP (60s)</span><span class="sv accent">{fmt(micro.vwap, 4)}</span></div>
          <div class="sr"><span class="sk">TWAP (60s)</span><span class="sv">{fmt(micro.twap, 4)}</span></div>
          <div class="sr"><span class="sk">Microprice</span><span class="sv accent">{fmt(micro.microprice, 4)}</span></div>
          <div class="sr"><span class="sk">Kyle's λ</span><span class="sv yellow">{fmt(micro.kyleLambda, 6)}</span></div>
          <div class="sr"><span class="sk">Amihud ILLIQ</span>
            <span class="sv" class:green={micro.amihudIlliq < 0.5} class:red={micro.amihudIlliq > 2}>
              {fmt(micro.amihudIlliq, 4)}
            </span>
          </div>
          <div class="sr"><span class="sk">Roll Spread</span><span class="sv">{fmt(micro.rollSpread, 5)} USD</span></div>
          <div class="sr"><span class="sk">Signed Flow</span>
            <span class="sv" class:green={micro.signedFlow > 0} class:red={micro.signedFlow < 0}>
              {fmt(micro.signedFlow, 2)}
            </span>
          </div>
          <div class="sr"><span class="sk">Trade Count</span><span class="sv teal">{fmtNum(micro.tradeCount)}</span></div>
          <div class="sr"><span class="sk">Last Trade</span>
            <span class="sv" class:green={micro.lastTradeSide > 0} class:red={micro.lastTradeSide < 0}>
              {micro.lastTradeSide > 0 ? 'BUY' : micro.lastTradeSide < 0 ? 'SELL' : '—'} @ {fmt(micro.lastTradePrice, 4)}
            </span>
          </div>
        </div>
        {/if}

        <!-- ─── RISK tab ─────────────────────────────────────────────────── -->
        {#if activeTab === 'ALL' || activeTab === 'RISK'}
        <div class="engine-section">
          <div class="es-header">RISK & PnL ENGINE</div>
          <div class="sr"><span class="sk">Entry Price</span><span class="sv">{fmt(risk.entryPrice, 4)}</span></div>
          <div class="sr"><span class="sk">Notional</span><span class="sv">${fmt(risk.positionNotional, 2)}</span></div>
          <div class="sr"><span class="sk">Sharpe (per-trade)</span><span class="sv teal">{fmt(risk.sharpePerTrade, 3)}</span></div>
          <div class="sr"><span class="sk">Max Drawdown</span><span class="sv red">${fmt(risk.maxDrawdown, 4)}</span></div>
          <div class="sr"><span class="sk">DV01</span><span class="sv yellow">${fmt(risk.dv01, 5)}</span></div>
          <div class="sr"><span class="sk">Liquidation Cost</span><span class="sv">${fmt(risk.liquidCost, 5)}</span></div>
          <div class="sr"><span class="sk">Market Impact</span><span class="sv">${fmt(risk.mktImpact, 5)}</span></div>
          <div class="sr"><span class="sk">Kalman Price</span><span class="sv accent">{fmt(risk.kalmanPrice, 4)}</span></div>
          <div class="sr"><span class="sk">Kalman Velocity</span>
            <span class="sv" class:green={risk.kalmanVelocity > 0} class:red={risk.kalmanVelocity < 0}>
              {fmt(risk.kalmanVelocity, 6)} {risk.kalmanVelocity > 0 ? '↑' : '↓'}
            </span>
          </div>
          <div class="sr"><span class="sk">Hit Rate</span>
            <span class="sv teal">{fmt(risk.hitRate * 100, 1)}%</span>
          </div>
          <div class="sr"><span class="sk">Impl Shortfall</span>
            <span class="sv yellow">{fmt(risk.slipImpl, 2)} bps</span>
          </div>
        </div>
        {/if}

        <!-- ─── REGIME tab ────────────────────────────────────────────────── -->
        {#if activeTab === 'ALL' || activeTab === 'REGIME'}
        <div class="engine-section">
          <div class="es-header">REGIME & MARKET STATE</div>
          <div class="sr"><span class="sk">Volatility (YZ)</span>
            <span class="sv" style="color:{regimeData.volRegime >= 2 ? 'var(--ask)' : regimeData.volRegime === 1 ? 'var(--yellow)' : 'var(--bid)'}">
              {fmt(regimeData.realizedVolAnnualized, 1)}% [{volRegimeLabel}]
            </span>
          </div>
          <div class="sr"><span class="sk">Hurst (R/S)</span>
            <span class="sv">{fmt(regimeData.hurstExponent, 4)} [{hurstLabel}]</span>
          </div>
          <div class="sr"><span class="sk">HMM State</span>
            <span class="sv" style="color:{regimeData.hmmBullProb > 0.6 ? 'var(--bid)' : regimeData.hmmBearProb > 0.6 ? 'var(--ask)' : 'var(--text-mid)'}">
              {hmmLabel} (P={fmt(Math.max(regimeData.hmmBullProb, regimeData.hmmBearProb), 3)})
            </span>
          </div>
          <div class="sr"><span class="sk">Mid ACF</span><span class="sv">{fmt(regimeData.autocorrLag1, 4)}</span></div>
          <div class="sr"><span class="sk">Spread (ticks)</span><span class="sv yellow">{fmt(regimeData.srTicks, 1)}</span></div>
          <div class="sr"><span class="sk">Regime Adj Score</span>
            <span class="sv" class:green={regimeData.regimeAdjustedScore > 0.1} class:red={regimeData.regimeAdjustedScore < -0.1}>
              {fmt(regimeData.regimeAdjustedScore, 4)}
            </span>
          </div>
          <div class="sr"><span class="sk">Edge Score</span>
            <span class="sv teal">{fmt(regimeData.edgeScore, 3)} bps</span>
          </div>
        </div>
        {/if}

        <!-- ─── STRATEGY tab ──────────────────────────────────────────────── -->
        {#if activeTab === 'ALL' || activeTab === 'STRAT'}
        <div class="engine-section">
          <div class="es-header">STRATEGY ENGINE</div>
          <div class="sr"><span class="sk">AS-MM Bid</span><span class="sv green">{fmt(strategy.asBid, 4)}</span></div>
          <div class="sr"><span class="sk">AS-MM Ask</span><span class="sv red">{fmt(strategy.asAsk, 4)}</span></div>
          <div class="sr"><span class="sk">Optimal δ*</span><span class="sv yellow">{fmt(strategy.asOptimalSpreadBps, 2)} bps</span></div>
          <div class="sr"><span class="sk">AS Skew</span><span class="sv">{fmt(strategy.asSkewBps, 2)} bps</span></div>
          <div class="sr"><span class="sk">MM Net PnL</span>
            <span class="sv" class:green={strategy.mmNetPnl >= 0} class:red={strategy.mmNetPnl < 0}>
              ${fmt(strategy.mmNetPnl, 4)}
            </span>
          </div>
          <div class="sr"><span class="sk">MM Win Rate</span><span class="sv teal">{fmt(strategy.mmWinRate * 100, 1)}%</span></div>
          <div class="sr"><span class="sk">MM Inventory</span>
            <span class="sv" class:red={strategy.mmInventoryAlert}>{fmt(strategy.mmInventory, 4)}</span>
          </div>
          <div class="sr"><span class="sk">Lat Arb Edge</span><span class="sv yellow">{fmt(strategy.latEdgeBps, 3)} bps</span></div>
          <div class="sr"><span class="sk">Lat Arb Sharpe</span><span class="sv teal">{fmt(strategy.latSharpe, 2)}</span></div>
          <div class="sr"><span class="sk">XchgArb</span>
            <span class="sv" class:green={strategy.exchArbBps > 1}>
              {fmt(strategy.exchArbBps, 2)} bps
              {strategy.exchConn ? '[conn]' : '[disc]'}
            </span>
          </div>
          {#if strategy.mmQuotingGated}
          <div class="sr alert-row"><span class="sk">Status</span><span class="sv yellow">GATED</span></div>
          {/if}
        </div>
        {/if}

        <!-- ─── CROSS-EXCHANGE (visible in ALL and STRAT) ───────────────── -->
        {#if activeTab === 'ALL' || activeTab === 'STRAT'}
        <div class="engine-section">
          <div class="es-header">CROSS-EXCHANGE FEED
            <span class="es-conn" class:conn-on={crossExchange.connected || strategy.exchConn}>
              {(crossExchange.connected || strategy.exchConn) ? 'CONNECTED' : 'DISCONNECTED'}
            </span>
          </div>
          <div class="sr"><span class="sk">Feed</span>
            <span class="sv">{crossExchange.isSpot ? 'Bybit (SPOT)' : 'Bybit (PERP)'}</span>
          </div>
          <div class="sr"><span class="sk">Bid</span>
            <span class="sv green">{fmt(crossExchange.bid || strategy.exchBid1, 4)}</span>
          </div>
          <div class="sr"><span class="sk">Ask</span>
            <span class="sv red">{fmt(crossExchange.ask || strategy.exchAsk1, 4)}</span>
          </div>
          <div class="sr"><span class="sk">Mid</span>
            <span class="sv">{fmt(crossExchange.mid || ((strategy.exchBid1 + strategy.exchAsk1) / 2), 4)}</span>
          </div>
          <div class="sr"><span class="sk">Spread</span>
            <span class="sv yellow">{fmt(crossExchange.spreadBps || strategy.exchMidBps, 2)} bps</span>
          </div>
          <div class="sr"><span class="sk">Binance Mid</span>
            <span class="sv accent">{fmt(crossExchange.binanceMid || analytics.mid, 4)}</span>
          </div>
          <div class="sr"><span class="sk">Drift (vs Binance)</span>
            <span class="sv" class:green={crossExchange.driftBps > 0} class:red={crossExchange.driftBps < 0}>
              {crossExchange.driftBps >= 0 ? '+' : ''}{fmt(crossExchange.driftBps, 2)} bps
            </span>
          </div>
          <div class="sr"><span class="sk">Arb Opportunity</span>
            <span class="sv" class:green={strategy.exchArbBps > 1}>
              {fmt(strategy.exchArbBps, 2)} bps
              {strategy.exchArbBps > 3 ? '*** ARB! ***' : ''}
            </span>
          </div>
        </div>
        {/if}

      </div>
    </section>

    <!-- ─── Col 2 bottom: Trade Tape ─────────────────────────────────────── -->
    <section class="panel tape-panel" id="trade-tape">
      <div class="ph">
        <span class="panel-dot" style="background:var(--yellow)"></span>
        <span class="ptitle">Trade Tape</span>
        <span class="pbadge">{trades.length} fills</span>
      </div>
      <div class="tape-hdr">
        <span>SIDE</span><span>PRICE</span><span>QTY</span><span>SIZE</span><span>TIME</span>
      </div>
      <div class="tape-scroll">
        {#each trades as t (`${t.seqNum}-${t.ts}`)}
          {@const tqty = t.qty ?? t.quantity ?? 0}
          {@const maxTapeQty = Math.max(...trades.map(x => x.qty ?? x.quantity ?? 0), 1)}
          <div class="tape-row" class:buy={t.side === 'buy'} class:sell={t.side === 'sell'}>
            <span class="side-badge" class:buy={t.side === 'buy'} class:sell={t.side === 'sell'}>
              {(t.side || '').toUpperCase()}
            </span>
            <span class="t-price" class:green={t.side === 'buy'} class:red={t.side === 'sell'}>
              {fmt(t.price ?? t.p ?? 0, 4)}
            </span>
            <span class="t-qty">{fmtK(tqty)}</span>
            <div class="t-bar-wrap">
              <div class="t-bar"
                class:buy={t.side === 'buy'} class:sell={t.side === 'sell'}
                style="width:{(tqty / maxTapeQty * 100).toFixed(0)}%">
              </div>
            </div>
            <span class="t-time">{t.ts}</span>
          </div>
        {/each}
        {#if trades.length === 0}
          <div class="empty-tape">Waiting for fills…</div>
        {/if}
      </div>
    </section>

    <!-- ─── Latency Timeline ───────────────────────────────────────────── -->
    <section class="panel lat-panel" id="latency-panel">
      <div class="ph">
        <span class="panel-dot" style="background:var(--teal)"></span>
        <span class="ptitle">Latency Timeline</span>
        <div class="lat-legend">
          <span class="leg teal">— Proc</span>
          <span class="leg purple">▪ Net</span>
        </div>
      </div>
      <canvas bind:this={latCanvas} class="lat-canvas"></canvas>
      <div class="lat-foot">
        <span class="lf" style="color:var(--teal)">Proc: {fmtUs(stats.avgLatUs)}</span>
        <span class="lf" style="color:var(--purple)">Net: {fmtUs(stats.netLatUs)}</span>
        <span class="lf" style="color:var(--yellow)">Max: {fmtUs(stats.maxLatUs)}</span>
      </div>
    </section>

    <!-- ─── Bottom strip ─────────────────────────────────────────────────── -->
    <div class="bottom-strip">
      <div class="bm">
        <span class="bml">Mid Price</span>
        <span class="bmv" class:green={midUp} class:red={!midUp}>{fmt(analytics.mid, 2)}</span>
        <span class="bms" class:green={midUp} class:red={!midUp}>{midChange >= 0 ? '+' : ''}{fmt(midChange, 4)}</span>
      </div>
      <div class="bm">
        <span class="bml">Spread</span>
        <span class="bmv yellow">{fmt(analytics.spreadBps, 2)}</span>
        <span class="bms">bps</span>
      </div>
      <div class="bm">
        <span class="bml">OFI Signal</span>
        <span class="bmv" style="color:{ofiColor}">{ofiArrow} {ofiSignal}</span>
        <span class="bms" style="color:{ofiColor}">{fmt(analytics.ofi, 4)}</span>
      </div>
      <div class="bm">
        <span class="bml">Imbalance</span>
        <span class="bmv" class:green={imbalPositive} class:red={!imbalPositive}>
          {((analytics.imbal || 0) * 100).toFixed(1)}%
        </span>
        <span class="bms">{imbalPositive ? 'Bid heavy' : 'Ask heavy'}</span>
      </div>
      <div class="bm">
        <span class="bml">Msg/s</span>
        <span class="bmv purple">{fmtNum(mps)}</span>
        <span class="bms">{fmtNum(stats.msgsDropped)} dropped</span>
      </div>
      <div class="bm">
        <span class="bml">Avg Latency</span>
        <span class="bmv teal">{fmtUs(stats.avgLatUs)}</span>
        <span class="bms">max {fmtUs(stats.maxLatUs)}</span>
      </div>
      <div class="bm">
        <span class="bml">Net Latency</span>
        <span class="bmv purple">{fmtUs(stats.netLatUs)}</span>
        <span class="bms">wire-to-process</span>
      </div>
      <div class="bm">
        <span class="bml">Sequence</span>
        <span class="bmv" class:red={seqGap}>{fmtNum(lastSeq)}</span>
        <span class="bms" class:red={seqGap}>{seqGap ? 'GAP DETECTED' : 'Continuous'}</span>
      </div>
    </div>

  </div><!-- /grid -->
</div><!-- /shell -->

<!-- ══ Styles ═══════════════════════════════════════════════════════════════ -->
<style>
  /* ── Layout shell ──────────────────────────────────────────────────────── */
  .shell {
    display: grid;
    grid-template-rows: 44px 1fr;
    height: 100vh;
    width: 100vw;
    background: var(--bg0);
    overflow: hidden;
  }

  /* ── Header ─────────────────────────────────────────────────────────────── */
  header {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 0 14px;
    background: var(--bg1);
    border-bottom: 1px solid var(--brd);
    position: relative;
    overflow: hidden;
  }
  header::after {
    content: '';
    position: absolute;
    bottom: 0; left: 0; right: 0;
    height: 1px;
    background: linear-gradient(90deg, var(--accent) 0%, transparent 60%);
    opacity: 0.4;
  }

  .brand {
    display: flex;
    align-items: center;
    gap: 7px;
    flex-shrink: 0;
  }
  .brand-hex { color: var(--accent); font-size: 14px; }
  .brand-text { font-size: 12px; font-weight: 700; letter-spacing: .18em; color: var(--text); }
  .brand-version { font-size: 9px; color: var(--text-dim); letter-spacing: .08em; }

  .h-sep { width: 1px; height: 22px; background: var(--brd2); flex-shrink: 0; }

  .hstat { display: flex; flex-direction: column; gap: 1px; flex-shrink: 0; }
  .hl { font-size: 8px; color: var(--text-dim); letter-spacing: .1em; text-transform: uppercase; }
  .hv { font-size: 11px; font-weight: 600; }
  .unit { font-size: 9px; font-weight: 400; color: var(--text-dim); }
  .mid-hv { font-size: 13px; transition: color .2s; }

  .spacer { flex: 1; }

  .gap-badge, .alert-badge {
    font-size: 8px; font-weight: 700; letter-spacing: .12em;
    padding: 2px 6px; border-radius: 2px;
    animation: blink-badge .6s ease infinite;
  }
  .gap-badge {
    background: rgba(255,61,90,.2); color: var(--ask);
    border: 1px solid var(--ask);
  }
  .alert-badge.red-bg {
    background: rgba(255,61,90,.25); color: #ff6b7f;
    border: 1px solid rgba(255,61,90,.5);
    margin-right: 4px;
  }
  .alert-badge.blue-bg {
    background: rgba(61,143,255,.25); color: var(--accent2);
    border: 1px solid rgba(61,143,255,.5);
    margin-right: 4px;
  }
  @keyframes blink-badge { 0%,100%{opacity:1} 50%{opacity:.4} }

  .conn-dot {
    width: 7px; height: 7px; border-radius: 50%;
    background: var(--text-dim); flex-shrink: 0;
    transition: background .3s;
  }
  .conn-dot.active { background: var(--bid); box-shadow: 0 0 6px var(--bid); animation: pulse-dot 1.8s ease infinite; }
  .conn-dot.connecting { background: var(--yellow); animation: pulse-dot .8s ease infinite; }
  @keyframes pulse-dot { 0%,100%{opacity:1} 50%{opacity:.4} }
  .conn-lbl { font-size: 9px; letter-spacing: .1em; flex-shrink: 0; }

  /* ── Main grid — 4-column ultra-wide ─────────────────────────────────────── */
  .grid {
    display: grid;
    grid-template-columns: 260px 1fr 210px 280px;
    grid-template-rows: 1fr 180px 42px;
    gap: 1px;
    background: var(--brd);
    height: calc(100vh - 44px);
    overflow: hidden;
  }

  /* ── Generic panel ───────────────────────────────────────────────────────── */
  .panel {
    background: var(--bg1);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    min-height: 0;
  }

  .ph {
    display: flex;
    align-items: center;
    gap: 7px;
    padding: 5px 10px;
    background: var(--bg2);
    border-bottom: 1px solid var(--brd);
    flex-shrink: 0;
  }
  .panel-dot { width: 5px; height: 5px; border-radius: 50%; flex-shrink: 0; }
  .ptitle { font-size: 9px; font-weight: 600; letter-spacing: .14em; text-transform: uppercase; color: var(--text-mid); }
  .pbadge {
    font-size: 8px; padding: 1px 5px; border-radius: 2px;
    background: var(--bg4); color: var(--text-dim);
  }
  .ml-auto { margin-left: auto; }

  /* ── OrderBook ───────────────────────────────────────────────────────────── */
  .ob-panel { grid-column: 1; grid-row: 1 / 3; }

  .ob-hdr {
    display: grid; grid-template-columns: 1fr 20px 50px 60px;
    padding: 3px 10px; font-size: 8px; color: var(--text-dim);
    letter-spacing: .08em; text-transform: uppercase;
    background: var(--bg2); border-bottom: 1px solid var(--brd);
    flex-shrink: 0;
  }
  .ob-hdr span:nth-child(n+3) { text-align: right; }

  .ob-side { flex: 1; overflow: hidden; display: flex; flex-direction: column; }
  .asks-side { justify-content: flex-end; }

  .ob-row {
    display: grid; grid-template-columns: 1fr 20px 50px 60px;
    padding: 1px 10px; position: relative;
    min-height: 18px; align-items: center;
    transition: background .12s;
  }
  .ob-row:hover { background: var(--bg3); }

  .depth-bar {
    position: absolute; top: 0; bottom: 0; right: 0;
    pointer-events: none;
    transition: width .15s ease;
  }
  .bid-bar { background: var(--bid-dim); }
  .ask-bar { background: var(--ask-dim); }

  .ob-price { font-size: 10px; font-weight: 600; z-index: 1; position: relative; font-variant-numeric: tabular-nums; }
  .bid-price { color: var(--bid); }
  .ask-price { color: var(--ask); }
  .ob-change { font-size: 8px; color: var(--text-xs); z-index: 1; }
  .ob-qty  { text-align: right; color: var(--text-mid); z-index: 1; position: relative; font-size: 10px; font-variant-numeric: tabular-nums; }
  .ob-total { text-align: right; color: var(--text-dim); z-index: 1; position: relative; font-size: 9px; font-variant-numeric: tabular-nums; }

  /* Spread bar */
  .spread-bar {
    flex-shrink: 0; padding: 5px 10px;
    background: var(--bg3); border-top: 1px solid var(--brd2); border-bottom: 1px solid var(--brd2);
  }
  .spread-inner {
    display: flex; align-items: center; justify-content: space-between; gap: 5px;
  }
  .sl { font-size: 7px; color: var(--text-dim); letter-spacing: .1em; }
  .sb-price { font-size: 10px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .spread-mid { display: flex; flex-direction: column; align-items: center; flex: 1; }
  .mid-val {
    font-size: 13px; font-weight: 700; color: var(--text);
    font-variant-numeric: tabular-nums;
    transition: color .25s, background .25s;
    padding: 0 4px; border-radius: 2px;
  }
  .spread-bps { font-size: 8px; color: var(--yellow); }

  /* Imbalance meter */
  .imbal-meter {
    display: flex; align-items: center; gap: 5px;
    padding: 4px 10px; flex-shrink: 0;
    border-top: 1px solid var(--brd); background: var(--bg2);
  }
  .im-lbl { font-size: 7px; font-weight: 600; flex-shrink: 0; }
  .im-track {
    flex: 1; height: 4px; background: var(--bg4);
    border-radius: 3px; position: relative; overflow: hidden;
  }
  .im-fill {
    position: absolute; top: 0; bottom: 0;
    border-radius: 3px; transition: width .2s, left .2s, right .2s;
  }
  .im-val { font-size: 8px; font-weight: 600; flex-shrink: 0; width: 40px; text-align: right; }

  /* ── Depth chart ─────────────────────────────────────────────────────────── */
  .depth-panel { grid-column: 2; grid-row: 1; }
  .pixi-canvas { flex: 1; width: 100%; min-height: 0; }

  .chart-legend { display: flex; gap: 14px; margin-left: auto; }
  .leg { font-size: 8px; letter-spacing: .06em; }

  /* ── Analytics panel (Col 3) ───────────────────────────────────────────── */
  .stats-panel { grid-column: 3; grid-row: 1 / 3; }

  /* OFI Signal card */
  .ofi-card {
    display: flex; align-items: center; gap: 10px;
    padding: 8px 12px; flex-shrink: 0;
    background: var(--bg2); border-bottom: 1px solid var(--brd);
    position: relative; overflow: hidden;
  }
  .ofi-card::before {
    content: ''; position: absolute; left: 0; top: 0; bottom: 0; width: 2px;
    background: var(--ofi-color);
  }
  .ofi-arrow {
    font-size: 28px; line-height: 1; font-weight: 700;
    transition: color .3s;
    font-family: var(--mono);
  }
  .ofi-body { display: flex; flex-direction: column; gap: 1px; }
  .ofi-signal { font-size: 13px; font-weight: 700; letter-spacing: .08em; transition: color .3s; }
  .ofi-label  { font-size: 7px; color: var(--text-dim); letter-spacing: .1em; }
  .ofi-val    { font-size: 8px; color: var(--text-mid); font-variant-numeric: tabular-nums; }

  .stat-list { flex: 1; overflow-y: auto; overflow-x: hidden; }
  .stat-list::-webkit-scrollbar { width: 3px; }
  .stat-list::-webkit-scrollbar-track { background: transparent; }
  .stat-list::-webkit-scrollbar-thumb { background: var(--brd2); border-radius: 3px; }

  .sr {
    display: flex; justify-content: space-between; align-items: center;
    padding: 3px 10px; border-bottom: 1px solid var(--brd);
  }
  .sr.alert-row { background: rgba(255,61,90,0.06); }
  .sk { font-size: 8px; color: var(--text-dim); letter-spacing: .06em; text-transform: uppercase; white-space: nowrap; }
  .sv { font-size: 10px; font-weight: 600; font-variant-numeric: tabular-nums; text-align: right; }
  .delta { font-size: 8px; font-weight: 400; margin-left: 4px; }
  .stat-divider { height: 1px; background: var(--bg4); margin: 1px 0; }

  .spark-row {
    display: flex; align-items: center; justify-content: space-between;
    padding: 3px 10px; border-bottom: 1px solid var(--brd); gap: 6px;
  }
  .spark { flex: 1; height: 22px; }

  /* ── Super Engine Insights (Col 4) ─────────────────────────────────────── */
  .engine-panel { grid-column: 4; grid-row: 1 / 3; }

  .tab-bar {
    display: flex; gap: 0;
    background: var(--bg2); border-bottom: 1px solid var(--brd);
    flex-shrink: 0; overflow-x: auto;
  }
  .tab-btn {
    flex: 1; padding: 5px 6px;
    font-size: 8px; font-weight: 600; letter-spacing: .1em;
    color: var(--text-dim); background: transparent;
    border: none; border-bottom: 2px solid transparent;
    font-family: var(--mono);
    transition: all .15s;
    cursor: pointer;
    white-space: nowrap;
  }
  .tab-btn:hover { color: var(--text-mid); background: var(--bg3); }
  .tab-btn.active {
    color: var(--accent); border-bottom-color: var(--accent);
    background: var(--bg3);
  }

  .engine-scroll {
    flex: 1; overflow-y: auto; overflow-x: hidden;
  }
  .engine-scroll::-webkit-scrollbar { width: 3px; }
  .engine-scroll::-webkit-scrollbar-track { background: transparent; }
  .engine-scroll::-webkit-scrollbar-thumb { background: var(--brd2); border-radius: 3px; }

  .engine-section {
    border-bottom: 1px solid var(--bg4);
  }
  .es-header {
    font-size: 8px; font-weight: 700; letter-spacing: .14em;
    padding: 5px 10px; color: var(--text-mid);
    background: var(--bg2); border-bottom: 1px solid var(--brd);
    position: sticky; top: 0; z-index: 2;
    display: flex; justify-content: space-between; align-items: center;
  }
  .es-conn {
    font-size: 7px; padding: 1px 5px; border-radius: 3px;
    background: rgba(239, 68, 68, 0.2); color: var(--red);
    font-weight: 600; letter-spacing: .08em;
  }
  .es-conn.conn-on {
    background: rgba(34, 197, 94, 0.2); color: var(--green);
  }

  /* Heat bar (DV01 risk) */
  .heat-bar-wrap {
    width: 80px; height: 5px; background: var(--bg4);
    border-radius: 3px; overflow: hidden;
  }
  .heat-bar {
    height: 100%; border-radius: 3px;
    transition: width .3s, background .3s;
  }

  /* ── Trade tape ──────────────────────────────────────────────────────────── */
  .tape-panel { grid-column: 2; grid-row: 2; }

  .tape-hdr {
    display: grid; grid-template-columns: 40px 80px 50px 1fr 70px;
    padding: 3px 10px; font-size: 8px; color: var(--text-dim);
    letter-spacing: .08em; text-transform: uppercase;
    background: var(--bg2); border-bottom: 1px solid var(--brd);
    flex-shrink: 0;
  }

  .tape-scroll {
    flex: 1; overflow: hidden; display: flex; flex-direction: column;
  }

  .tape-row {
    display: grid; grid-template-columns: 40px 80px 50px 1fr 70px;
    padding: 2px 10px; border-bottom: 1px solid var(--brd);
    align-items: center; font-size: 10px;
    animation: slide-in .15s ease;
  }
  @keyframes slide-in { from { opacity: 0; transform: translateY(-4px); } to { opacity: 1; transform: none; } }
  .tape-row.buy  { background: rgba(0,229,160,.03); }
  .tape-row.sell { background: rgba(255,61,90,.03); }

  .side-badge {
    font-size: 7px; font-weight: 700; padding: 2px 4px; border-radius: 2px;
    text-align: center; letter-spacing: .06em;
  }
  .side-badge.buy  { color: var(--bid); background: var(--bid-dim); }
  .side-badge.sell { color: var(--ask); background: var(--ask-dim); }

  .t-price { font-variant-numeric: tabular-nums; font-weight: 600; font-size: 10px; }
  .t-qty   { color: var(--text-mid); font-variant-numeric: tabular-nums; font-size: 9px; }
  .t-bar-wrap { height: 4px; background: var(--bg4); border-radius: 2px; overflow: hidden; }
  .t-bar { height: 100%; border-radius: 2px; transition: width .2s; }
  .t-bar.buy  { background: var(--bid); }
  .t-bar.sell { background: var(--ask); }
  .t-time { color: var(--text-dim); font-size: 8px; text-align: right; }
  .empty-tape { padding: 16px; color: var(--text-dim); text-align: center; font-size: 10px; }

  /* ── Latency panel ───────────────────────────────────────────────────────── */
  .lat-panel { grid-column: 3; grid-row: 2; }

  .lat-canvas { flex: 1; width: 100%; min-height: 0; }
  .lat-legend { display: flex; gap: 10px; margin-left: auto; }
  .lat-foot {
    display: flex; gap: 12px; padding: 3px 10px;
    border-top: 1px solid var(--brd); background: var(--bg2); flex-shrink: 0;
  }
  .lf { font-size: 8px; }

  /* ── Bottom strip ────────────────────────────────────────────────────────── */
  .bottom-strip {
    grid-column: 1 / 5; grid-row: 3;
    display: flex; background: var(--bg1);
    border-top: 1px solid var(--brd2);
  }
  .bm {
    flex: 1; display: flex; flex-direction: column; justify-content: center;
    padding: 4px 10px; border-right: 1px solid var(--brd);
  }
  .bml { font-size: 7px; color: var(--text-dim); letter-spacing: .1em; text-transform: uppercase; margin-bottom: 1px; }
  .bmv { font-size: 14px; font-weight: 700; letter-spacing: -.01em; line-height: 1; font-variant-numeric: tabular-nums; }
  .bms { font-size: 7px; margin-top: 1px; color: var(--text-dim); }

  /* ── Color utilities ─────────────────────────────────────────────────────── */
  .green  { color: var(--bid);    }
  .red    { color: var(--ask);    }
  .accent { color: var(--accent); }
  .yellow { color: var(--yellow); }
  .purple { color: var(--purple); }
  .teal   { color: var(--teal);   }
  .dim    { color: var(--text-dim); }

  /* Flash animations on mid price change */
  .flash-up {
    background: var(--bid-glow) !important;
    color: var(--bid) !important;
  }
  .flash-dn {
    background: var(--ask-glow) !important;
    color: var(--ask) !important;
  }
</style>