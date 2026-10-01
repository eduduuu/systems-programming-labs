# Hardware Cache Profiling & Thread Isolation

Benchmarking data structures in C usually involves a heavy initialization phase (allocating memory, parsing files, setting up nodes). If you profile the entire process using standard `perf stat ./benchmark`, the hardware metrics from the setup phase will pollute your actual operation data.

This project solves that by using Operating System IPC (Inter-Process Communication) to isolate hardware profiling strictly to the worker thread executing the core workload.

## 🏗️ Architecture

1. **Python Orchestrator (`script.py`):** Spawns the C benchmark with an unbuffered stdout pipe.
2. **C Worker Thread (`benchmark.c`):** Generates the test array, prints its OS-level thread ID (`gettid()`), and blocks execution via `getchar()`.
3. **Targeted Profiling:** Python intercepts the `tid` and attaches `perf stat -t <tid>` specifically to that OS thread.
4. **Execution Handoff:** Python pipes a newline character (`\n`) to `stdin`, unblocking the C thread to execute the operations.
5. **Telemetry Dump:** Upon completion, Python gracefully halts `perf` with `SIGINT` and parses the L1/LLC cache misses into a CSV.

*Note: This specific harness successfully exposed a silent logical bug in a graph data structure batch-insert function by revealing an anomaly in L1/L2 cache events via PAPI telemetry.*

## 🚀 How to Run

### Option 1: Basic (Perf Only)
Runs the external Python orchestrator to track thread-level Last Level Cache (LLC) misses.

```bash
# 1. Compile the C binary
gcc -O2 -pthread benchmark.c -o benchmark

# 2. Run the orchestrator (requires sudo for perf events)
sudo python3 script.py
```

### Option 2: Advanced (PAPI + Perf)
Compiles the C binary with the PAPI library to track internal, function-by-function cache telemetry alongside the external `perf` check.

```bash
# 1. Compile with PAPI macro and linker flag
gcc -O2 -pthread -DUSE_PAPI benchmark.c -lpapi -o benchmark

# 2. Run the orchestrator
sudo python3 script.py
```

## 📊 Output
Results are automatically logged to:
* `throughput.csv` - Raw operation times and operation counts.
* `benchmark_perf.csv` - External hardware counters (`page-faults`, `L1/LLC-misses`).
* `cache_papi.csv` *(if PAPI enabled)* - Internal function-level instructions and cache references.