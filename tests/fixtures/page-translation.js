// Test controls belong to this fixture, never to the browser implementation.
(() => {
    const ids = ['heading', 'intro', 'article', 'edited', 'excluded', 'noClass', 'editable', 'hidden', 'invisible', 'code', 'input', 'textarea', 'button'];
    const elements = Object.fromEntries(ids.map(id => [id, document.getElementById(id)]));
    const originals = Object.fromEntries(ids.map(id => [id, elements[id].textContent]));
    const link = document.getElementById('link');
    let clicks = 0;
    elements.button.addEventListener('click', () => ++clicks);
    // A website cannot intercept or replace the isolated controller.
    window.__summitTranslation = {sample() { throw Error('Website controller called'); }};
    window.translationFixture = {
        snapshot() {
            return {lang: document.documentElement.lang, clicks, linkRetained: document.getElementById('link') === link,
                elementsRetained: ids.every(id => document.getElementById(id) === elements[id]),
                input: elements.input.value, textarea: elements.textarea.value,
                texts: Object.fromEntries(ids.map(id => [id, elements[id].textContent])), originals,
                dynamic: document.getElementById('dynamic').textContent,
                pageControllerUnchanged: Object.keys(window.__summitTranslation).join() === 'sample'};
        },
        add() { document.getElementById('dynamic').textContent = 'Die neue Ausstellung ist für alle Besucher kostenlos.'; },
        moveAndEdit() {
            document.getElementById('moved').append(elements.intro);
            elements.edited.textContent = 'Website edit after translation.';
        },
        click() { elements.button.click(); }
    };
    if (new URL(location.href).searchParams.has('delayed')) {
        const content = document.createDocumentFragment();
        while (document.body.firstChild) content.append(document.body.firstChild);
        setTimeout(() => document.body.append(content), 1500);
    }
})();
