#!/usr/bin/env python3
"""Claude Code status line hook: saves the plan's rate-limit numbers for the dashboard's
system page (~/.cache/maodie/claude_usage.json) and prints a short status line.
Claude Code passes the session JSON on stdin; nothing secret is in it or stored."""
import json
import os
import sys
import time

d = json.load(sys.stdin)
rl = d.get("rate_limits")
if rl:
    path = os.path.expanduser("~/.cache/maodie/claude_usage.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w") as f:
        json.dump({"rate_limits": rl, "saved_at": time.time()}, f)
    os.replace(path + ".tmp", path)
week = ((rl or {}).get("seven_day") or {}).get("used_percentage")
model = (d.get("model") or {}).get("display_name", "")
print(f"{model}  本周已用 {week:.0f}%" if week is not None else model)
