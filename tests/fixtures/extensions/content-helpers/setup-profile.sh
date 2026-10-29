#!/bin/sh
# setup-profile.sh PROFILE: a Summit profile with the ext106 fixture installed (developer mode accepts its fingerprint).
P=${1:?profile}; rm -rf $P; U=http://192.168.1.200:8851/ext106/ext
mkdir -p $P/Extensions/packages/pkg-ext106/package; chmod 700 $P/Extensions $P/Extensions/packages
for f in manifest.json background.js content.js rules.json; do curl -sf -o $P/Extensions/packages/pkg-ext106/package/$f $U/$f || echo "failed $f"; done
cat > $P/Extensions/catalog.json <<'JSON'
{"format": 1, "extensions": [{"identifier": "ext106-fixture", "name": "ext106 fixture", "version": "1.0",
 "fingerprint": "0000000000000000000000000000000000000000000000000000000000000000", "package": "pkg-ext106",
 "enabled": true, "allow_file_urls": false, "allow_private_browsing": false, "installation_order": 1,
 "source": "firefox", "update_url": "", "auto_update": false, "updated": 0, "checked": 0,
 "permissions": ["declarativeNetRequest", "declarativeNetRequestFeedback", "tabs"], "origins": ["<all_urls>"]}]}
JSON
ls -la $P/Extensions $P/Extensions/packages/pkg-ext106/package
