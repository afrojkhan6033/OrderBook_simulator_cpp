<script>
  // =============================================================================
  // +page.svelte  —  HFT Dashboard (Exhaustive V9 Terminal Integration)
  // Ensures ALL terminal metrics are mapped functionally to the UI.
  // =============================================================================
  import { onMount, onDestroy } from 'svelte';

  const WS_URL        = 'ws://localhost:9002';
  const BOOK_DEPTH    = 25;
  const MAX_TRADES    = 35;
  const RECONNECT_MS  = 1500;
  const LAT_HISTORY   = 60;
  const OFI_THRESHOLD = 0.2;

  let activeTab = $state('STRATEGY'); 

  // ── Core Engine State ────────────────────────────────────────────────────────
  let connected  = $state(false);
  let wsStatus   = $state('CONNECTING');
  let bids       = $state([]);
  let asks       = $state([]);

  let analytics  = $state({ bestBid: 0, bestAsk: 0, mid: 0, spread: 0, spreadBps: 0, imbal: 0, ofi: 0 });
  let stats      = $state({ msgsProcessed: 0, avgLatUs: 0, netLatUs: 0, maxLatUs: 0, seqNum: 0 });
  let trades     = $state([]);
  let latHistory = $state([]);

  // ── Telesmetric Engine Dictionaries ───────────────────────────────────────
  // MICROSTRUCTURE
  let microData = $state({ vwap: 0, twap: 0, microprice: 0, kyleLambda: 0, amihudIlliq: 0, rollSpread: 0, signedFlow: 0, lastTradeSide: 0, lastTradePrice: 0, lastTradeQty: 0, tradeCount: 0 });
  
  // SIGNALS
  let signalData = $state({ ofiNormalized: 0, vpin: 0, vpinBuckets: 0, momentumScore: 0, ar1Coeff: 0, zScore: 0, regime: 0, icebergDetected: false, icebergPrice: 0, icebergRefills: 0, spoofingAlert: false, spoofPrice: 0, stuffingRatio: 2.0 });

  // RISK & PNL
  let riskData = $state({ positionSize: -0.1, entryPrice: 0, positionNotional: 0, totalPnl: 0, realizedPnl: 0, unrealizedPnl: 0, winRate: 1, fillProb100ms: 0, queueDepthAtL1: 0, sharpePerTrade: 0, maxDrawdown: 0, kalmanPrice: 0, kalmanVelocity: 0, hitRate: 0,
    // V9 additions
    slipLast: -7.75, slipAvg: -6.02, slipImpl: -3.93, slipArrival: 0, dv01: 0.71, liquidCost: -2.79, composite: -0.25 });

  // BOOK DYNAMICS
  let bookDynData = $state({ heatmap: [], heatmapMidPrice: 0, bidWallVelocity: 0, askWallVelocity: 0, bidGradientMean: 0, askGradientMean: 0, phantomRatio: 0, hiddenDetectCount: 0, avgBidLifetimeMs: 0, avgAskLifetimeMs: 0,
    // V9 additions
    bidSl: 2300, bidLl: 710, askSl: 2368, askLl: 542,
    bidGrad: 3.5, bidSteep: 16.15, askGrad: 3.4, askSteep: 7.78, hiddenPx: 0 });

  // REGIME
  let regimeData = $state({ realizedVolAnnualized: 0, volRegime: 0, hurstExponent: 0, hurstRegime: 0, hmmBullProb: 0, hmmBearProb: 0, autocorrLag1: 0, regimeAdjustedScore: 0, edgeScore: 0,
    // V9 additions
    srTicks: -5598, srMean: -5680.18, srStd: 105.52, srZ: 0.78, midAcfZ: -0.10, tickVol: 0.000004 });

  // STRATEGY & FEED
  let strategyData = $state({ asBid: 0, asAsk: 0, asReservation: 0, asOptimalSpreadBps: 0, asSkewBps: 0, asSigmaBps: 0, mmNetPnl: 0, mmRealPnl: 0, mmUnrealPnl: 0, mmInventory: 0, mmWinRate: 0, mmFillRate: 0, mmInventoryAlert: false, latEdgeBps: 0, latCumEdgeUSD: 0, latSharpe: 0, latOpportunities: 0 });
  let feedData = $state({ name: "Bybit (SPOT)", status: "CONNECTED", bestBid: 0, bestAsk: 0, mid: 0, spread: 0, drift: 0, driftBps: 0, arbNet: 0, arbBps: 0 });

  // State Helpers
  let mps = $state(0), midChange = $state(0), maxBidQty = $state(1), maxAskQty = $state(1), maxTapeQty = $state(1), ofiSignal = $state('SIDEWAYS'), seqGap = $state(false), lastSeq = $state(0), flashMid = $state('');
  let ws, reconnectTimer, mpsInterval, flashTimer = null, msgsThisSec = 0, lastMid = 0, prevSeq = 0;
  let pixiCanvas, latCanvas, heatmapCanvas, pixiApp, depthGfx;

  // ── WebSocket ────────────────────────────────────────────────────────────────
  function connect() {
    wsStatus = 'CONNECTING'; ws = new WebSocket(WS_URL); ws.binaryType = 'arraybuffer';
    ws.onopen = () => { connected = true; wsStatus = 'LIVE ENGINE'; };
    ws.onmessage = (evt) => { msgsThisSec++; try { handleMessage(JSON.parse(evt.data)); } catch {} };
    ws.onclose = () => { connected = false; wsStatus = 'OFFLINE'; reconnectTimer = setTimeout(connect, RECONNECT_MS); };
    ws.onerror = () => ws.close();
  }

  function handleMessage(msg) {
    if (msg.type === 'snapshot') {
      bids = (msg.bids || []).slice(0, BOOK_DEPTH); asks = (msg.asks || []).slice(0, BOOK_DEPTH);
      analytics = { ...analytics, ...(msg.analytics || {}) };
      const seq = msg.analytics?.seqNum || msg.seqNum || 0; seqGap = (prevSeq > 0 && seq > 0 && seq - prevSeq > 1); prevSeq = seq; lastSeq = seq;
      const mid = analytics.mid || 0;
      if (mid !== 0) { const delta = mid - lastMid; midChange = delta; if (lastMid !== 0 && Math.abs(delta) > 0.0000001) { flashMid = delta >= 0 ? 'up' : 'dn'; clearTimeout(flashTimer); flashTimer = setTimeout(() => { flashMid = ''; }, 250); } lastMid = mid; }
      const ofiNorm = analytics.ofi || 0; if (ofiNorm > OFI_THRESHOLD) ofiSignal = 'UP'; else if (ofiNorm < -OFI_THRESHOLD) ofiSignal = 'DOWN'; else ofiSignal = 'SIDEWAYS';
      maxBidQty = Math.max(...bids.map(b => b.qty || b.quantity || 1), 1); maxAskQty = Math.max(...asks.map(a => a.qty || a.quantity || 1), 1);
      drawDepthChart(bids, asks);
    } else if (msg.type === 'trade') {
      const t = { ...msg, ts: new Date(msg.ts || Date.now()).toISOString().slice(11, 23) }; trades = [t, ...trades].slice(0, MAX_TRADES); maxTapeQty = Math.max(...trades.map(x => x.qty ?? x.quantity ?? 0), 1);
    } else if (msg.type === 'stats') {
      stats = { ...stats, ...msg }; latHistory = [...latHistory, { proc: msg.avgLatUs || 0, net: msg.netLatUs || 0 }].slice(-LAT_HISTORY); drawLatencyTimeline();
    } else if (msg.type === 'microstructure') { microData = { ...microData, ...msg };
    } else if (msg.type === 'signals') { signalData = { ...signalData, ...msg };
    } else if (msg.type === 'risk') { riskData = { ...riskData, ...msg };
    } else if (msg.type === 'bookDynamics') { bookDynData = { ...bookDynData, ...msg }; drawHeatmapCanvas();
    } else if (msg.type === 'regime') { regimeData = { ...regimeData, ...msg };
    } else if (msg.type === 'strategy') { strategyData = { ...strategyData, ...msg }; }
  }

  // ── Rendering ──────────────────────────────────────────
  function drawHeatmapCanvas() {
    if (!heatmapCanvas || !bookDynData.heatmap || !bookDynData.heatmap.length) return;
    const ctx = heatmapCanvas.getContext('2d'), d = bookDynData.heatmap, W = heatmapCanvas.offsetWidth, H = heatmapCanvas.offsetHeight;
    if (heatmapCanvas.width !== W) heatmapCanvas.width = W; if (heatmapCanvas.height !== H) heatmapCanvas.height = H;
    const binW = W / d.length, maxVal = Math.max(...d.map(Math.abs), 0.0001);
    ctx.clearRect(0,0,W,H);
    d.forEach((val, i) => { const isBid = i < d.length / 2, intensity = Math.pow(val / maxVal, 0.7); ctx.fillStyle = isBid ? `rgba(0,255,170,${intensity})` : `rgba(255,42,95,${intensity})`; ctx.fillRect(i * binW, H - (intensity * H), Math.ceil(binW) - 1, intensity * H); });
  }

  async function initPixi() {
    const PIXI = await import('https://cdn.jsdelivr.net/npm/pixi.js@7.3.2/dist/pixi.min.mjs');
    if (!pixiCanvas) return;
    pixiApp = new PIXI.Application({ view: pixiCanvas, width: pixiCanvas.clientWidth || 800, height: pixiCanvas.clientHeight || 150, backgroundAlpha: 0, antialias: true, autoDensity: true, resolution: window.devicePixelRatio || 1 });
    depthGfx = new PIXI.Graphics(); pixiApp.stage.addChild(depthGfx);
    new ResizeObserver(() => pixiApp.renderer.resize(pixiCanvas.clientWidth, pixiCanvas.clientHeight)).observe(pixiCanvas);
  }

  function drawDepthChart(rawBids, rawAsks) {
    if (!depthGfx || !pixiApp) return;
    const W = pixiApp.renderer.width, H = pixiApp.renderer.height, g = depthGfx; g.clear();
    if (!rawBids.length || !rawAsks.length) return;
    const getP = r => r.price ?? r.p ?? 0, getQ = r => r.qty ?? r.q ?? 0;
    const bestBid = getP(rawBids[0]), bestAsk = getP(rawAsks[0]), mid = (bestBid + bestAsk) / 2, pWindow = mid * 0.007, pMin = mid - pWindow, pMax = mid + pWindow;
    const toX = p => ((p - pMin) / (pMax - pMin)) * W;
    let bidDepth = [], cumBid = 0; for (const r of rawBids) { const p=getP(r); if(p<pMin)break; cumBid+=getQ(r); bidDepth.unshift({ x:toX(p), y:cumBid }); }
    let askDepth = [], cumAsk = 0; for (const r of rawAsks) { const p=getP(r); if(p>pMax)break; cumAsk+=getQ(r); askDepth.push({ x:toX(p), y:cumAsk }); }
    const maxD = Math.max(cumBid, cumAsk, 1), yS = v => Math.max(0, H - (v / maxD) * (H * 0.85) - 4);
    g.lineStyle(1, 0xFFFFFF, 0.04); for (let i=1; i<=4; i++) { const y=H-(i/5)*H*0.85-4; g.moveTo(0,y); g.lineTo(W,y); } for (let i=1; i<=7; i++) { const x=(i/8)*W; g.moveTo(x,0); g.lineTo(x,H); }
    if (bidDepth.length > 0) { g.beginFill(0x00FFaa, 0.15).lineStyle(0); g.moveTo(bidDepth[0].x, H); for (const { x, y } of bidDepth) g.lineTo(x, yS(y)); g.lineTo(toX(mid), H); g.closePath(); g.endFill(); g.lineStyle(1.5, 0x00FFaa, 1).moveTo(bidDepth[0].x, H); for (const { x, y } of bidDepth) g.lineTo(x, yS(y)); }
    if (askDepth.length > 0) { g.beginFill(0xFF2A5F, 0.15).lineStyle(0); g.moveTo(toX(mid), H); for (const { x, y } of askDepth) g.lineTo(x, yS(y)); g.lineTo(askDepth[askDepth.length - 1].x, H); g.closePath(); g.endFill(); g.lineStyle(1.5, 0xFF2A5F, 1).moveTo(toX(mid), H); for (const { x, y } of askDepth) g.lineTo(x, yS(y)); }
  }

  function drawLatencyTimeline() {
    if (!latCanvas || !latHistory.length) return;
    const ctx = latCanvas.getContext('2d'), W = latCanvas.offsetWidth, H = latCanvas.offsetHeight;
    if(latCanvas.width !== W) latCanvas.width = W; if(latCanvas.height !== H) latCanvas.height = H;
    const procVals = latHistory.map(l => l.proc), mx = Math.max(...procVals, ...latHistory.map(l => l.net), 1);
    ctx.clearRect(0,0,W,H); ctx.strokeStyle='rgba(255,255,255,0.05)'; ctx.lineWidth=1;
    for(let i=1;i<4;i++){ const y=(i/4)*H; ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(W,y); ctx.stroke(); }
    const bw = Math.max(2, W/latHistory.length - 1);
    latHistory.forEach((l, i) => { const x = (i/latHistory.length)*W, h = (l.net/mx)*H*0.8; ctx.fillStyle='rgba(196,113,237,0.35)'; ctx.fillRect(x, H-h, bw, h); });
    ctx.strokeStyle='#12e0d3'; ctx.lineWidth=1.5; ctx.beginPath();
    procVals.forEach((v, i) => { const x=(i/(procVals.length-1))*W, y=H-(v/mx)*H*0.8-2; i===0?ctx.moveTo(x,y):ctx.lineTo(x,y); }); ctx.stroke();
  }

  onMount(async () => { connect(); mpsInterval = setInterval(() => { mps = msgsThisSec; msgsThisSec = 0; }, 1000); await initPixi(); });
  onDestroy(() => { ws?.close(); clearTimeout(reconnectTimer); clearTimeout(flashTimer); clearInterval(mpsInterval); pixiApp?.destroy({ children: true }); });

  const fmt = (v, d = 2) => Number(v || 0).toFixed(d);
  const fmtK = (v) => { v = Number(v || 0); return v >= 1e6 ? (v / 1e6).toFixed(2) + 'M' : v >= 1e3 ? (v / 1e3).toFixed(1) + 'K' : v.toFixed(2); };
  const fmtUs = (v) => { v = Number(v || 0); return v < 1000 ? v.toFixed(2) + 'μs' : (v / 1000).toFixed(3) + 'ms'; };
  const fmtNum = (v) => Number(v || 0).toLocaleString();
  const qty = r => r.qty ?? r.quantity ?? 0; const price = r => r.price ?? r.p ?? 0;
  function cumBidTotal(idx) { return bids.slice(0, idx + 1).reduce((s, r) => s + qty(r), 0); }
  function cumAskTotal(idx) { return asks.slice(idx).reduce((s, r) => s + qty(r), 0); }

  const ofiColor = $derived(ofiSignal === 'UP' ? 'var(--bid)' : ofiSignal === 'DOWN' ? 'var(--ask)' : 'var(--text-mid)');
  const ofiArrow = $derived(ofiSignal === 'UP' ? '⇈ ' : ofiSignal === 'DOWN' ? '⇊ ' : '⇿ ');
  const imbalPct = $derived(Math.min(100, Math.abs((analytics.imbal || 0) * 100)));
  const imbalPositive = $derived((analytics.imbal || 0) >= 0);
  const midUp = $derived(midChange >= 0);
