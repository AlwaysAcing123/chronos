#!/bin/bash
# Counts the evidence files that contain either Alpaca credential, without
# printing a value: the values reach grep through a pipe, never an argument
# or a file. Prints 0 only after a completed clean scan. Nonzero exit means
# a match or a failed scan; neither is evidence that credentials are absent.
set -euo pipefail
directory="${1:?usage: scan_evidence.sh <evidence directory>}"
if [[ ! -d "$directory" || ! -r "$directory" ]]; then
    echo "Evidence directory is missing or unreadable; scan not completed." >&2
    exit 2
fi
if ! api_key=$(security find-generic-password -a chronos -s com.chronos.alpaca.api-key-id -w 2>/dev/null) ||
   ! api_secret=$(security find-generic-password -a chronos -s com.chronos.alpaca.api-secret -w 2>/dev/null) ||
   [[ -z "$api_key" || -z "$api_secret" ]]; then
    echo "Both credentials must be available in Keychain; scan not completed." >&2
    exit 2
fi
if matches=$(printf '%s\n' "$api_key" "$api_secret" | grep -rlF -f /dev/stdin -- "$directory"); then
    printf '%s\n' "$matches" | wc -l | tr -d ' '
    exit 1
else
    scan_status=$?
    if [[ "$scan_status" -ne 1 ]]; then
        echo "Evidence scan failed; no clean result is available." >&2
        exit 2
    fi
    echo 0
fi
