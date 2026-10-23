// Logs bookmark events (the user's own changes included), then reads the
// tree, makes a folder with a bookmark in Other Bookmarks, finds, renames and
// moves it, checks the errors and removes the folder again. One console line
// per step: "[bookmarks] ...".
const log = message => console.log(`[bookmarks] ${message}`);
chrome.bookmarks.onCreated.addListener((id, node) => log(`onCreated ${id} ${node.type} "${node.title}" in ${node.parentId}@${node.index}`));
chrome.bookmarks.onRemoved.addListener((id, info) => log(`onRemoved ${id} from ${info.parentId}@${info.index} children=${(info.node.children || []).length}`));
chrome.bookmarks.onChanged.addListener((id, info) => log(`onChanged ${id} "${info.title}" ${info.url || ''}`));
chrome.bookmarks.onMoved.addListener((id, info) => log(`onMoved ${id} ${info.oldParentId}@${info.oldIndex} -> ${info.parentId}@${info.index}`));

(async () => {
    try {
        const [root] = await chrome.bookmarks.getTree();
        log(`getTree root=${root.id} folders=${root.children.map(folder => `${folder.id}:${folder.title}(${folder.children.length})`).join(',')}`);
        const folder = await chrome.bookmarks.create({ parentId: '2', title: 'Fixture folder' });
        const mark = await chrome.bookmarks.create({ parentId: folder.id, title: 'Fixture', url: 'https://example.com/?bookmarks' });
        log(`create folder=${folder.id} bookmark=${mark.id} parent=${mark.parentId}`);
        log(`search ${(await chrome.bookmarks.search('fixture example')).map(node => node.id).join(',')}`);
        log(`search url ${(await chrome.bookmarks.search({ url: 'https://example.com/?bookmarks' })).length}`);
        const renamed = await chrome.bookmarks.update(mark.id, { title: 'Fixture renamed' });
        log(`update title="${renamed.title}"`);
        const moved = await chrome.bookmarks.move(mark.id, { parentId: '1', index: 0 });
        log(`move parent=${moved.parentId} index=${moved.index}`);
        log(`getChildren 1 first=${(await chrome.bookmarks.getChildren('1'))[0].title}`);
        log(`getRecent ${(await chrome.bookmarks.getRecent(1))[0].title}`);
        log(`get ${(await chrome.bookmarks.get([mark.id, folder.id])).map(node => node.title).join(',')}`);
        await chrome.bookmarks.move(mark.id, { parentId: folder.id });
        await chrome.bookmarks.remove(folder.id).catch(error => log(`remove non-empty rejected: ${error.message}`));
        await chrome.bookmarks.update('1', { title: 'x' }).catch(error => log(`root rename rejected: ${error.message}`));
        await chrome.bookmarks.move(folder.id, { parentId: folder.id }).catch(error => log(`move into itself rejected: ${error.message}`));
        await chrome.bookmarks.removeTree(folder.id);
        log(`removeTree, search ${(await chrome.bookmarks.search('Fixture')).length}`);
        await chrome.bookmarks.get('999999').catch(error => log(`missing rejected: ${error.message}`));
    } catch (error) {
        log(`error ${error.message}`);
    }
})();
