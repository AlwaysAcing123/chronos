#!/bin/bash
# Starts an isolated paper-trading server on the live Alpaca IEX stream,
# with model deployments enabled, for validate.py. Simulated trading only:
# the server consumes market data and never sends an order anywhere.
#
# Credentials come from the macOS Keychain straight into the server's
# environment. They are never echoed, logged, or passed as arguments. Do
# not add `set -x` to this script.
#
#   CHRONOS_VALIDATION_DATABASE_URL  an isolated, migrated database (required)
#   CHRONOS_VALIDATION_EVIDENCE      a directory for the server log (required)
#   CHRONOS_VALIDATION_PORT          default 8083; must be free
#   CHRONOS_VALIDATION_SERVER        default build-full/chronos_paper_trading_server
#   CHRONOS_VALIDATION_INSTRUMENT    default AAPL
#   CHRONOS_VALIDATION_STREAM_ID     default alpaca-iex-validation
#   CHRONOS_VALIDATION_ADOPT_LIVE_STREAM_HISTORY
#                                    1 to continue a stream whose history
#                                    predates migration 006 (ADR 0020), once
set -euo pipefail

: "${CHRONOS_VALIDATION_DATABASE_URL:?set it to an isolated, migrated database}"
: "${CHRONOS_VALIDATION_EVIDENCE:?set it to a directory for the server log}"
port="${CHRONOS_VALIDATION_PORT:-8083}"
server="${CHRONOS_VALIDATION_SERVER:-build-full/chronos_paper_trading_server}"
instrument="${CHRONOS_VALIDATION_INSTRUMENT:-AAPL}"
stream="${CHRONOS_VALIDATION_STREAM_ID:-alpaca-iex-validation}"
adopt=()
if [[ "${CHRONOS_VALIDATION_ADOPT_LIVE_STREAM_HISTORY:-}" == "1" ]]; then
    adopt=(--adopt-live-stream-history)
fi

if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "port $port is in use; choose another with CHRONOS_VALIDATION_PORT" >&2
    exit 2
fi
# Alpaca's free plan allows one market-data connection per account. Never
# open a second one, and never stop someone else's to make room.
if pgrep -f -- "--alpaca-iex" >/dev/null 2>&1; then
    echo "another local server already holds an Alpaca IEX connection; coordinate with its owner" >&2
    exit 2
fi
for service in com.chronos.alpaca.api-key-id com.chronos.alpaca.api-secret; do
    if ! security find-generic-password -a chronos -s "$service" >/dev/null 2>&1; then
        echo "the Keychain has no $service entry for account chronos" >&2
        exit 2
    fi
done
mkdir -p "$CHRONOS_VALIDATION_EVIDENCE"

APCA_API_KEY_ID="$(security find-generic-password -a chronos -s com.chronos.alpaca.api-key-id -w)" \
APCA_API_SECRET_KEY="$(security find-generic-password -a chronos -s com.chronos.alpaca.api-secret -w)" \
CHRONOS_POSTGRES_URL="$CHRONOS_VALIDATION_DATABASE_URL" \
    exec "$server" \
        --stream-id "$stream" \
        --instrument "$instrument" \
        --alpaca-iex \
        ${adopt[@]+"${adopt[@]}"} \
        --port "$port" \
        --model-deployments \
        --research-python python/.venv/bin/python \
        --research-python-root python \
        --research-artifacts data \
        >"$CHRONOS_VALIDATION_EVIDENCE/server.log" 2>&1
