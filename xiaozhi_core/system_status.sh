#!/bin/sh

# Small, dependency-free status tool for the QZdesk MCP gateway.
set -u

load_average=$(awk '{print $1" "$2" "$3}' /proc/loadavg 2>/dev/null || printf 'unavailable')
memory=$(awk '/MemTotal:/ {total=$2} /MemAvailable:/ {available=$2} END {
    if (total > 0) printf "%d%%", (100 * (total - available) / total)
    else printf "unavailable"
}' /proc/meminfo 2>/dev/null)
disk=$(df -P / 2>/dev/null | awk 'NR==2 {print $5}')
uptime=$(awk '{printf "%s seconds", $1}' /proc/uptime 2>/dev/null || printf 'unavailable')

printf '{"status":"ok","load":"%s","memory_used":"%s","disk_used":"%s","uptime":"%s"}\n' \
    "$load_average" "${memory:-unavailable}" "${disk:-unavailable}" "$uptime"
