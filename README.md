# 🚀 Ultra-Low Latency HFT Order Book Simulator

A high-performance, real-time High-Frequency Trading (HFT) order book simulator and microstructure analytics engine. This project features a completely zero-allocation **C++20 core** for market data processing, a **Node.js binary gateway**, and an ultra-wide professional-grade **SvelteKit dashboard** for real-time telemetry visualization.

## 🏗 System Architecture

```mermaid
graph TD
    classDef cpp fill:#2b3e50,stroke:#668099,stroke-width:2px,color:#fff
    classDef node fill:#3c873a,stroke:#2b6129,stroke-width:2px,color:#fff
    classDef svelte fill:#ff3e00,stroke:#b32b00,stroke-width:2px,color:#fff
    classDef ext fill:#f39c12,stroke:#b17008,stroke-width:2px,color:#fff

    subgraph "External Feeds"
        B[Binance WebSocket Feed]:::ext
    end

    subgraph "C++20 HFT Core Engine"
        MD[LiveMarketData Streamer]:::cpp
        OB[OrderBook Manager]:::cpp
        AE[Microstructure Analytics]:::cpp
        SE[Signal & Risk Engine]:::cpp
        RE[Regime & Dynamics Engine]:::cpp
        BS[TCP Binary Serializer]:::cpp
        
        MD -->|@depth / @aggTrade| OB
        OB --> AE
        AE --> SE
        OB --> RE
        SE --> BS
        RE --> BS
        AE --> BS
    end

    subgraph "Middleware Layer"
        TCP[TCP Listener 8080]:::node
        GW[Node.js WebSocket Gateway 8081]:::node
    end

    subgraph "Client Interface"
        UI[SvelteKit Dashboard]:::svelte
    end

    B -->|RapidJSON Parsing| MD
    BS -->|Custom Binary Protocol| TCP
    TCP --> GW
    GW -->|WebSocket Broadcast| UI
```

## ✨ Key Features

### 🧠 Core HFT Engines
*   **Signal Engine**: Multi-level OFI (Order Flow Imbalance), VPIN (adverse selection), Momentum/Mean Reversion detection, and anomaly alerts (Iceberg orders, Quote Stuffing, Spoofing).
*   **Risk Engine**: Real-time simulated position tracking, slippage calculation (microprice vs execution), fill probability models, and max drawdown tracking.
*   **Strategy Engine**: Avellaneda-Stoikov market-making simulation, Latency Arb simulation, and Cross-Exchange Arb detection.
*   **Regime Engine**: Volatility regimes (Yang-Zhang), Spread widening alerts, and HMM-based (Hidden Markov Model) market state detection.
*   **Book Dynamics**: Heatmap intensity tracking, depth migration velocity, and finite-difference order book gradients.

### ⚡ Performance Optimizations
*   **Zero-Allocation Memory Pools**: Uses an `OrderPool` pattern to definitively eliminate heap allocation overhead and memory fragmentation.
*   **Lock-Free Queues**: Utilizes `boost::lockfree::spsc_queue` for wait-free bounded latency message passing between WS and processing threads.
*   **SIMD Math Acceleration**: Implements AVX2 vectorized linear regression and array reductions for calculation speedups up to 4x.
*   **Custom Binary Protocol**: Highly efficient C++ to Node.js telemetry transmission over local TCP sockets.

## 🛠 Tech Stack

**Engine (C++)**
- `C++20`, `CMake`, `vcpkg`
- **Libraries:** `Boost.Lockfree`, `Boost.Asio`, `OpenSSL`, `CURL`, `RapidJSON`

**Gateway (Node.js)**
- `Node.js` (v18+)
- **Libraries:** `ws` (WebSockets), `net` (TCP Sockets)

**Dashboard (Svelte)**
- `SvelteKit`, `Vite`
- **Styles:** Custom CSS Grid, modern dynamic layouts

## 📦 Installation & Setup

### 1. Prerequisites
- **OS:** Windows (Visual Studio 2022 with C++ desktop development workload recommended).
- **Tools:** CMake (3.15+), vcpkg package manager.
- **Runtimes:** Node.js (v18+).

### 2. Building the C++ Engine

Ensure `vcpkg` is correctly configured in your environment.

```powershell
# From the project root directory
mkdir build
cd build

# Replace the path below with your actual vcpkg installation path
cmake .. -DCMAKE_TOOLCHAIN_FILE=A:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

> **Note**: Update the `BOOST_ROOT` and `OPENSSL_ROOT_DIR` environment paths inside `CMakeLists.txt` if your vcpkg installation differs.

### 3. Setting up the Node.js Gateway

The gateway bridges the C++ core's binary TCP output to the frontend via WebSockets.

```bash
cd gateway
npm install
npm run dev
```

### 4. Setting up the Svelte UI

Launch the professional 4-column HFT monitoring dashboard.

```bash
cd ui
npm install
npm run dev
```

## 🚀 Running the System

To see the full pipeline in action, execute the components in this order:

1.  **Start the Gateway:** Runs the Node.js bridge. (Listens on TCP `:8080` and WS `:8081`).
2.  **Start the UI:** Launch the Svelte development server and open `http://localhost:5173` in your browser.
3.  **Launch the Engine:** Run the compiled `LiveMarketData.exe` located in your `build` directory.
    - The engine will aggressively connect to the Binance WebSocket feed and immediately stream live, fully decoded telemetry to your dashboard.

## 📖 Additional Documentation
For a deep dive into latency reduction techniques, structural optimizations, and benchmark results, see [PERFORMANCE_IMPROVEMENTS.md](./PERFORMANCE_IMPROVEMENTS.md).

---
> **Disclaimer**: This system is designed for research and educational purposes. All interactions with the Binance API must rigorously adhere to their Terms of Service.
