#!/usr/bin/env python3
"""Tiny streamable-HTTP MCP client.

  mcp.py [--url U --token T] list
  mcp.py call TOOL '{"json":"args"}'
  mcp.py sh 'command' [timeout_s]        (air/OS run_command)
  mcp.py put LOCAL REMOTE                 (air/OS file_put, base64)
  mcp.py get REMOTE LOCAL                 (air/OS file_get)

Defaults: the laptop (/mnt/Documents/LaptopMCP.txt), or MCP_URL / MCP_TOKEN.
"""
import base64, json, os, sys, time, urllib.error, urllib.request

def defaults():
    url = os.environ.get("MCP_URL")
    token = os.environ.get("MCP_TOKEN")
    if not url or not token:
        text = open("/mnt/Documents/LaptopMCP.txt").read()
        for line in text.splitlines():
            if line.startswith("URL:") and not url: url = line.split(None, 1)[1].strip()
            if line.startswith("Authorization:") and not token: token = line.split()[-1].strip()
    return url, token

class Client:
    def __init__(self, url, token):
        self.url, self.token, self.session, self.next = url, token, None, 1
    def rpc(self, method, params=None, notify=False, timeout=600):
        body = {"jsonrpc": "2.0", "method": method}
        if params is not None: body["params"] = params
        if not notify:
            body["id"] = self.next; self.next += 1
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.token: headers["Authorization"] = "Bearer " + self.token
        if self.session: headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(self.url, json.dumps(body).encode(), headers)
        for attempt in range(6):
            try:
                resp = urllib.request.urlopen(req, timeout=timeout)
                break
            except urllib.error.URLError as error:
                if attempt == 5 or not isinstance(error.reason, (ConnectionRefusedError, OSError)): raise
                time.sleep(5)
        with resp:
            sid = resp.headers.get("Mcp-Session-Id")
            if sid: self.session = sid
            data = resp.read().decode("utf-8", "replace")
            ctype = resp.headers.get("Content-Type", "")
        if notify or not data.strip(): return None
        if "text/event-stream" in ctype:
            for line in data.splitlines():
                if line.startswith("data:"):
                    msg = json.loads(line[5:])
                    if "id" in msg: data = msg; break
            else: return None
        else:
            data = json.loads(data)
        if "error" in data: raise RuntimeError(json.dumps(data["error"]))
        return data.get("result")
    def init(self):
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "mcp.py", "version": "1"}})
        try: self.rpc("notifications/initialized", notify=True)
        except Exception: pass
        return self
    def call(self, tool, args, timeout=600):
        return self.rpc("tools/call", {"name": tool, "arguments": args}, timeout=timeout)

def text_of(result):
    out = []
    for item in (result or {}).get("content", []):
        if item.get("type") == "text": out.append(item["text"])
        else: out.append("[%s %s]" % (item.get("type"), item.get("mimeType", "")))
    return "\n".join(out)

def main(argv):
    url = token = None
    while argv and argv[0].startswith("--"):
        if argv[0] == "--url": url = argv[1]
        elif argv[0] == "--token": token = argv[1]
        argv = argv[2:]
    if not url or not token:
        url, token = defaults()
    c = Client(url, token).init()
    cmd = argv[0]
    if cmd == "list":
        for t in c.rpc("tools/list")["tools"]:
            print(t["name"], "-", (t.get("description") or "").splitlines()[0][:150])
    elif cmd == "schema":
        for t in c.rpc("tools/list")["tools"]:
            if len(argv) < 2 or t["name"] == argv[1]: print(json.dumps(t, indent=1))
    elif cmd == "call":
        r = c.call(argv[1], json.loads(argv[2]) if len(argv) > 2 else {})
        if os.environ.get("RAW"): print(json.dumps(r, indent=1))
        else:
            print(text_of(r))
            for item in (r or {}).get("content", []):
                if item.get("type") == "image" and os.environ.get("IMAGE_OUT"):
                    open(os.environ["IMAGE_OUT"], "wb").write(base64.b64decode(item["data"]))
    elif cmd == "sh":
        timeout = int(argv[2]) if len(argv) > 2 else 60
        r = c.call("run_command", {"command": argv[1], "timeout_ms": timeout * 1000}, timeout=timeout + 30)
        print(text_of(r))
    elif cmd == "put":
        data = open(argv[1], "rb").read()
        r = c.call("file_put", {"path": argv[2], "base64": base64.b64encode(data).decode(), "mode": argv[3] if len(argv) > 3 else "0644"})
        print(text_of(r))
    elif cmd == "get":
        r = c.call("file_get", {"path": argv[1], "encoding": "base64", "max_bytes": 64 << 20})
        t = text_of(r)
        try:
            j = json.loads(t)
            payload = j.get("base64") or j.get("content_base64") or j.get("data")
            open(argv[2], "wb").write(base64.b64decode(payload))
            print("saved", argv[2])
        except Exception:
            print(t[:2000])
    else:
        print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
