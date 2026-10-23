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
    // A port: two pings, then "bye"; the receiver disconnects.
    const port = chrome.runtime.connect(RECEIVER, { name: 'fixture' });
    port.onMessage.addListener(message => log(`port got ${message}`));
    port.onDisconnect.addListener(() => log(`port disconnected${chrome.runtime.lastError ? ': ' + chrome.runtime.lastError.message : ''}`));
    port.postMessage('ping 1');
    port.postMessage('ping 2');
    setTimeout(() => port.postMessage('bye'), 1500);
    // The closed receiver refuses the connection.
    const closed = chrome.runtime.connect(CLOSED, { name: 'refused' });
    closed.onDisconnect.addListener(() => log(`closed port disconnected${chrome.runtime.lastError ? ': ' + chrome.runtime.lastError.message : ''}`));
}, 3000);
