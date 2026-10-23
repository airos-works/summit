// Logs bookmark events (the user's own changes included), then reads the
// tree, makes a folder with a bookmark in Other Bookmarks, finds, renames and
// moves it, checks the errors and removes the folder again. One console line
// per step: "[bookmarks] ...".
const log = message => console.log(`[bookmarks] ${message}`);
// Runs one step; argument errors throw synchronously, as in Chrome.
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
chrome.bookmarks.onCreated.addListener((id, node) => log(`onCreated ${id} ${node.type} "${node.title}" in ${node.parentId}@${node.index}`));
chrome.bookmarks.onRemoved.addListener((id, info) => log(`onRemoved ${id} from ${info.parentId}@${info.index} children=${(info.node.children || []).length}`));
chrome.bookmarks.onChanged.addListener((id, info) => log(`onChanged ${id} "${info.title}" ${info.url || ''}`));
chrome.bookmarks.onMoved.addListener((id, info) => log(`onMoved ${id} ${info.oldParentId}@${info.oldIndex} -> ${info.parentId}@${info.index}`));

(async () => {
    let folder, mark;
    await step('getTree', async () => {
        const [root] = await chrome.bookmarks.getTree();
        return `root=${root.id} ${root.children.map(node => `${node.id}:${node.title}(${node.children.length})`).join(',')}`;
    });
    await step('create', async () => {
        folder = await chrome.bookmarks.create({ parentId: '2', title: 'Fixture folder' });
        mark = await chrome.bookmarks.create({ parentId: folder.id, title: 'Fixture', url: 'https://example.com/?bookmarks' });
        return `folder=${folder.id} bookmark=${mark.id} parent=${mark.parentId}`;
    });
    await step('search text', async () => (await chrome.bookmarks.search('fixture example')).map(node => node.id).join(','));
    await step('search url', async () => (await chrome.bookmarks.search({ url: 'https://example.com/?bookmarks' })).length);
    await step('update', async () => (await chrome.bookmarks.update(mark.id, { title: 'Fixture renamed' })).title);
    await step('move', async () => {
        const moved = await chrome.bookmarks.move(mark.id, { parentId: '1', index: 0 });
        return `parent=${moved.parentId} index=${moved.index}`;
    });
    await step('getChildren', async () => (await chrome.bookmarks.getChildren('1'))[0].title);
    await step('getRecent', async () => (await chrome.bookmarks.getRecent(1))[0].title);
    await step('get list', async () => (await chrome.bookmarks.get([mark.id, folder.id])).map(node => node.title).join(','));
    await step('get one', async () => (await chrome.bookmarks.get(mark.id))[0].title);
    await step('getSubTree', async () => (await chrome.bookmarks.getSubTree(folder.id))[0].children.length);
    await step('move back', async () => (await chrome.bookmarks.move(mark.id, { parentId: folder.id })).parentId);
    await step('remove non-empty (must fail)', () => chrome.bookmarks.remove(folder.id));
    await step('rename bar (must fail)', () => chrome.bookmarks.update('1', { title: 'x' }));
    await step('move into itself (must fail)', () => chrome.bookmarks.move(folder.id, { parentId: folder.id }));
    await step('removeTree', async () => {
        await chrome.bookmarks.removeTree(folder.id);
        return `left ${(await chrome.bookmarks.search('Fixture')).length}`;
    });
    await step('missing (must fail)', () => chrome.bookmarks.get('999999'));
    await step('data: URL (must fail)', () => chrome.bookmarks.create({ title: 'x', url: 'data:text/plain,x' }));
})();
