# TradeSim — Verifiable Sharded Trade Matching Engine

**TradeSim** is a verifiable, high-performance C++ order matching engine.

It simulates how crypto exchanges handle thousands of trades per second, with a **verifiable write-ahead log (WAL)** and a built-in **WebSocket API** for live trading updates.

---

## 🚀 Quick Start

### 🧰 Requirements
- Linux or WSL (Ubuntu 22.04+)
- `g++` (>= 11)
- No external dependencies — only the C++ standard library.

### 🧱 Build
From the project root:

```bash
g++ -O3 -std=gnu++17 -pthread \
  apps/ws_server.cpp engine/engine.cpp \
  -Iengine -o ws_tradesim
