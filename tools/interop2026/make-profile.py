#!/usr/bin/env python3
"""make-profile.py BASE_PROFILE.json HOSTS_FILE OUT.json [MCP_PORT MCP_TOKEN]

A Summit profile for running web-platform-tests:
- trusts wpt's pregenerated certificate (tools/certs/web-platform.test.pem)
  for every wpt host, as if the user had pressed Continue on each warning;
  the driver ignores acceptInsecureCerts (see the README);
- allows pop-ups for every wpt origin on the fixed ports (automation sessions
  still block pop-ups);
- turns off what would change test results or open extra windows: HTTPS-First
  (it upgrades wpt's plain HTTP ports), session restore, update checks,
  password saving and spell checking; autoplay is allowed, as in wpt runs of
  other browsers;
- turns Summit's MCP server on, for on-screen reftest screenshots.

BASE_PROFILE.json is a profile.json written by the Summit under test (start it
once with --profile on an empty folder), so the version and defaults match."""
import json, sys

# SHA-256 of tools/certs/web-platform.test.pem in wpt (valid until Oct 2027):
# openssl x509 -in tools/certs/web-platform.test.pem -noout -fingerprint -sha256
WPT_CERT_SHA256 = "bfc6f384d5e96ed58af80cae7a54e016c7191ed37e9c44b2fe4489810d383086"

base, hosts_file, out = sys.argv[1:4]
port = int(sys.argv[4]) if len(sys.argv) > 4 else 7841
token = sys.argv[5] if len(sys.argv) > 5 else "interop26"

profile = json.load(open(base))
hosts = [line.split()[1] for line in open(hosts_file)
         if line.strip() and not line.startswith("#")]
profile["trustedCertificates"] = [
    {"host": host, "sha256": WPT_CERT_SHA256, "subject": "web-platform.test", "added": 1}
    for host in hosts]
popups = {}
for host in hosts:
    for scheme, ports in (("http", (8000, 8001)), ("https", (8443, 8444, 9000))):
        for p in ports:
            popups[f"{scheme}://{host}:{p}"] = True
profile["sitePermissions"] = {"popups": popups}
profile.update(autoplay="allow", restoreSession=False, httpsFirst=False,
               savePasswords=False, spellChecking=False)
profile["updates"]["check"] = False
profile["mcp"] = {"enabled": True, "localOnly": False, "port": port, "token": token}
json.dump(profile, open(out, "w"), indent=1)
print(f"{len(hosts)} hosts, {len(popups)} pop-up origins -> {out}")
