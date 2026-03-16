<script>
  // =============================================================================
  // src/routes/+page.svelte  —  HFT OrderBook Dashboard
  //
  // Data flow: WebSocket (ws://localhost:9002) → reactive stores → PixiJS WebGL
  //
  // Panels:
  //   • OrderBook table — bid/ask levels with animated depth bars
  //   • PixiJS WebGL depth chart — GPU-rendered cumulative depth curve
  //   • Mid-price sparkline  (Canvas 2D — lightweight)
  //   • Stats strip — mirrors PerformanceMetrics + MarketAnalytics from C++
  //   • Trade tape — live fill stream
  // =============================================================================
  import { onMount, onDestroy, tick } from 'svelte';

  // ── Config ──────────────────────────────────────────────────────────────────
  const WS_URL        = 'ws://localhost:9002';
  const BOOK_DEPTH    = 20;   // levels shown in table
  const CHART_DEPTH   = 50;   // depth chart uses all available levels
  const PRICE_SCALE   = 10000;// matches C++ encoding
  const MAX_TRADES    = 30;
  const RECONNECT_MS  = 1500;
  const SPARKLINE_LEN = 120;

  // ── Reactive state ───────────────────────────────────────────────────────────
  let connected = $state(false);
  let bids = $state([]);   
  let asks = $state([]);   

  let analytics = $state({
    bestBid:0, bestAsk:0, mid:0, spread:0, spreadBps:0,
    bidLiq:0, askLiq:0, imbal:0
  });

  let stats = $state({
    msgsReceived:0, msgsProcessed:0, msgsDropped:0,
    avgLatUs:0, maxLatUs:0, minLatUs:0,
    spreadBps:0, midPrice:0, imbalance:0, ofi:0, seqNum:0
  });

  let trades = $state([]);
  let midHistory = $state([]);

  let mps = $state(0);
  let midChange = $state(0);

  let maxBidQty = $state(1);
  let maxAskQty = $state(1);

  // ── Internal counters ────────────────────────────────────────────────────────
  let ws;
  let reconnectTimer;
  let msgsThisSec = 0;
  let mpsInterval;
  let lastMid = 0;

  // ── PixiJS refs ──────────────────────────────────────────────────────────────
  let pixiCanvas;
  let pixiApp;
  let depthGraphics;  // PIXI.Graphics for the WebGL depth chart

  // ── Sparkline canvas ─────────────────────────────────────────────────────────
  let sparkCanvas;
  let spreadCanvas;

  // ── WebSocket ────────────────────────────────────────────────────────────────
  function connect() {
    ws = new WebSocket(WS_URL);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      connected = true;
      console.log('[WS] Connected to gateway');
    };

    ws.onmessage = (evt) => {
      msgsThisSec++;
      try {
        const msg = JSON.parse(evt.data);
        handleMessage(msg);
      } catch {}
    };

    ws.onclose = () => {
      connected = false;
      reconnectTimer = setTimeout(connect, RECONNECT_MS);
    };

    ws.onerror = () => { ws.close(); };
  }

  function handleMessage(msg) {
    if (msg.type === 'snapshot') {
      // bids arrive best→worst from gateway (sorted by parser)
      bids = msg.bids.slice(0, BOOK_DEPTH);
      asks = msg.asks.slice(0, BOOK_DEPTH);
      analytics = msg.analytics;

      const mid = analytics.mid;
      midChange = mid - lastMid;
      lastMid = mid;

      midHistory = [...midHistory, mid].slice(-SPARKLINE_LEN);

      drawDepthChart(bids, asks);
      drawSparkline();
      drawSpreadSparkline(analytics.spreadBps);
    } else if (msg.type === 'trade') {
      const t = { ...msg, ts: new Date(msg.ts).toISOString().slice(11, 23) };
      trades = [t, ...trades].slice(0, MAX_TRADES);
    } else if (msg.type === 'stats') {
      stats = msg;
    }
  }

  // ── PixiJS depth chart (WebGL) ────────────────────────────────────────────
  async function initPixi() {
    const PIXI = await import('https://cdn.jsdelivr.net/npm/pixi.js@7.3.2/dist/pixi.min.mjs');

    pixiApp = new PIXI.Application({
      view: pixiCanvas,
      width: pixiCanvas.clientWidth || 700,
      height: pixiCanvas.clientHeight || 220,
      backgroundColor: 0x0f1115,
      antialias: true,
      autoDensity: true,
      resolution: window.devicePixelRatio || 1,
    });

    depthGraphics = new PIXI.Graphics();
    pixiApp.stage.addChild(depthGraphics);

    // Responsive resize
    const ro = new ResizeObserver(() => {
      const w = pixiCanvas.clientWidth;
      const h = pixiCanvas.clientHeight;
      pixiApp.renderer.resize(w, h);
    });
    ro.observe(pixiCanvas);
  }

  function drawDepthChart(rawBids, rawAsks) {
    if (!depthGraphics || !pixiApp) return;

    const W = pixiApp.renderer.width;
    const H = pixiApp.renderer.height;
    const g = depthGraphics;
    g.clear();

    if (!rawBids.length || !rawAsks.length) return;

    const bestBid = rawBids[0].price;
    const bestAsk = rawAsks[0].price;
    const mid     = (bestBid + bestAsk) / 2;

    // Price window: mid ± 0.5%
    const window = mid * 0.005;
    const priceMin = mid - window;
    const priceMax = mid + window;

    const priceToX = (p) => ((p - priceMin) / (priceMax - priceMin)) * W;

    // Build cumulative bid depth (right→left from spread)
    const bidDepth = [];
    let cumBid = 0;
    for (const { price, qty } of rawBids) {
      if (price < priceMin) break;
      cumBid += qty;
      bidDepth.unshift({ x: priceToX(price), y: cumBid });
    }

    // Build cumulative ask depth (left→right from spread)
    const askDepth = [];
    let cumAsk = 0;
    for (const { price, qty } of rawAsks) {
      if (price > priceMax) break;
      cumAsk += qty;
      askDepth.push({ x: priceToX(price), y: cumAsk });
    }

    const maxDepth = Math.max(cumBid, cumAsk, 1);
    const yScale   = (v) => H - (v / maxDepth) * (H * 0.85) - 4;
    const midX     = priceToX(mid);

    // ── Bid fill (green) ──────────────────────────────────────────────────────
    if (bidDepth.length > 1) {
      g.beginFill(0x00e676, 0.15);
      g.moveTo(bidDepth[0].x, H);
      for (const { x, y } of bidDepth) g.lineTo(x, yScale(y));
      g.lineTo(midX, H);
      g.closePath();
      g.endFill();

      // Bid line
      g.lineStyle(1.5, 0x00e676, 0.9);
      g.moveTo(bidDepth[0].x, H);
      for (const { x, y } of bidDepth) g.lineTo(x, yScale(y));
    }

    // ── Ask fill (red) ────────────────────────────────────────────────────────
    if (askDepth.length > 1) {
      g.beginFill(0xff4757, 0.15);
      g.moveTo(midX, H);
      for (const { x, y } of askDepth) g.lineTo(x, yScale(y));
      g.lineTo(askDepth[askDepth.length - 1].x, H);
      g.closePath();
      g.endFill();

      // Ask line
      g.lineStyle(1.5, 0xff4757, 0.9);
      g.moveTo(midX, H);
      for (const { x, y } of askDepth) g.lineTo(x, yScale(y));
    }

    // ── Mid price line ────────────────────────────────────────────────────────
    g.lineStyle(1, 0x4a9eff, 0.6);
    g.moveTo(midX, 0);
    g.lineTo(midX, H);

    // ── Spread zone ───────────────────────────────────────────────────────────
    const bidX = priceToX(bestBid);
    const askX = priceToX(bestAsk);
    g.lineStyle(0);
    g.beginFill(0xffd666, 0.06);
    g.drawRect(bidX, 0, askX - bidX, H);
    g.endFill();

    // ── Grid lines ────────────────────────────────────────────────────────────
    g.lineStyle(0.5, 0x1e2540, 0.8);
    const yTicks = 4;
    for (let i = 1; i <= yTicks; i++) {
      const y = H - (i / (yTicks + 1)) * H * 0.85 - 4;
      g.moveTo(0, y);
      g.lineTo(W, y);
    }
  }

  // ── Sparklines (Canvas 2D — lightweight) ─────────────────────────────────
  function drawSparkline() {
    if (!sparkCanvas || midHistory.length < 2) return;
    const c = sparkCanvas;
    const ctx = c.getContext('2d');
    c.width = c.offsetWidth;
    c.height = c.offsetHeight;
    const W = c.width, H = c.height;
    const mn = Math.min(...midHistory), mx = Math.max(...midHistory);
    const rng = mx - mn || 0.01;
    ctx.clearRect(0, 0, W, H);
    ctx.strokeStyle = '#4a9eff';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    midHistory.forEach((v, i) => {
      const x = (i / (midHistory.length - 1)) * W;
      const y = H - ((v - mn) / rng) * (H - 6) - 3;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
    // Area fill
    ctx.lineTo(W, H); ctx.lineTo(0, H); ctx.closePath();
    ctx.fillStyle = 'rgba(74,158,255,0.08)';
    ctx.fill();
  }

  let spreadHistory = [];
  function drawSpreadSparkline(bps) {
    spreadHistory = [...spreadHistory, bps].slice(-SPARKLINE_LEN);
    if (!spreadCanvas || spreadHistory.length < 2) return;
    const c = spreadCanvas;
    const ctx = c.getContext('2d');
    c.width = c.offsetWidth; c.height = c.offsetHeight;
    const W = c.width, H = c.height;
    const mn = Math.min(...spreadHistory), mx = Math.max(...spreadHistory);
    const rng = mx - mn || 0.01;
    ctx.clearRect(0, 0, W, H);
    ctx.strokeStyle = '#ffd666';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    spreadHistory.forEach((v, i) => {
      const x = (i / (spreadHistory.length - 1)) * W;
      const y = H - ((v - mn) / rng) * (H - 6) - 3;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
    ctx.lineTo(W, H); ctx.lineTo(0, H); ctx.closePath();
    ctx.fillStyle = 'rgba(255,214,102,0.08)';
    ctx.fill();
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
    clearInterval(mpsInterval);
    pixiApp?.destroy(true);
  });

  // ── Helpers ───────────────────────────────────────────────────────────────
  const fmt  = (v, d=2) => Number(v).toFixed(d);
  const fmtK = (v) => v >= 1000 ? (v/1000).toFixed(1)+'k' : fmt(v);
  const fmtUs= (v) => v < 1000 ? v.toFixed(2)+'μs' : (v/1000).toFixed(2)+'ms';
  const midUp= () => midChange >= 0;



  $effect(() => {
    maxBidQty = Math.max(...bids.map(b => b.qty), 1);
    maxAskQty = Math.max(...asks.map(a => a.qty), 1);
  });
</script>

<!-- ══ Markup ══════════════════════════════════════════════════════════════ -->
<div class="shell">

  <!-- Header -->
  <header>
    <span class="brand">⬡ HFT ENGINE</span>
    <span class="sep"></span>
    <div class="hstat">
      <span class="hl">Symbol</span>
      <span class="hv accent">BTCUSDT</span>
    </div>
    <div class="hstat">
      <span class="hl">Status</span>
      <span class="hv" class:green={connected} class:red={!connected}>
        {connected ? 'LIVE' : 'OFFLINE'}
      </span>
    </div>
    <div class="hstat">
      <span class="hl">Seq</span>
      <span class="hv">{stats.seqNum.toLocaleString()}</span>
    </div>
    <div class="hstat">
      <span class="hl">Msgs/s</span>
      <span class="hv purple">{mps.toLocaleString()}</span>
    </div>
    <div class="hstat">
      <span class="hl">Avg Lat</span>
      <span class="hv teal">{fmtUs(stats.avgLatUs)}</span>
    </div>
    <div class="hstat">
      <span class="hl">Max Lat</span>
      <span class="hv yellow">{fmtUs(stats.maxLatUs)}</span>
    </div>
    <div class="hstat">
      <span class="hl">Dropped</span>
      <span class="hv" class:red={stats.msgsDropped > 0}>{stats.msgsDropped.toLocaleString()}</span>
    </div>
    <span class="spacer"></span>
    <div class="pulse" class:active={connected}></div>
    <span class="live-lbl" class:green={connected}>{connected ? 'LIVE' : 'DISCONNECTED'}</span>
  </header>

  <!-- Main grid -->
  <div class="grid">

    <!-- ── OrderBook ── -->
    <section class="panel ob-panel">
      <div class="ph">
        <span class="ptitle">Order Book</span>
        <span class="pbadge">DEPTH {BOOK_DEPTH}</span>
      </div>
      <div class="ob-cols">
        <span>Price (USD)</span><span>Qty</span><span>Total</span>
      </div>

      <div class="ob-scroll">
        <!-- Asks (reversed: worst → best, so best ask is at bottom near spread) -->
        {#each [...asks].reverse() as row (row.price)}
          <div class="ob-row ask">
            <div class="depth-bar ask" style="width:{(row.qty/maxAskQty*100).toFixed(1)}%"></div>
            <span class="ob-price ask">{fmt(row.price, 4)}</span>
            <span class="ob-num">{fmtK(row.qty)}</span>
            <span class="ob-num dim">{fmtK(asks.slice(asks.indexOf(row)).reduce((s,r)=>s+r.qty,0))}</span>
          </div>
        {/each}
      </div>

      <!-- Spread bar -->
      <div class="spread-bar">
        <span class="sl">MID</span>
        <span class="mid-price" class:up={midUp()} class:dn={!midUp()}>
          ${fmt(analytics.mid, 4)}
        </span>
        <span class="sl">SPD</span>
        <span class="spread-val">{fmt(analytics.spreadBps, 2)}</span>
        <span class="sl">bps</span>
      </div>

      <div class="ob-scroll">
        {#each bids as row (row.price)}
          <div class="ob-row bid">
            <div class="depth-bar bid" style="width:{(row.qty/maxBidQty*100).toFixed(1)}%"></div>
            <span class="ob-price bid">{fmt(row.price, 4)}</span>
            <span class="ob-num">{fmtK(row.qty)}</span>
            <span class="ob-num dim">{fmtK(bids.slice(0, bids.indexOf(row)+1).reduce((s,r)=>s+r.qty,0))}</span>
          </div>
        {/each}
      </div>
    </section>

    <!-- ── PixiJS Depth Chart ── -->
    <section class="panel depth-panel">
      <div class="ph">
        <span class="ptitle">Cumulative Depth — WebGL (PixiJS)</span>
        <span class="pbadge">GPU</span>
      </div>
      <canvas bind:this={pixiCanvas} class="pixi-canvas"></canvas>
      <div class="chart-labels">
        <span class="cl green">▲ Bids</span>
        <span class="cl yellow">Spread zone</span>
        <span class="cl red">▼ Asks</span>
      </div>
    </section>

    <!-- ── Stats ── -->
    <section class="panel stats-panel">
      <div class="ph"><span class="ptitle">Market Stats</span></div>
      <div class="stat-list">
        <div class="sr"><span class="sk">Best Bid</span><span class="sv green">${fmt(analytics.bestBid,4)}</span></div>
        <div class="sr"><span class="sk">Best Ask</span><span class="sv red">${fmt(analytics.bestAsk,4)}</span></div>
        <div class="sr"><span class="sk">Mid Price</span><span class="sv" class:green={midUp()} class:red={!midUp()}>${fmt(analytics.mid,4)}</span></div>
        <div class="sr"><span class="sk">Spread</span><span class="sv yellow">{fmt(analytics.spreadBps,2)} bps</span></div>
        <div class="sr"><span class="sk">Bid Liq</span><span class="sv green">{fmtK(analytics.bidLiq)}</span></div>
        <div class="sr"><span class="sk">Ask Liq</span><span class="sv red">{fmtK(analytics.askLiq)}</span></div>
        <div class="sr"><span class="sk">Imbalance</span><span class="sv accent">{(analytics.imbal*100).toFixed(1)}%</span></div>
        <div class="sr"><span class="sk">OFI</span><span class="sv purple">{fmt(stats.ofi,2)}</span></div>

        <div class="divider"></div>

        <div class="sr"><span class="sk">Msgs Rx</span><span class="sv">{stats.msgsReceived.toLocaleString()}</span></div>
        <div class="sr"><span class="sk">Msgs OK</span><span class="sv teal">{stats.msgsProcessed.toLocaleString()}</span></div>
        <div class="sr"><span class="sk">Dropped</span><span class="sv" class:red={stats.msgsDropped>0}>{stats.msgsDropped.toLocaleString()}</span></div>
        <div class="sr"><span class="sk">Avg Lat</span><span class="sv teal">{fmtUs(stats.avgLatUs)}</span></div>
        <div class="sr"><span class="sk">Max Lat</span><span class="sv yellow">{fmtUs(stats.maxLatUs)}</span></div>
        <div class="sr"><span class="sk">Min Lat</span><span class="sv green">{fmtUs(stats.minLatUs)}</span></div>

        <div class="divider"></div>

        <!-- Mid price sparkline -->
        <div class="spark-row">
          <span class="sk">Mid price</span>
          <canvas bind:this={sparkCanvas} class="spark"></canvas>
        </div>
        <!-- Spread sparkline -->
        <div class="spark-row">
          <span class="sk">Spread bps</span>
          <canvas bind:this={spreadCanvas} class="spark"></canvas>
        </div>
      </div>
    </section>

    <!-- ── Trade Tape ── -->
    <section class="panel tape-panel">
      <div class="ph">
        <span class="ptitle">Trade Tape</span>
        <span class="pbadge">{trades.length} fills</span>
      </div>
      <div class="tape-cols">
        <span>Side</span><span>Price</span><span>Qty</span><span>Time</span>
      </div>
      <div class="tape-scroll">
        {#each trades as t (t.seqNum + '-' + t.ts)}
          <div class="tape-row" class:buy={t.side==='buy'} class:sell={t.side==='sell'}>
            <span class="side-badge" class:buy={t.side==='buy'} class:sell={t.side==='sell'}>
              {t.side.toUpperCase()}
            </span>
            <span class="t-price">${fmt(t.price,4)}</span>
            <span class="t-qty">{fmtK(t.qty)}</span>
            <span class="t-time">{t.ts}</span>
          </div>
        {/each}
        {#if trades.length === 0}
          <div class="empty">Waiting for fills…</div>
        {/if}
      </div>
    </section>

    <!-- ── Metrics strip ── -->
    <section class="metrics-strip">
      <div class="mc">
        <span class="ml">Mid Price</span>
        <span class="mv" class:green={midUp()} class:red={!midUp()}>${fmt(analytics.mid,2)}</span>
        <span class="ms" class:green={midUp()} class:red={!midUp()}>
          {midChange >= 0 ? '+' : ''}{fmt(midChange,4)}
        </span>
      </div>
      <div class="mc">
        <span class="ml">Spread</span>
        <span class="mv yellow">{fmt(analytics.spreadBps,2)}</span>
        <span class="ms">bps</span>
      </div>
      <div class="mc">
        <span class="ml">Imbalance</span>
        <span class="mv accent">{(analytics.imbal*100).toFixed(1)}%</span>
        <span class="ms">{analytics.imbal > 0 ? 'bid pressure' : 'ask pressure'}</span>
      </div>
      <div class="mc">
        <span class="ml">OFI</span>
        <span class="mv purple">{fmt(stats.ofi,2)}</span>
        <span class="ms">order flow imbal</span>
      </div>
      <div class="mc">
        <span class="ml">Msgs/s</span>
        <span class="mv teal">{mps.toLocaleString()}</span>
        <span class="ms">{stats.msgsDropped} dropped</span>
      </div>
      <div class="mc" style="border-right:none">
        <span class="ml">Avg Latency</span>
        <span class="mv yellow">{fmtUs(stats.avgLatUs)}</span>
        <span class="ms">max {fmtUs(stats.maxLatUs)}</span>
      </div>
    </section>

  </div><!-- /grid -->
</div><!-- /shell -->

<!-- ══ Styles ══════════════════════════════════════════════════════════════ -->
<style>
  :root {
    --bg0:#0a0b0d; --bg1:#0f1115; --bg2:#141720; --bg3:#1c2030; --bg4:#242840;
    --brd:#1e2540;
    --bid:#00e676; --bid-dim:rgba(0,230,118,.12); --bid-mid:rgba(0,230,118,.4);
    --ask:#ff4757; --ask-dim:rgba(255,71,87,.12);  --ask-mid:rgba(255,71,87,.4);
    --accent:#4a9eff; --accent-dim:rgba(74,158,255,.12);
    --text:#e2e8f8; --text-dim:#5a6a9a; --text-mid:#8896c0;
    --yellow:#ffd666; --purple:#b57bff; --teal:#00d4aa;
    --mono:'IBM Plex Mono',monospace;
  }
  *{box-sizing:border-box;margin:0;padding:0}

  .shell{
    background:var(--bg0);color:var(--text);
    font-family:var(--mono);font-size:11px;
    display:grid;grid-template-rows:44px 1fr;
    min-height:100vh;
  }

  /* Header */
  header{
    display:flex;align-items:center;gap:20px;
    padding:0 16px;background:var(--bg1);
    border-bottom:1px solid var(--brd);
  }
  .brand{font-size:12px;font-weight:600;letter-spacing:.12em;color:var(--accent)}
  .sep{width:1px;height:20px;background:var(--brd)}
  .hstat{display:flex;flex-direction:column;gap:1px}
  .hl{font-size:9px;color:var(--text-dim);letter-spacing:.08em;text-transform:uppercase}
  .hv{font-size:12px;font-weight:500}
  .spacer{flex:1}
  .pulse{width:6px;height:6px;border-radius:50%;background:var(--text-dim)}
  .pulse.active{background:var(--bid);animation:blink 1.4s infinite}
  @keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
  .live-lbl{font-size:9px;letter-spacing:.1em}

  /* Grid */
  .grid{
    display:grid;
    grid-template-columns:280px 1fr 220px;
    grid-template-rows:1fr 140px 48px;
    gap:1px;background:var(--brd);
    height:calc(100vh - 44px);
  }

  /* Panels */
  .panel{
    background:var(--bg1);overflow:hidden;
    display:flex;flex-direction:column;
  }
  .ph{
    display:flex;align-items:center;gap:8px;
    padding:7px 12px;background:var(--bg2);
    border-bottom:1px solid var(--brd);flex-shrink:0;
  }
  .ptitle{font-size:9px;font-weight:500;letter-spacing:.12em;
    text-transform:uppercase;color:var(--text-dim)}
  .pbadge{
    font-size:9px;padding:1px 6px;border-radius:2px;
    background:var(--bg4);color:var(--text-mid);margin-left:auto;
  }

  /* OrderBook panel */
  .ob-panel{grid-column:1;grid-row:1/3}
  .ob-cols{
    display:grid;grid-template-columns:1fr 64px 72px;
    padding:4px 12px;font-size:9px;color:var(--text-dim);
    letter-spacing:.08em;border-bottom:1px solid var(--brd);
    background:var(--bg2);flex-shrink:0;
  }
  .ob-cols span:not(:first-child){text-align:right}
  .ob-scroll{flex:1;overflow:hidden}
  .ob-row{
    display:grid;grid-template-columns:1fr 64px 72px;
    padding:2px 12px;position:relative;cursor:default;
  }
  .depth-bar{
    position:absolute;top:0;bottom:0;right:0;
    opacity:.12;pointer-events:none;transition:width .18s ease;
  }
  .depth-bar.bid{background:var(--bid)}
  .depth-bar.ask{background:var(--ask)}
  .ob-price{font-size:11px;font-weight:500;z-index:1;position:relative}
  .ob-price.bid{color:var(--bid)}
  .ob-price.ask{color:var(--ask)}
  .ob-num{text-align:right;color:var(--text-mid);z-index:1;position:relative}
  .ob-num.dim{color:var(--text-dim);font-size:10px}
  .spread-bar{
    display:flex;align-items:center;justify-content:center;
    padding:5px 12px;background:var(--bg2);gap:8px;
    border-top:1px solid var(--brd);border-bottom:1px solid var(--brd);
    flex-shrink:0;
  }
  .sl{font-size:9px;color:var(--text-dim)}
  .mid-price{font-size:13px;font-weight:600}
  .spread-val{font-size:11px;font-weight:500;color:var(--yellow)}

  /* Depth chart */
  .depth-panel{grid-column:2;grid-row:1}
  .pixi-canvas{width:100%;flex:1;display:block;min-height:0}
  .chart-labels{
    display:flex;justify-content:center;gap:24px;
    padding:5px;border-top:1px solid var(--brd);font-size:9px;flex-shrink:0;
  }
  .cl{color:var(--text-dim)}
  .cl.green{color:var(--bid)}
  .cl.red{color:var(--ask)}
  .cl.yellow{color:var(--yellow)}

  /* Stats */
  .stats-panel{grid-column:3;grid-row:1}
  .stat-list{flex:1;overflow:hidden}
  .sr{
    display:flex;justify-content:space-between;align-items:center;
    padding:5px 12px;border-bottom:1px solid var(--brd);
  }
  .sk{font-size:9px;color:var(--text-dim);letter-spacing:.06em;text-transform:uppercase}
  .sv{font-size:11px;font-weight:500}
  .divider{height:1px;background:var(--bg4);margin:4px 0}
  .spark-row{
    display:flex;align-items:center;justify-content:space-between;
    padding:4px 12px;border-bottom:1px solid var(--brd);gap:8px;
  }
  .spark{flex:1;height:28px;display:block}

  /* Trade tape */
  .tape-panel{grid-column:1;grid-row:3 / span 1}
  /* Actually put tape in row 2 col 1 */
  .tape-panel{grid-column:1;grid-row:2}
  .tape-cols{
    display:grid;grid-template-columns:52px 90px 70px 1fr;
    padding:4px 12px;font-size:9px;color:var(--text-dim);
    letter-spacing:.08em;border-bottom:1px solid var(--brd);
    background:var(--bg2);flex-shrink:0;
  }
  .tape-scroll{flex:1;overflow:hidden;display:flex;flex-direction:column}
  .tape-row{
    display:grid;grid-template-columns:52px 90px 70px 1fr;
    padding:3px 12px;border-bottom:1px solid var(--brd);align-items:center;
    font-size:10px;
  }
  .tape-row.buy{background:rgba(0,230,118,.03)}
  .tape-row.sell{background:rgba(255,71,87,.03)}
  .side-badge{
    font-size:9px;font-weight:500;padding:1px 5px;border-radius:2px;
    text-align:center;
  }
  .side-badge.buy{color:var(--bid);background:var(--bid-dim)}
  .side-badge.sell{color:var(--ask);background:var(--ask-dim)}
  .t-price{color:var(--text)}
  .t-qty{color:var(--text-mid)}
  .t-time{color:var(--text-dim);font-size:9px;text-align:right}
  .empty{padding:12px;color:var(--text-dim);text-align:center;font-size:10px}

  /* Metrics strip */
  .metrics-strip{
    grid-column:2/4;grid-row:2;
    display:flex;background:var(--bg1);
  }
  .mc{
    flex:1;display:flex;flex-direction:column;justify-content:center;
    padding:10px 14px;border-right:1px solid var(--brd);background:var(--bg1);
  }
  .ml{font-size:9px;color:var(--text-dim);letter-spacing:.1em;text-transform:uppercase;margin-bottom:2px}
  .mv{font-size:18px;font-weight:600;letter-spacing:-.02em;line-height:1}
  .ms{font-size:9px;margin-top:2px;color:var(--text-dim)}

  /* Bottom strip */
  .metrics-strip + .metrics-strip,
  .grid > :last-child.metrics-strip{grid-column:1/4}

  /* Color utilities */
  .green{color:var(--bid)}
  .red{color:var(--ask)}
  .accent{color:var(--accent)}
  .yellow{color:var(--yellow)}
  .purple{color:var(--purple)}
  .teal{color:var(--teal)}
  .up{color:var(--bid)}
  .dn{color:var(--ask)}
</style>