<svelte:head>
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin="anonymous" />
  <link
    href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400;500;600;700&family=Space+Grotesk:wght@300;400;500;600&display=swap"
    rel="stylesheet"
  />
</svelte:head>
<script>
  let { children } = $props();
</script>

{@render children()}
<style>
  :global(*, *::before, *::after) {
    box-sizing: border-box;
    margin: 0;
    padding: 0;
  }

  :global(:root) {
    /* ── Core backgrounds ─────────────────────────────────── */
    --bg0:   #070809;
    --bg1:   #0c0e12;
    --bg2:   #111419;
    --bg3:   #161a21;
    --bg4:   #1c2130;
    --bg5:   #222844;

    /* ── Borders ──────────────────────────────────────────── */
    --brd:   #1a2035;
    --brd2:  #232c42;

    /* ── Semantic colors ──────────────────────────────────── */
    --bid:      #00e5a0;
    --bid-glow: rgba(0, 229, 160, 0.18);
    --bid-dim:  rgba(0, 229, 160, 0.10);
    --bid-mid:  rgba(0, 229, 160, 0.35);

    --ask:      #ff3d5a;
    --ask-glow: rgba(255, 61, 90, 0.18);
    --ask-dim:  rgba(255, 61, 90, 0.10);
    --ask-mid:  rgba(255, 61, 90, 0.35);

    --accent:   #3d8fff;
    --accent2:  #6eb3ff;
    --accent-dim: rgba(61, 143, 255, 0.12);

    --yellow:   #f5c518;
    --yellow-dim: rgba(245, 197, 24, 0.12);

    --purple:   #a78bfa;
    --purple-dim: rgba(167, 139, 250, 0.12);

    --teal:     #22d3ee;
    --teal-dim: rgba(34, 211, 238, 0.12);

    --orange:   #fb923c;

    /* ── Text ─────────────────────────────────────────────── */
    --text:     #d4ddf7;
    --text-mid: #7a8bbf;
    --text-dim: #3d4d7a;
    --text-xs:  #2a3460;

    /* ── Typography ───────────────────────────────────────── */
    --mono: 'JetBrains Mono', monospace;
    --sans: 'Space Grotesk', sans-serif;
  }

  :global(html, body) {
    background: var(--bg0);
    color: var(--text);
    font-family: var(--mono);
    font-size: 11px;
    overflow: hidden;
    height: 100%;
    width: 100%;
  }

  :global(canvas) { display: block; }
  :global(button) { cursor: pointer; }
</style>