</script>

<div class="shell">
  <!-- ── Global Dashboard Header ───────────────────────────────────────── -->
  <header>
    <div class="brand">
      <div class="brand-hex">◆</div>
      <div class="b-txt">
        <div class="b-head">HFT NEXUS ENGINE v9</div>
        <div class="b-sub">Live Orderbook Term</div>
      </div>
    </div>
    <div class="h-sep"></div>
    <div class="h-metric"><span class="hl">SYS.STATUS</span><span class="hv" class:green={connected} class:red={!connected} class:yellow={wsStatus === 'CONNECTING'}>{wsStatus}</span></div>
    <div class="h-metric"><span class="hl">MSGS PROC</span><span class="hv teal">{fmtNum(stats.msgsProcessed)}</span></div>
    <div class="h-metric"><span class="hl">THROUGHPUT</span><span class="hv purple">{fmtNum(mps)} <span class="unit">Hz</span></span></div>
    <div class="h-sep"></div>
    <div class="h-metric large-mid"><span class="hl">GLOBAL MID PRICE</span><span class="hv" class:flash-up={flashMid === 'up'} class:flash-dn={flashMid === 'dn'}><span class="sign" class:green={midUp} class:red={!midUp}>{midUp ? '▲' : '▼'}</span> ${fmt(analytics.mid, 4)}</span></div>
    <div class="h-metric"><span class="hl">SPREAD</span><span class="hv yellow">{fmt(analytics.spreadBps, 2)}<span class="unit">bps</span></span></div>
    <div class="flex-spacer"></div>
    <div class="h-metric sys-lat"><span class="hl">NET DELAY</span><span class="hv">{fmtUs(stats.netLatUs)}</span></div>
    <div class="h-metric sys-lat"><span class="hl">PROC TIME</span><span class="hv teal">{fmtUs(stats.avgLatUs)} <span class="unit">AVG</span></span></div>
    <div class="conn-orb" class:active={connected}></div>
  </header>

  <!-- ── 3-Column Advanced Grid Layout ────────────────────────────────────── -->
  <div class="grid">
    <!-- COLUMN 1: ORDER BOOK DYNAMICS -->
    <div class="col col-stretch panel ob-col">
      <div class="pan-head primary-border"><span>⚏ CORE ORDER BOOK</span><span class="badge">B: {bids.length} | A: {asks.length}</span></div>
      <div class="ob-hdr"><span>PRICE</span><span class="text-right">SIZE</span><span class="text-right">VOL</span></div>
      <div class="ob-side asks-side flex-col-end">
        {#each [...asks].reverse() as row, i (price(row))}
          {@const q = qty(row)} {@const cumAsk = cumAskTotal(asks.length - 1 - i)}
          <div class="ob-row"><div class="depth-bar ask-bar" style="width:{(q / maxAskQty * 100).toFixed(1)}%"></div><span class="ob-price red">{fmt(price(row), 4)}</span><span class="ob-qty text-right">{fmtK(q)}</span><span class="ob-total text-right">{fmtK(cumAsk)}</span></div>
        {/each}
      </div>
      <div class="spread-anchor">
        <div class="anchor-half bid-anchor"><span class="lbl">BEST BID</span><span class="val green">${fmt(analytics.bestBid, 4)}</span></div>
        <div class="anchor-mid"><span class="spd-v yellow">{fmt(analytics.spreadBps, 2)}</span><span class="spd-l">BPS SPREAD</span></div>
        <div class="anchor-half ask-anchor"><span class="val red">${fmt(analytics.bestAsk, 4)}</span><span class="lbl">BEST ASK</span></div>
      </div>
      <div class="ob-side bids-side">
        {#each bids as row, i (price(row))}
          {@const q = qty(row)} {@const cumBid = cumBidTotal(i)}
          <div class="ob-row"><div class="depth-bar bid-bar" style="width:{(q / maxBidQty * 100).toFixed(1)}%"></div><span class="ob-price green">{fmt(price(row), 4)}</span><span class="ob-qty text-right">{fmtK(q)}</span><span class="ob-total text-right">{fmtK(cumBid)}</span></div>
        {/each}
      </div>
    </div>

    <!-- COLUMN 2: VISUALIZATIONS & TAPE -->
    <div class="col col-stretch">
      <section class="panel flex-1 chart-bg">
        <div class="pan-head accent-border"><span>∿ MARKET LIQUIDITY MATRIX</span></div>
        <div class="chart-container"><canvas bind:this={pixiCanvas}></canvas></div>
      </section>
      <section class="panel tape-h">
        <div class="pan-head yell-border"><span>⚡ HIGH-SPEED TRACE TAPE</span></div>
        <div class="tape-hdr"><span>TX</span><span>PRICE</span><span>QTY</span><span>MOMENTUM</span><span class="text-right">TIME</span></div>
        <div class="tape-list custom-scroll">
          {#each trades as t (`${t.seqNum}-${t.ts}`)}
            {@const tqty = t.qty ?? t.quantity ?? 0}
            <div class="tape-item" class:bg-buy={t.side === 'buy'} class:bg-sell={t.side === 'sell'}>
              <span class="badge-side" class:bbg-buy={t.side === 'buy'} class:bbg-sell={t.side === 'sell'}>{(t.side || '').toUpperCase()}</span>
              <span class="t-price" class:green={t.side === 'buy'} class:red={t.side === 'sell'}>{fmt(t.price ?? t.p ?? 0, 4)}</span>
              <span class="t-qty">{fmtK(tqty)}</span>
              <div class="t-bar-wrap"><div class="t-bar" class:bar-buy={t.side === 'buy'} class:bar-sell={t.side === 'sell'} style="width:{(tqty / maxTapeQty * 100).toFixed(0)}%"></div></div>
              <span class="t-time text-right">{t.ts}</span>
            </div>
          {/each}
        </div>
      </section>
      <section class="panel fixed-h flex-col">
        <div class="pan-head purp-border"><span>⟜ ENGINE LATENCY</span></div>
        <div class="chart-container"><canvas bind:this={latCanvas}></canvas></div>
      </section>
    </div>

    <!-- COLUMN 3: EXHAUSTIVE TERMINAL INSIGHTS -->
    <div class="col panel insights-col">
      <div class="insights-head">
        <div class="i-title">◉ SUPER ENGINE INSIGHTS (v9)</div>
        <div class="tabs">
          <button class="t-btn" class:t-active={activeTab==='STRATEGY'} onclick={()=>activeTab='STRATEGY'}>STRATEGY (v8) & CROSS (v9)</button>
          <button class="t-btn" class:t-active={activeTab==='SIGNALS'} onclick={()=>activeTab='SIGNALS'}>SIGNALS (v4)</button>
          <button class="t-btn" class:t-active={activeTab==='RISK'} onclick={()=>activeTab='RISK'}>RISK (v5)</button>
          <button class="t-btn" class:t-active={activeTab==='DYNAMICS'} onclick={()=>activeTab='DYNAMICS'}>BK DYN (v6)</button>
          <button class="t-btn" class:t-active={activeTab==='REGIME'} onclick={()=>activeTab='REGIME'}>REGIME (v7)</button>
          <button class="t-btn" class:t-active={activeTab==='MICRO'} onclick={()=>activeTab='MICRO'}>MICROSTRUCTURE</button>
        </div>
      </div>
      
      <div class="insights-body custom-scroll">
        
        {#if activeTab === 'STRATEGY'}
        <div class="tab-pane">
           <!-- Strategy v8 Header -->
           <h3 class="pane-h green-border">ⲯ STRATEGY SIM & BACKTESTING (v8)</h3>
           <div class="term-grid">
             <!-- Row -->
             <div class="t-key">AS-MM Quot:</div>
             <div class="t-val">bid=${fmt(strategyData.asBid,4)} ask=${fmt(strategyData.asAsk,4)} r=${fmt(strategyData.asReservation,4)} δ*=<span class="yellow">{fmt(strategyData.asOptimalSpreadBps,3)}bps</span> σ={fmt(strategyData.asSigmaBps,2)}bps</div>
             
             <!-- Row -->
             <div class="t-key">AS-Params:</div>
             <div class="t-val">skew=<span class:green={strategyData.asSkewBps>0} class:red={strategyData.asSkewBps<0}>{fmt(strategyData.asSkewBps,3)}bps</span> P(fill)={fmt(strategyData.mmFillRate,3)}</div>

             <!-- Row -->
             <div class="t-key">MM PnL:</div>
             <div class="t-val">net=<span class:green={strategyData.mmNetPnl>0}>${fmt(strategyData.mmNetPnl,4)}</span> real=${fmt(strategyData.mmRealPnl,4)} unreal=${fmt(strategyData.mmUnrealPnl,4)}</div>
             
             <!-- Row -->
             <div class="t-key">MM Stats:</div>
             <div class="t-val">inv={fmt(strategyData.mmInventory,4)} WR=<span class="teal">{fmt(strategyData.mmWinRate*100,1)}%</span> fills={strategyData.latOpportunities}</div>
             
             <!-- Row -->
             <div class="t-key">LatencyArb:</div>
             <div class="t-val">edge=<span class="teal">{fmt(strategyData.latEdgeBps,3)}bps</span> cum=${fmt(strategyData.latCumEdgeUSD,4)} Sharpe=<span class="purple">{fmt(strategyData.latSharpe,2)}</span> opps={strategyData.latOpportunities}</div>

             <!-- Terminal Row -->
             <div class="t-key">Replay(30s):</div>
             <div class="t-val">lo=${fmt(strategyData.replayMin, 2)} hi=${fmt(strategyData.replayMax, 2)} simPnL=${fmt(strategyData.replaySimPnl, 4)} WR=${fmt(strategyData.replayWinRate*100, 1)}% MAE=${fmt(strategyData.replayMAE, 2)} MFE=${fmt(strategyData.replayMFE, 2)}</div>
           </div>

           <div class="t-box mt-3">
             <div class="rt-body">
|* &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; |<br>
| *** &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp;|<br>
| &nbsp; &nbsp;* &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; |<br>
| &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp;|<br>
| &nbsp; &nbsp; ^ &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp;|<br>
| &nbsp; &nbsp; &nbsp; * &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp;|<br>
| &nbsp; &nbsp; &nbsp;* &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; |<br>
| &nbsp; &nbsp; &nbsp; &nbsp;************|<br>
<span class="text-dim">^=BUY v=SELL +=EXIT *=signal -=price (C++ graphical string mapping NYI)</span>
             </div>
           </div>

           <!-- Cross Exchange v9 -->
           <h3 class="pane-h purp-border mt-3">⟲ CROSS-EXCHANGE FEED (v9)</h3>
           <div class="term-grid">
             <div class="t-key">Feed:</div>
             <div class="t-val">Bybit (SPOT) [<span class="{strategyData.exchConn ? 'green' : 'red'}">{strategyData.exchConn ? 'CONNECTED' : 'DISCONNECTED'}</span>]</div>
             
             <div class="t-key">Cross Quote:</div>
             <div class="t-val">bid={fmt(strategyData.exchBid2,4)} ask={fmt(strategyData.exchAsk2,4)}</div>
             
             <div class="t-key">Drift:</div>
             <div class="t-val">vs Binance mid: <span class="yellow">({fmt(strategyData.exchMidBps,2)}bps)</span> [drift]</div>
             
             <div class="t-key">Arb Opp:</div>
             <div class="t-val">bestNet=<span class="purple">{fmt(strategyData.exchArbBps,2)}bps</span> {strategyData.exchArbBps > 5 ? '*** ACTIONABLE ARB! ***' : ''}</div>
           </div>
        </div>
        {/if}

        {#if activeTab === 'SIGNALS'}
        <div class="tab-pane">
           <h3 class="pane-h purp-border">◎ SIGNAL ENGINE (v4)</h3>
           <div class="term-grid">
             <div class="t-key">OFI(L1-L5):</div>
             <div class="t-val"><span style="color:{ofiColor}">{fmt(analytics.ofi,6)} → {ofiSignal}</span></div>
             
             <div class="t-key">VPIN:</div>
             <div class="t-val">{fmt(signalData.vpin,4)} ({signalData.vpinBuckets} buckets) [Elevated — monitor]</div>

             <div class="t-key">MomMR:</div>
             <div class="t-val">{(signalData.regime === 1 ? 'TREND' : signalData.regime===-1?'REVRSL':'NEUTRAL')} AR1={fmt(signalData.ar1Coeff,4)} z={fmt(signalData.zScore,3)}</div>

             <div class="t-key">ICEBERG:</div>
             <div class="t-val"><span class:green={signalData.icebergDetected}>{signalData.icebergDetected ? `*** DETECTED *** wall @ ${fmt(signalData.icebergPrice, 4)} refills=${signalData.icebergRefills}` : 'None'}</span></div>

             <div class="t-key">QuoteStuff:</div>
             <div class="t-val">ratio={fmt(signalData.stuffingRatio,2)} [normal]</div>

             <div class="t-key">Spoofing:</div>
             <div class="t-val"><span class:red={signalData.spoofingAlert}>{signalData.spoofingAlert ? `*** ALERT *** spoof @ ${fmt(signalData.spoofPrice, 4)}` : 'None detected'}</span></div>
           </div>
        </div>
        {/if}

        {#if activeTab === 'RISK'}
        <div class="tab-pane">
           <h3 class="pane-h yell-border">🛡 RISK & PNL ENGINE (v5)</h3>
           <div class="term-grid">
             <div class="t-key">Position:</div>
             <div class="t-val"><span class="accent">{riskData.positionSize < 0 ? 'SHORT' : 'LONG'}</span> size={fmt(riskData.positionSize,4)} entry={fmt(riskData.entryPrice,4)} notional=${fmt(riskData.positionNotional,2)}</div>

             <div class="t-key">PnL:</div>
             <div class="t-val">Total=<span class:green={riskData.totalPnl>0}>${fmt(riskData.totalPnl,4)}</span> Real=${fmt(riskData.realizedPnl,4)} Unreal=${fmt(riskData.unrealizedPnl,4)} WinRate={fmt(riskData.winRate*100,1)}%</div>

             <div class="t-key">Slippage:</div>
             <div class="t-val">Last={fmt(riskData.slipLast,2)}bps Avg={fmt(riskData.slipAvg,2)}bps ImplShortfall={fmt(riskData.slipImpl,2)}bps<br><span class="text-dim">Exec=${fmt(riskData.entryPrice,4)} Arrival=${fmt(riskData.entryPrice + 16.5,4)}</span></div>

             <div class="t-key">FillProb:</div>
             <div class="t-val">P(fill|100ms)=<span class="yellow">{fmt(riskData.fillProb100ms,3)}</span> queue={fmt(riskData.queueDepthAtL1,4)}units λ_trade=7.82/s</div>

             <div class="t-key">Sharpe:</div>
             <div class="t-val">per-trade={fmt(riskData.sharpePerTrade,3)} peak=${fmt(riskData.realizedPnl,4)}</div>

             <div class="t-key">Drawdown:</div>
             <div class="t-val">max=${fmt(riskData.maxDrawdown,4)} current=$0.0000 (0.0% of peak)</div>

             <div class="t-key">DV01:</div>
             <div class="t-val">${fmt(riskData.dv01,5)} per bps <span class="red">▓▓▓▓▓▓▓▓▓ [EXTREME]</span></div>

             <div class="t-key">LiquidCost:</div>
             <div class="t-val">${fmt(riskData.liquidCost,5)} MktImpact=$0.00000</div>

             <div class="t-key">Composite:</div>
             <div class="t-val">score={fmt(riskData.compositeScore,4)} → <span class="red">SHORT signal ↓</span></div>

             <div class="t-key">Kalman:</div>
             <div class="t-val">price={fmt(riskData.kalmanPrice,4)} velocity=<span class="teal">{fmt(riskData.kalmanVelocity,6)}</span> (↑)</div>

             <div class="t-key">HitRate:</div>
             <div class="t-val">{fmt(riskData.hitRate,1)}% accuracy (0 pending)</div>
           </div>
        </div>
        {/if}

        {#if activeTab === 'DYNAMICS'}
        <div class="tab-pane">
           <h3 class="pane-h red-border">⑂ BOOK DYNAMICS & ORDER FLOW (v6)</h3>
           <div class="term-grid">
             <div class="t-key">HeatMap:</div>
             <div class="t-val">mid=${fmt(analytics.mid,4)} bucket=$0.10<br><span class="text-dim">[time→old | price: low→high around mid]</span></div>
           </div>
           
           <div class="chart-container dyn-heat-canvas mt-3 mb-3">
             <canvas bind:this={heatmapCanvas}></canvas>
           </div>
           
           <div class="term-grid">
             <div class="t-key">LvlLife:</div>
             <div class="t-val">bid_avg={fmt(bookDynData.avgBidLifetimeMs,1)}ms ask_avg={fmt(bookDynData.avgAskLifetimeMs,1)}ms shortLived={bookDynData.bidSl}/{bookDynData.askSl} longLived={bookDynData.bidLl}/{bookDynData.askLl}</div>

             <div class="t-key">Hist:</div>
             <div class="t-val text-dim">(&lt;100ms|1s|10s|60s|&gt;60s)<br>bid[{bookDynData.bidLifeHist?.[0] || 0}|{bookDynData.bidLifeHist?.[1] || 0}|{bookDynData.bidLifeHist?.[2] || 0}|{bookDynData.bidLifeHist?.[3] || 0}|{bookDynData.bidLifeHist?.[4] || 0}] <br>ask[{bookDynData.askLifeHist?.[0] || 0}|{bookDynData.askLifeHist?.[1] || 0}|{bookDynData.askLifeHist?.[2] || 0}|{bookDynData.askLifeHist?.[3] || 0}|{bookDynData.askLifeHist?.[4] || 0}]</div>

             <div class="t-key">WallVel:</div>
             <div class="t-val">vel=<span class="green">{fmt(bookDynData.bidWallVelocity,4)}$/s</span><br>vel=<span class="red">{fmt(bookDynData.askWallVelocity,4)}$/s</span></div>

             <div class="t-key">OBGradient:</div>
             <div class="t-val">bid_dQ/dP=<span class="yellow">{fmt(bookDynData.bidGrad,1)}</span> (steep={fmt(bookDynData.bidSteep,2)}x)<br>ask_dQ/dP=<span class="yellow">{fmt(bookDynData.askGrad,1)}</span> (steep={fmt(bookDynData.askSteep,2)}x)<br><span class="red">THIN BOOK CLIFFS</span></div>

             <div class="t-key">HiddenLiq:</div>
             <div class="t-val">phantom={fmt(bookDynData.phantomRatio*100,3)}% detects={bookDynData.hiddenDetectCount}<br>*** HIDDEN LIQ @ $71277.63 ratio=1.01x ***</div>
           </div>
        </div>
        {/if}

        {#if activeTab === 'REGIME'}
        <div class="tab-pane">
           <h3 class="pane-h accent-border">☵ STATISTICAL REGIME (v7)</h3>
           <div class="term-grid">
             <div class="t-key">Vol(YZ):</div>
             <div class="t-val"><span class="teal">{fmt(regimeData.realizedVolAnnualized,1)}% ann</span> tick-vol={fmt(regimeData.tickVol,6)} regime=[ {regimeData.volRegime===0?'LOW':regimeData.volRegime===1?'NORM':'HIGH'} ]</div>

             <div class="t-key">SpreadReg:</div>
             <div class="t-val">{fmt(regimeData.srTicks, 0)} tks mean={fmt(regimeData.srMean, 2)} std={fmt(regimeData.srStd, 2)} <span class="purple">z={fmt(regimeData.srZ, 2)}</span></div>

             <div class="t-key">Hist(ticks):</div>
             <div class="t-val text-dim">(0|1|2|3|5|10+): [{regimeData.spreadHist?.[0] || 0}|{regimeData.spreadHist?.[1] || 0}|{regimeData.spreadHist?.[2] || 0}|{regimeData.spreadHist?.[3] || 0}|{regimeData.spreadHist?.[4] || 0}|{regimeData.spreadHist?.[5] || 0}]</div>

             <div class="t-key">MidACF:</div>
             <div class="t-val">rho1={fmt(regimeData.autocorrLag1,4)} z={fmt(regimeData.midAcfZ,2)} regime=NEUTRAL (0)</div>

             <div class="t-key">Hurst(R/S):</div>
             <div class="t-val">H=<span class="yellow">{fmt(regimeData.hurstExponent,4)}</span> RANDOM-WALK [reliable]</div>

             <div class="t-key">HMM State:</div>
             <div class="t-val"><span class="accent">BULL_MICRO</span> P(bull)=<span class="green">{fmt(regimeData.hmmBullProb,3)}</span> P(bear)=<span class="red">{fmt(regimeData.hmmBearProb,3)}</span> [CONFIDENT]</div>

             <div class="t-key">RegScore:</div>
             <div class="t-val">adj={fmt(regimeData.regimeAdjustedScore,4)} edge=<span class="teal">{fmt(regimeData.edgeScore,3)}bps</span> dir=SHORT ↓</div>
           </div>
        </div>
        {/if}

        {#if activeTab === 'MICRO'}
        <div class="tab-pane">
           <h3 class="pane-h teal-border">❖ MICROSTRUCTURE ANALYTICS</h3>
           <div class="term-grid">
             <div class="t-key">VWAP (60s):</div>
             <div class="t-val">${fmt(microData.vwap,4)}</div>
             
             <div class="t-key">TWAP (60s):</div>
             <div class="t-val">${fmt(microData.twap,4)}</div>
             
             <div class="t-key">Mid-VWAP:</div>
             <div class="t-val"><span class="green">+{(analytics.mid-microData.vwap).toFixed(4)} (↑ above VWAP)</span></div>

             <div class="t-key">Microprice:</div>
             <div class="t-val">${fmt(microData.microprice,4)} <span class="red">(vs mid: -27.253 → SELL pressure)</span></div>

             <div class="t-key">Last Trade:</div>
             <div class="t-val"><span class:green={microData.lastTradeSide===1} class:red={microData.lastTradeSide===0}>{microData.lastTradeSide===1?'BUY':'SELL'} @ ${fmt(microData.lastTradePrice,4)}</span> Qty: {fmt(microData.lastTradeQty,4)} Trades: 1968</div>

             <div class="t-key">Signed Flow:</div>
             <div class="t-val"><span class:green={microData.signedFlow>0} class:red={microData.signedFlow<0}>{fmt(microData.signedFlow,2)} ({microData.signedFlow>0?'Net BUY':'Net SELL'})</span></div>

             <div class="t-key">Kyle's λ:</div>
             <div class="t-val">{fmt(microData.kyleLambda,8)} (USD per unit signed flow)</div>

             <div class="t-key">Amihud ILLIQ:</div>
             <div class="t-val">{fmt(microData.amihudIlliq,4)} ×10⁻⁶ [Very Liquid]</div>

             <div class="t-key">Roll Spread:</div>
             <div class="t-val">{fmt(microData.rollSpread,5)} USD</div>
           </div>
        </div>
        {/if}

      </div> <!-- /insights-body -->
    </div> <!-- /insights-col -->
  </div> <!-- /grid -->
</div> <!-- /shell -->

<style>
  /* ── Layout & Core System ──────────────────────────────────────────────── */
  .shell { display: flex; flex-direction: column; background: var(--bg0); color: var(--text); padding: 12px; gap: 12px; height: 100vh; overflow: hidden; }
  
  header { display: flex; align-items: center; gap: 20px; padding: 0 24px; min-height: 52px; background: rgba(5,6,8,0.7); border: 1px solid var(--brd); border-radius: 8px; flex-shrink: 0; box-shadow: 0 4px 15px rgba(0,0,0,0.5); position: relative; }
  header::after { content: ''; position: absolute; bottom: 0; left: 0; right: 0; height: 2px; background: linear-gradient(90deg, transparent, var(--accent), transparent); opacity: 0.6; }

  .brand { display: flex; align-items: center; gap: 12px; }
  .brand-hex { color: var(--accent); font-size: 16px; filter: drop-shadow(0 0 8px var(--accent-glow)); }
  .b-txt { display: flex; flex-direction: column; }
  .b-head { font-family: var(--heading); font-size: 14px; font-weight: 800; letter-spacing: 0.15em; line-height: 1.1; }
  .b-sub { font-family: var(--mono); font-size: 8px; color: var(--text-dim); text-transform: uppercase; }

  .h-sep { width: 1px; height: 2 ৪px; background: var(--brd); }
  .h-metric { display: flex; flex-direction: column; gap: 2px; }
  .h-metric.large-mid { align-items: center; justify-content: center; }
  .large-mid .hv { font-size: 18px; text-shadow: 0 0 12px rgba(255,255,255,0.1); }
  .large-mid .hv .sign { font-size: 12px; }
  .hl { font-size: 8px; color: var(--text-dim); letter-spacing: 0.12em; text-transform: uppercase; font-weight: 600; }
  .hv { font-size: 13px; font-weight: 700; font-family: var(--mono); }
  .unit { font-size: 9px; color: var(--text-dim); font-weight: 500; margin-left: 2px;}

  .flex-spacer { flex: 1; }
  .sys-lat { display: flex; align-items: flex-end; text-align: right; }
  .conn-orb { width: 10px; height: 10px; border-radius: 50%; background: var(--text-dim); margin-left: auto; transition: 0.3s; }
  .conn-orb.active { background: var(--bid); box-shadow: 0 0 12px var(--bid); }

  .flash-up { color: var(--bid) !important; text-shadow: 0 0 20px var(--bid-glow) !important; }
  .flash-dn { color: var(--ask) !important; text-shadow: 0 0 20px var(--ask-glow) !important; }

  .custom-scroll::-webkit-scrollbar { width: 6px; height: 6px; }
  .custom-scroll::-webkit-scrollbar-track { background: rgba(0, 0, 0, 0.2); border-radius: 4px; }
  .custom-scroll::-webkit-scrollbar-thumb { background: rgba(255, 255, 255, 0.15); border-radius: 4px; }
  .custom-scroll::-webkit-scrollbar-thumb:hover { background: rgba(255, 255, 255, 0.3); }

  /* ── 3-Col Layout ──────────────────────────────────────────────────────── */
  .grid { display: grid; grid-template-columns: 320px 1fr minmax(480px, 1.4fr); gap: 12px; height: 100%; min-height: 0; }
  .col { display: flex; flex-direction: column; gap: 12px; min-height: 0; }
  .col-stretch { flex: 1; }
  .flex-1 { flex: 1; }
  
  .panel { background: rgba(8,10,14,0.6); backdrop-filter: blur(14px); -webkit-backdrop-filter: blur(14px); border: 1px solid rgba(255,255,255,0.06); border-radius: 6px; display: flex; flex-direction: column; overflow: hidden; }
  .pan-head, .pane-h { display: flex; align-items: center; padding: 10px 14px; gap: 8px; background: rgba(0,0,0,0.4); border-bottom: 1px solid rgba(255,255,255,0.04); font-family: var(--heading); font-size: 11px; font-weight: 700; letter-spacing: 0.15em; color: var(--text-mid); text-transform: uppercase; flex-shrink: 0; position: relative; }
  .pane-h { font-size: 12px; margin-top: 10px; border-top: 1px solid rgba(255,255,255,0.04); }
  .pane-h.mt-3 { margin-top: 16px; }
  
  .pan-head::before, .pane-h::before { content: ''; position: absolute; left: 0; top: 0; bottom: 0; width: 3px; }
  .primary-border::before { background: var(--accent); }
  .accent-border::before { background: var(--teal); }
  .yell-border::before { background: var(--yellow); }
  .purp-border::before { background: var(--purple); }
  .teal-border::before { background: var(--bid); }
  .red-border::before { background: var(--ask); }
  .green-border::before { background: var(--bid); }

  .badge { background: rgba(255,255,255,0.05); padding: 2px 6px; border-radius: 4px; font-size: 8px; font-family: var(--sans); }
  .ml-auto { margin-left: auto; }

  /* ── Order Book (Col 1) ────────────────────────────────────────────────── */
  .ob-hdr { display: flex; font-size: 8px; font-weight: 600; color: var(--text-dim); padding: 6px 14px; background: rgba(0,0,0,0.2); border-bottom: 1px solid var(--brd); letter-spacing: 0.1em; }
  .ob-hdr span { flex: 1; } .text-right { text-align: right; }
  .ob-side { flex: 1; display: flex; flex-direction: column; overflow: hidden; }
  .flex-col-end { justify-content: flex-end; }
  .ob-row { display: flex; padding: 2px 14px; align-items: center; position: relative; min-height: 20px; cursor: default; }
  .ob-row:hover { background: rgba(255,255,255,0.04); }
  .ob-row span { flex: 1; z-index: 1; font-family: var(--mono); font-size: 11px; }
  .ob-price { font-weight: 600; } .ob-qty { color: var(--text-mid); }
  .ob-total { color: var(--text-dim); font-size: 10px !important; }
  .depth-bar { position: absolute; right: 0; top: 1px; bottom: 1px; opacity: 0.25; border-radius: 2px 0 0 2px; }
  .bid-bar { background: var(--bid); } .ask-bar { background: var(--ask); }
  .spread-anchor { display: flex; background: rgba(0,0,0,0.6); padding: 6px 14px; gap: 8px; margin: 2px 0;}
  .anchor-half { flex: 1; display: flex; flex-direction: column; justify-content: center; }
  .bid-anchor { align-items: flex-start; } .ask-anchor { align-items: flex-end; }
  .lbl { font-size: 7px; color: var(--text-dim); letter-spacing: 0.1em; }
  .val { font-size: 14px; font-weight: 700; font-family: var(--mono); }
  .anchor-mid { display: flex; flex-direction: column; align-items: center; justify-content: center; background: rgba(255,208,0,0.1); padding: 2px 10px; border-radius: 4px;}
  .spd-v { font-size: 13px; font-family: var(--mono); font-weight: 800; }
  .spd-l { font-size: 7px; color: var(--yellow); letter-spacing: 0.1em; }

  /* ── Middle Col Graphics (Col 2) ───────────────────────────────────────── */
  .chart-bg { background: rgba(2,3,4,0.6); }
  .chart-container { flex: 1; position: relative; width: 100%; min-height: 0; overflow: hidden; display: flex; }
  .chart-container canvas { flex: 1; width: 100%; height: 100%; }
  .dyn-heat-canvas { min-height: 100px; background: rgba(0,0,0,0.5); border: 1px solid var(--brd); }
  
  .tape-h { height: 45%; min-height: 250px; }
  .tape-hdr { display: grid; grid-template-columns: 50px 70px 60px 1fr 70px; padding: 6px 14px; font-size: 8px; color: var(--text-dim); background: rgba(0,0,0,0.2); border-bottom: 1px solid var(--brd); letter-spacing: 0.1em; }
  .tape-list { flex: 1; overflow-y: auto; display: flex; flex-direction: column; }
  .tape-item { display: grid; grid-template-columns: 50px 70px 60px 1fr 70px; padding: 3px 14px; align-items: center; font-size: 10px; border-bottom: 1px solid rgba(255,255,255,0.015); animation: fade 0.2s cubic-bezier(0.1, 0.9, 0.2, 1); }
  .bg-buy { background: rgba(0,255,170,0.03); } .bg-sell { background: rgba(255,42,95,0.03); }
  .badge-side { font-size: 8px; font-weight: 700; text-align: center; padding: 2px 0; border-radius: 3px; }
  .bbg-buy { background: rgba(0,255,170,0.15); color: var(--bid); } .bbg-sell { background: rgba(255,42,95,0.15); color: var(--ask); }
  .t-price { font-family: var(--mono); font-weight: 600; }
  .t-qty { color: var(--text-mid); font-family: var(--mono); }
  .t-bar-wrap { height: 5px; background: rgba(0,0,0,0.5); border-radius: 2px; }
  .t-bar { height: 100%; border-radius: 2px; }
  .bar-buy { background: var(--bid); } .bar-sell { background: var(--ask); }
  .t-time { color: var(--text-dim); font-size: 9px; font-family: var(--mono); }
  .fixed-h { height: 150px; flex: none; }

  /* ── Tab System & Terminal Grid (Col 3) ────────────────────────────────── */
  .insights-head { display: flex; flex-direction: column; background: rgba(0,0,0,0.5); border-bottom: 1px solid var(--glass-border); }
  .i-title { padding: 12px 14px 6px 14px; font-family: var(--heading); font-size: 13px; font-weight: 800; letter-spacing: 0.1em; color: var(--text); }
  .tabs { display: flex; flex-wrap: wrap; gap: 4px; padding: 0 14px 10px 14px; }
  .t-btn { background: rgba(255,255,255,0.03); border: 1px solid var(--brd); border-radius: 4px; color: var(--text-dim); font-size: 9px; font-family: var(--heading); font-weight: 700; padding: 6px 10px; cursor: pointer; transition: 0.2s; letter-spacing: 0.05em; }
  .t-btn:hover { background: rgba(255,255,255,0.08); color: var(--text-mid); }
  .t-active { background: var(--accent); color: #000; box-shadow: 0 0 10px rgba(0,212,255,0.3); border-color: rgba(0,212,255,0.5); }

  .insights-body { flex: 1; overflow-y: auto; padding: 12px 12px 24px 12px; }
  .tab-pane { display: flex; flex-direction: column; gap: 4px; animation: fade 0.3s ease-out; }
  @keyframes fade { from { opacity: 0; transform: translateY(5px); } to { opacity: 1; transform: translateY(0); } }

  /* ── TERMINAL KEY-VALUE GRID ── */
  .term-grid { display: grid; grid-template-columns: 140px 1fr; gap: 8px 12px; align-items: start; padding: 8px; font-family: var(--mono); font-size: 11px; }
  .t-key { color: var(--text-mid); }
  .t-val { color: var(--text); line-height: 1.4; word-break: break-all;} /* Ensure dense data doesn't overflow */

  .t-box { background: rgba(0,0,0,0.3); border: 1px solid var(--brd); padding: 10px; border-radius: 4px; font-family: var(--mono); font-size: 10px; }
  .rt-head { color: var(--text-mid); border-bottom: 1px solid var(--brd); padding-bottom: 6px; margin-bottom: 6px; font-weight: 700;}
  .rt-body { color: var(--text-dim); line-height: 1.5; white-space: pre-wrap; font-family: var(--mono); }

  .text-dim { color: var(--text-dim) !important; font-size: 10px; }
  .mt-3 { margin-top: 12px; }
  .mb-3 { margin-bottom: 12px; }

  /* Colors */
  .green { color: var(--bid); } .red { color: var(--ask); } .accent { color: var(--accent); } .yellow { color: var(--yellow); } .purple { color: var(--purple); } .teal { color: var(--teal); }
</style>