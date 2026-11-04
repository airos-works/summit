#!/bin/bash
# sp3.sh BUILD RUNS: Speedometer 3.1 in the test copy BUILD on the test machine
# ($SUMMIT_REMOTE_DIR/BUILD), RUNS times, each in a freshly started Summit with
# a fresh profile; prints each score and the mean. Uses Summit's MCP server
# (the seed profile turns it on: port 7841, token interop26). SP3_ENV adds
# NAME=VALUE settings for that Summit.
KIT=$(cd "$(dirname "$0")" && pwd)
BUILD=$1; RUNS=${2:-3}
SSH=${SUMMIT_SSH:-ssh -F /mnt/HaikuWork/x399/ssh/config ws-haiku}
DIR=${SUMMIT_REMOTE_DIR:-/boot/home/interop}
HOST=${SUMMIT_HOST:-192.168.1.247}
X=$DIR/$BUILD
scores=()
for run in $(seq "$RUNS"); do
	$SSH "for sig in TERM KILL; do kill -\$sig \$(ps | grep -E '$DIR/x[a-z]*/(apps/Summit|lib/summit-webkit/)' | grep -v grep | awk '{print \$(NF-3)}') 2>/dev/null; sleep 2; done; rm -rf $DIR/pbench; mkdir -p $DIR/pbench; cp $DIR/pbench.seed $DIR/pbench/profile.json" < /dev/null
	# -f: the started Summit keeps the session open; do not wait for it.
	${SSH/ssh /ssh -f -n } "cd $X && WEBKIT_EXEC_PATH=$X/lib/summit-webkit exec env ${SP3_ENV:-} $X/apps/Summit --profile $DIR/pbench 'https://browserbench.org/Speedometer3.1/?startAutomatically' > $DIR/sp3-$BUILD.log 2>&1 < /dev/null"
	# SP3_PROFILE=SECONDS: a system-wide sample of that long, 40 s into the run
	# ($DIR/sp3-BUILD-RUN.prof on the test machine; the score is then skewed).
	[[ -z ${SP3_PROFILE:-} ]] || ${SSH/ssh /ssh -f -n } "sleep 40; profile -a -o $DIR/sp3-$BUILD-$run.prof sleep $SP3_PROFILE > /dev/null 2>&1 < /dev/null"
	score=$(KIT=$KIT python3 - "$HOST" <<'EOF'
import sys, time, json, socket
socket.setdefaulttimeout(40)
sys.path.insert(0, __import__('os').environ['KIT'])
import mcp
_rpc = mcp.Client.rpc
mcp.Client.rpc = lambda self, method, params=None, notify=False, timeout=40: _rpc(self, method, params, notify, min(timeout, 40))
url = f"http://{sys.argv[1]}:7841/mcp"
deadline = time.time() + 1500
client = None
while time.time() < deadline:
    time.sleep(10)
    try:
        if client is None:
            client = mcp.Client(url, "interop26").init()
        # The tab's address only (browser state, no inspector): Speedometer
        # moves to #summary when it is done. Evaluating in the page while it
        # runs would disturb the run.
        tabs = json.loads(mcp.text_of(client.call("tabs", {}, timeout=30)))
        done = any(t.get("url", "").endswith("#summary") for w in tabs.get("windows", []) for t in w.get("tabs", []))
        if not done:
            continue
        time.sleep(2)
        text = mcp.text_of(client.call("evaluate", {"expression": "(() => { const e = document.querySelector('#result-number'); return e ? e.textContent.trim() : ''; })()"}, timeout=30))
    except Exception:
        client = None
        continue
    value = text.strip().strip('"')
    # A tool error comes back as text too: only a score counts.
    if __import__('re').fullmatch(r"\d+(\.\d+)?", value):
        print(value)
        sys.exit(0)
print("timeout")
EOF
)
	echo "$BUILD run $run: $score"
	scores+=("$score")
done
$SSH "for sig in TERM KILL; do kill -\$sig \$(ps | grep -E '$DIR/x[a-z]*/(apps/Summit|lib/summit-webkit/)' | grep -v grep | awk '{print \$(NF-3)}') 2>/dev/null; sleep 2; done; true" < /dev/null
python3 -c "
import sys
v=[float(x) for x in sys.argv[1:] if x.replace('.','',1).isdigit()]
print('$BUILD mean', round(sum(v)/len(v),2) if v else 'n/a', 'of', len(v), 'runs:', v)" "${scores[@]}"
