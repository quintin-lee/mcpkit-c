#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"

echo "=========================================================="
echo " MCPKit Performance Benchmark Suite"
echo " Build directory: ${BUILD_DIR}"
echo "=========================================================="

EXTRA_ARGS="$*"

if [ ! -f "${BUILD_DIR}/benchmarks/bench_json_rpc" ]; then
    echo "Configuring and building benchmark suite..."
    cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -DMCPKIT_BUILD_BENCHMARKS=ON
    cmake --build "${BUILD_DIR}" -j --target bench_json_rpc bench_http_throughput bench_ws_throughput bench_async_server
fi

echo ""
echo ">>> [1/4] Running JSON-RPC Framing & Serialization Benchmark..."
"${BUILD_DIR}/benchmarks/bench_json_rpc" ${EXTRA_ARGS}

echo ""
echo ">>> [2/4] Running Streamable HTTP POST Throughput Benchmark..."
"${BUILD_DIR}/benchmarks/bench_http_throughput" ${EXTRA_ARGS}

echo ""
echo ">>> [3/4] Running WebSocket Codec & Transport Throughput Benchmark..."
"${BUILD_DIR}/benchmarks/bench_ws_throughput" ${EXTRA_ARGS}

echo ""
echo ">>> [4/4] Running Process-Isolated Async Server RSS Benchmark..."
"${BUILD_DIR}/benchmarks/bench_async_server" ${EXTRA_ARGS}

echo ""
echo "=========================================================="
echo " All benchmarks completed successfully!"
echo "=========================================================="
