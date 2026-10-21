// This page belongs to Summit, in a private context with a self-only CSP.
// The native model cache serves pinned, verified files through the same
// origin. Source page text is never sent to a network service.
(() => {
    let worker = null, serial = 0, models = [], ready = false;
    const pending = new Map();
    const send = value => window.webkit.messageHandlers.summitTranslation.postMessage(JSON.stringify(value));
    function stop(error = Error('Translation stopped.')) {
        if (worker) worker.terminate();
        worker = null; ready = false; models = [];
        for (const {reject, timer} of pending.values()) { clearTimeout(timer); reject(error); }
        pending.clear();
    }
    function call(name, ...args) {
        if (!worker) return Promise.reject(Error('The translation worker is not running.'));
        return new Promise((resolve, reject) => {
            const id = ++serial;
            const timer = setTimeout(() => stop(Error('Local translation took too long.')), 120000);
            pending.set(id, {resolve, reject, timer});
            try { worker.postMessage({id, name, args}); }
            catch (error) { pending.delete(id); clearTimeout(timer); reject(error); }
        });
    }
    async function read(file) {
        // Only browser-supplied cache resources are ever fetched here.
        if (!file || !/^[0-9a-f]{64}$/.test(file.sha256)) throw Error('Invalid translation model.');
        const response = await fetch(new URL('models/' + file.sha256, location.href), {credentials: 'omit'});
        if (!response.ok) throw Error('Could not read the translation model.');
        const bytes = await response.arrayBuffer();
        if (bytes.byteLength !== file.size) throw Error('The translation model has the wrong size.');
        return bytes;
    }
    async function configure(specification) {
        stop();
        if (!Array.isArray(specification) || specification.length < 1 || specification.length > 2)
            throw Error('This language pair is unavailable.');
        worker = new Worker(new URL('bergamot/translator-worker.js', location.href));
        worker.addEventListener('error', event => { event.preventDefault(); stop(Error('The local translation worker failed.')); });
        worker.addEventListener('messageerror', () => stop(Error('The local translation worker returned invalid data.')));
        worker.addEventListener('message', ({data}) => {
            const request = pending.get(data?.id);
            if (!request) return;
            pending.delete(data.id); clearTimeout(request.timer);
            if (data.error !== undefined) request.reject(Error(data.error.message || 'Local translation failed.'));
            else request.resolve(data.result);
        });
        await call('initialize', {useNativeIntGemm: false, cacheSize: 0});
        for (const model of specification) {
            const buffers = {};
            for (const file of model.files) buffers[file.part] = await read(file);
            const vocabs = buffers.vocab ? [buffers.vocab] : [buffers.srcvocab, buffers.trgvocab];
            if (!buffers.model || !buffers.lex || vocabs.some(buffer => !buffer)) throw Error('Incomplete translation model.');
            const pair = {from: model.from, to: model.to};
            await call('loadTranslationModel', pair, {model: buffers.model, shortlist: buffers.lex,
                vocabs, config: model.config || {}});
            models.push(pair);
        }
        ready = true;
        return {ready};
    }

    function decode(html, ids) {
        if (typeof html !== 'string' || html.length > 500000 || !ids.length) throw Error('Invalid translation output.');
        // Parse only in this private document. None of the output's markup is
        // inserted into a website; the response contains plain text per node.
        const template = document.createElement('template');
        template.innerHTML = html;
        if ([...template.content.querySelectorAll('*')].some(node => !['P', 'SPAN'].includes(node.tagName)))
            throw Error('Unexpected markup in translation output.');
        const spans = [...template.content.querySelectorAll('[data-summit-id]')];
        if (spans.length !== ids.length || spans.some((span, i) => span.getAttribute('data-summit-id') !== ids[i]))
            throw Error('The translator could not preserve this paragraph.');
        const values = new Map(ids.map(id => [id, '']));
        const walker = document.createTreeWalker(template.content, NodeFilter.SHOW_TEXT);
        let previous = ids[0];
        while (walker.nextNode()) {
            const node = walker.currentNode;
            const span = node.parentElement?.closest('[data-summit-id]');
            const id = span ? span.getAttribute('data-summit-id') : previous;
            if (!values.has(id)) throw Error('Unknown translation node.');
            values.set(id, values.get(id) + node.data);
            previous = id;
        }
        return ids.map(id => ({id, text: values.get(id)}));
    }

    async function translate(epoch, groups) {
        if (!ready || !Array.isArray(groups) || groups.length > 256) throw Error('The translator is not ready.');
        let values = [];
        // Small batches keep cancellation responsive and bound worker memory.
        for (let offset = 0; offset < groups.length; offset += 8) {
            const batch = groups.slice(offset, offset + 8);
            const output = await call('translate', {models, texts: batch.map(group => ({text: group.html, html: true, qualityScores: false}))});
            if (!Array.isArray(output) || output.length !== batch.length) throw Error('Incomplete translation output.');
            for (let i = 0; i < batch.length; ++i) values.push(...decode(output[i].target.text, batch[i].ids));
        }
        return {epoch, values};
    }
    globalThis.summitTranslator = Object.freeze({configure, translate, stop});
    send({ready: true}).catch(() => stop());
})();
