// Runs only in Summit's persistent isolated world. Never add identifiers or
// translation state to the website's globals, attributes or event handlers.
(() => {
    if (globalThis.__summitTranslation) return;
    const excluded = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'TEXTAREA',
        'INPUT', 'SELECT', 'OPTION', 'PRE', 'CODE', 'KBD', 'SAMP', 'SVG', 'MATH',
        'IFRAME', 'OBJECT', 'EMBED', 'CANVAS', 'VIDEO', 'AUDIO']);
    const blocks = new Set(['P', 'DIV', 'SECTION', 'ARTICLE', 'MAIN', 'ASIDE',
        'HEADER', 'FOOTER', 'NAV', 'LI', 'DT', 'DD', 'H1', 'H2', 'H3', 'H4',
        'H5', 'H6', 'TD', 'TH', 'CAPTION', 'FIGCAPTION', 'BLOCKQUOTE', 'BUTTON', 'LABEL']);
    const encoder = new TextEncoder();
    const entries = new Map();
    let byNode = new WeakMap();
    let nextID = 0, epoch = 0, active = false, dirty = true, observer = null;
    let source = '', target = '', originalLang = null, changedLang = false;
    let totalBytes = 0, translatedCount = 0, truncated = false;
    const limits = {visit: 50000, entries: 20000, retained: 4000000, node: 50000, batch: 24000, group: 6000};
    const escape = text => text.replace(/[&<>"']/g,
        c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
    const supported = () => ['http:', 'https:'].includes(location.protocol)
        && ['text/html', 'application/xhtml+xml'].includes(document.contentType) && !!document.body;

    function skip(element) {
        if (excluded.has(element.tagName) || element.isContentEditable
            || element.translate === false || element.classList.contains('notranslate')
            || element.hidden || element.getAttribute('aria-hidden') === 'true') return true;
        const style = getComputedStyle(element);
        return style.display === 'none' || style.visibility === 'hidden' || style.visibility === 'collapse';
    }

    // A bounded preorder traversal. Do not read control values, markup,
    // credentials or text belonging to an explicitly untranslated subtree.
    function walk(visit) {
        if (!supported()) return false;
        const stack = [{node: document.body, block: document.body}];
        let visited = 0;
        while (stack.length) {
            if (++visited > limits.visit) return true;
            const {node, block} = stack.pop();
            if (node.nodeType === Node.TEXT_NODE) {
                if (/\p{L}/u.test(node.data) && visit(node, block) === false) return true;
                continue;
            }
            if (node.nodeType !== Node.ELEMENT_NODE || skip(node)) continue;
            const nextBlock = blocks.has(node.tagName) ? node : block;
            for (let child = node.lastChild; child; child = child.previousSibling) {
                if (stack.length + visited >= limits.visit) return true;
                if (child.nodeType === Node.ELEMENT_NODE || child.nodeType === Node.TEXT_NODE)
                    stack.push({node: child, block: nextBlock});
            }
        }
        return false;
    }

    function state() {
        return {epoch, active, source, target, translated: translatedCount, truncated};
    }

    function sample() {
        const parts = [];
        let bytes = 0;
        walk(node => {
            const value = node.data.slice(0, 65536).trim();
            // Sampling ends at a whole code point and never exceeds the
            // native detector's 64 KiB UTF-8 input limit.
            const remaining = 65536 - bytes - (parts.length ? 1 : 0);
            if (remaining <= 0) return false;
            let text = value;
            let encoded = encoder.encode(text);
            if (encoded.length > remaining) {
                text = new TextDecoder('utf-8', {fatal: false}).decode(encoded.slice(0, remaining));
                if (text.endsWith('\uFFFD')) text = text.slice(0, -1);
                encoded = encoder.encode(text);
            }
            if (text) { parts.push(text); bytes += encoded.length + (parts.length > 1 ? 1 : 0); }
            return bytes < 65532;
        });
        return {url: document.URL, text: parts.join('\n'), languages: [...navigator.languages], ...state()};
    }

    function begin(from, to) {
        if (!supported() || typeof from !== 'string' || typeof to !== 'string'
            || !/^[a-z]{2,3}(?:-[A-Za-z]{4})?$/.test(from) || !/^[a-z]{2,3}(?:-[A-Za-z]{4})?$/.test(to))
            throw Error('This document cannot be translated.');
        restore();
        active = true; source = from; target = to; dirty = true;
        originalLang = document.documentElement.getAttribute('lang');
        observer = new MutationObserver(changes => {
            if (!active) return;
            for (const change of changes) {
                if (change.type === 'characterData') {
                    const entry = byNode.get(change.target);
                    if (entry && change.target.data === entry.translated) continue;
                }
                dirty = true;
                break;
            }
        });
        observer.observe(document.body, {childList: true, characterData: true, subtree: true,
            attributes: true, attributeFilter: ['class', 'style', 'hidden', 'aria-hidden', 'translate', 'contenteditable']});
        return state();
    }

    function collect() {
        if (!active || !dirty || !supported()) return {groups: [], ...state()};
        const groups = [];
        let group = null, bytes = 0, stopped = false;
        const finish = () => {
            if (group) { groups.push({html: '<p>' + group.html + '</p>', ids: group.ids}); group = null; }
        };
        const bounded = walk((node, block) => {
            const text = node.data;
            let entry = byNode.get(node);
            if (entry && (entry.pending || text === entry.translated)) return true;
            const length = encoder.encode(text).length;
            if (length > limits.node) { truncated = true; return true; }
            if (bytes + length > limits.batch && bytes) { stopped = true; return false; }
            if (!entry) {
                if (entries.size >= limits.entries || totalBytes + length > limits.retained) { truncated = true; return false; }
                entry = {id: String(++nextID), node, original: text, translated: null, pending: false};
                entries.set(entry.id, entry); byNode.set(node, entry); totalBytes += length;
            } else {
                // The website edited this text after translation; use that
                // new text as the original, never overwrite the site's edit
                // with a translation requested for its previous contents.
                totalBytes -= encoder.encode(entry.original).length;
                entry.original = text; entry.translated = null;
                totalBytes += length;
                if (totalBytes > limits.retained) { truncated = true; return false; }
            }
            if (group && (group.block !== block || group.bytes + length > limits.group)) finish();
            if (!group && groups.length >= 128) { stopped = true; return false; }
            if (!group) group = {block, ids: [], html: '', bytes: 0};
            entry.pending = true;
            group.ids.push(entry.id);
            group.html += '<span data-summit-id="' + entry.id + '">' + escape(text) + '</span>';
            group.bytes += length; bytes += length;
            return true;
        });
        finish();
        // Pending text is ignored on another collect call. A completed batch
        // makes the document dirty again so remaining/new nodes get a turn.
        dirty = stopped;
        if (bounded && !stopped) truncated = true;
        return {groups, ...state()};
    }

    function apply(resultEpoch, values) {
        if (!active || resultEpoch !== epoch || !Array.isArray(values)) return {...state(), applied: 0};
        let applied = 0;
        for (const value of values) {
            if (!value || typeof value.id !== 'string' || typeof value.text !== 'string' || value.text.length > 200000) continue;
            const entry = entries.get(value.id);
            if (!entry || !entry.pending) continue;
            entry.pending = false;
            const node = entry.node;
            if (!node.isConnected || node.ownerDocument !== document || node.data !== entry.original) continue;
            let eligible = true, depth = 0;
            for (let parent = node.parentElement; parent; parent = parent.parentElement) {
                if (++depth > 1000 || skip(parent)) { eligible = false; break; }
                if (parent === document.body) break;
            }
            if (!eligible) continue;
            // Translation results are always text, never parsed as markup in
            // the website's document. The node and its parent stay intact.
            entry.translated = value.text;
            node.data = value.text;
            ++applied; ++translatedCount;
        }
        if (applied && !changedLang) {
            document.documentElement.setAttribute('lang', target);
            changedLang = true;
        }
        dirty = true;
        return {...state(), applied};
    }

    function fail(resultEpoch, ids) {
        if (resultEpoch !== epoch || !Array.isArray(ids)) return state();
        for (const id of ids) { const entry = entries.get(id); if (entry) entry.pending = false; }
        dirty = true;
        return state();
    }

    function restore() {
        ++epoch; active = false;
        if (observer) observer.disconnect();
        observer = null;
        let restored = 0;
        for (const entry of entries.values()) {
            if (entry.translated === null || entry.node.ownerDocument !== document
                || entry.node.data !== entry.translated) continue;
            entry.node.data = entry.original;
            ++restored;
        }
        if (changedLang && document.documentElement.getAttribute('lang') === target) {
            if (originalLang === null) document.documentElement.removeAttribute('lang');
            else document.documentElement.setAttribute('lang', originalLang);
        }
        entries.clear(); byNode = new WeakMap(); totalBytes = 0;
        changedLang = false; translatedCount = 0; truncated = false; dirty = true;
        return {...state(), restored};
    }

    globalThis.__summitTranslation = Object.freeze({sample, begin, collect, apply, fail, restore, state});
})();
