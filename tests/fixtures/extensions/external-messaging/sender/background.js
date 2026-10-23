// Three seconds after start: message the receiver (admits everyone), the closed
// receiver (admits one other ID) and an ID that is not installed.
// One console line per result: "[external-sender] ...".
const log = message => console.log(`[external-sender] ${message}`);
const RECEIVER = 'lbemamlibomhdpkllpeodnmpommkhbdl';
const CLOSED = 'dbjglklidijppgkmdpjggbmfnbmnghnb';
const MISSING = 'pppppppppppppppppppppppppppppppp';
setTimeout(async () => {
    for (const [name, id] of [['receiver', RECEIVER], ['closed', CLOSED], ['missing', MISSING]]) {
        try {
            const reply = await chrome.runtime.sendMessage(id, { hello: name });
            log(`${name} replied ${JSON.stringify(reply)} (sender ${chrome.runtime.id})`);
        } catch (error) {
            log(`${name} rejected: ${error.message}`);
        }
    }
}, 3000);
