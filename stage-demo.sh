#!/usr/bin/env bash
# Put four invented sessions on the device, one per state, for screenshots.
#
#   ./stage-demo.sh [console-ip]      # default: 192.168.1.200
#   ./stage-demo.sh --clear           # remove them again
#
# Every name, path and prompt here is fabricated. Nothing real reaches a
# published photograph, which is the whole point: the card renders prompt text
# and assistant replies, so a candid shot of a working device publishes
# whatever happened to be on it.
#
# Session ids are the fixed strings below rather than UUIDs, so re-running
# updates the same four arcs instead of stacking new ones.

set -euo pipefail

IP="${1:-192.168.1.200}"
[[ "${1:-}" == "--clear" ]] && { curl -sf -X POST "http://192.168.1.200/clear"; echo; exit 0; }

U="http://$IP"
H='Content-Type: application/json'

post() { curl -sf --max-time 5 -X POST "$1" -H "$H" -d "$2" || { echo "error: $IP unreachable" >&2; exit 1; }; }

curl -sf --max-time 5 -X POST "$U/clear" >/dev/null

# 1. working - cyan comet. Labelled, to show ?label= naming an arc.
post "$U/hook?label=ring%20firmware" '{
  "session_id":"demo-working",
  "hook_event_name":"UserPromptSubmit",
  "cwd":"/home/dev/projects/ring-firmware",
  "user_prompt":"align the screen arcs with the physical ring so they point at the same LEDs"}'

# 2. background - violet comet, with agents outstanding. Two subagent events,
#    because one is enough to open the window but two shows a pending count.
post "$U/hook" '{
  "session_id":"demo-background",
  "hook_event_name":"UserPromptSubmit",
  "cwd":"/home/dev/projects/board-update",
  "user_prompt":"draft the Q3 board update covering revenue, headcount and roadmap"}'
post "$U/hook" '{"session_id":"demo-background","hook_event_name":"SubagentStart"}'
post "$U/hook" '{"session_id":"demo-background","hook_event_name":"TaskCreated"}'

# 3. input - hot pink slow pulse. Has a reply, so the card shows "said: ...".
post "$U/hook" '{
  "session_id":"demo-input",
  "hook_event_name":"UserPromptSubmit",
  "cwd":"/home/dev/projects/inbox-triage",
  "user_prompt":"sort the support inbox by topic and flag anything urgent"}'
post "$U/hook" '{
  "session_id":"demo-input",
  "hook_event_name":"Stop",
  "cwd":"/home/dev/projects/inbox-triage",
  "last_assistant_message":"Sorted 312 messages into eight topics and flagged four as urgent. Two look like duplicates."}'

# 4. attention - pink-red fast pulse, the state the whole device exists for.
post "$U/hook" '{
  "session_id":"demo-attention",
  "hook_event_name":"UserPromptSubmit",
  "cwd":"/home/dev/projects/site-rebuild",
  "user_prompt":"rebuild the docs site and deploy it to staging"}'
post "$U/hook" '{
  "session_id":"demo-attention",
  "hook_event_name":"Notification",
  "notification_type":"permission_prompt",
  "cwd":"/home/dev/projects/site-rebuild"}'

echo "staged on $IP:"
curl -sf --max-time 5 "$U/" | python3 -c "
import json, sys
for s in json.load(sys.stdin)['sessions']:
    agents = f\"  {s['pending']} agents\" if s['pending'] else ''
    print(f\"  {s['state']:11} {s['name']:16}{agents}\")
    print(f\"              \\\"{s['topic']}\\\"\")
"
cat <<'EOF'

Two shots worth taking:

  1. No card open - four arcs and the centre summary.
  2. Tap the pink-red arc - the card names it and says what it is blocked on.
     The card dismisses itself after 15 s, so tap and shoot.

./stage-demo.sh --clear   removes them; live sessions repopulate on their own.
EOF
