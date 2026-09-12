#!/usr/bin/env bash
#
# Smoke-tests the HTTP layer against a real running server: every endpoint the
# README documents, plus the input validation that unit tests cannot reach
# because it lives in the request handlers rather than in the engine.
#
# That gap is not hypothetical. `floor=nan` reached a NaN-to-integer conversion
# (undefined behaviour) through the handler, and no test in the suite could have
# caught it, because the suite never speaks HTTP.
#
#   Usage: scripts/smoke_test.sh [path-to-adserve_server] [port]
#
# Run it from the repository root: the server resolves data/ads.csv relative to
# the working directory.

set -euo pipefail

SERVER=${1:-./build/adserve_server}
PORT=${2:-8099}
BASE="http://127.0.0.1:${PORT}"

"$SERVER" --host 127.0.0.1 --port "$PORT" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    if curl -fsS "${BASE}/health" >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! curl -fsS "${BASE}/health" >/dev/null 2>&1; then
    echo "server never became healthy on ${BASE}"
    exit 1
fi

failures=0

# check <description> <expected substring> <actual response>
check() {
    if [[ "$3" == *"$2"* ]]; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1"
        echo "          expected to contain: $2"
        echo "          got:                 $3"
        failures=$((failures + 1))
    fi
}

# ad <query string> <page text>
ad() {
    curl -sS -X POST "${BASE}/ad?$1" --data "$2"
}

SIP_PAGE='Should you start a SIP in mutual funds or pick stocks for your portfolio?'

check "relevant page fills with the SIP ad" '"ad_id":302' \
    "$(ad 'user_id=u1&url=https://news.example/sip&floor=2' "$SIP_PAGE")"

check "same URL again is a cache hit" '"cache_hit":true' \
    "$(ad 'user_id=u2&url=https://news.example/sip&floor=2' "$SIP_PAGE")"

check "unrelated page reports no_relevant_ads" '"reason":"no_relevant_ads"' \
    "$(ad 'user_id=u3&url=https://x.example/poetry' 'medieval poetry and cathedral architecture')"

check "floor above every bid reports below_floor" '"reason":"below_floor"' \
    "$(ad 'user_id=u4&url=https://y.example/z&floor=99' 'running shoes marathon')"

check "NaN floor is rejected, not converted" 'finite' \
    "$(ad 'user_id=u5&url=https://a.example/b&floor=nan' 'running shoes marathon')"

check "negative floor is rejected" 'finite' \
    "$(ad 'user_id=u6&url=https://a.example/b&floor=-1' 'running shoes marathon')"

check "missing user_id is rejected" 'user_id' \
    "$(curl -sS -X POST "${BASE}/ad?url=https://a.example/b" --data 'running shoes')"

check "click is recorded" '"recorded":true' \
    "$(curl -sS -X POST "${BASE}/click?ad_id=302")"

check "unknown ad id is rejected" 'unknown ad_id' \
    "$(curl -sS -X POST "${BASE}/click?ad_id=999999")"

check "stats reports the requests it served" '"requests":' \
    "$(curl -sS "${BASE}/stats")"

if (( failures > 0 )); then
    echo "${failures} smoke check(s) failed"
    exit 1
fi

echo "all smoke checks passed"
