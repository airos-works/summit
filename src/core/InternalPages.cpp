#include "InternalPages.h"
#include "Address.h"
#include "Favicon.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>

namespace summit {
static std::tm LocalTime(int64_t seconds)
{
    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm result { };
    localtime_r(&time, &result);
    return result;
}
static int DayKey(const std::tm& time) { return (time.tm_year + 1900) * 1000 + time.tm_yday; }

static std::string DateLabel(const std::tm& time)
{
    static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* months[] = { "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December" };
    return std::string(days[time.tm_wday % 7]) + ", " + std::to_string(time.tm_mday) + " "
        + months[time.tm_mon % 12] + " " + std::to_string(time.tm_year + 1900);
}

std::vector<HistoryDay> GroupHistoryByDay(const std::vector<PageRecord>& history, int64_t now)
{
    const std::tm today = LocalTime(now);
    // Step back a calendar day at noon, so daylight saving changes cannot skip one.
    std::tm previous = today;
    previous.tm_mday -= 1;
    previous.tm_hour = 12;
    previous.tm_min = previous.tm_sec = 0;
    previous.tm_isdst = -1;
    const std::tm yesterday = LocalTime(static_cast<int64_t>(std::mktime(&previous)));
    std::vector<HistoryVisit> visits;
    for (const auto& page : history) {
        visits.push_back({&page, page.visited});
        for (int64_t time : page.earlier) visits.push_back({&page, time});
    }
    // Untimed visits (0) last, in history's order.
    std::stable_sort(visits.begin(), visits.end(), [](const HistoryVisit& a, const HistoryVisit& b) { return a.time > b.time; });
    std::vector<HistoryDay> days;
    std::map<int, size_t> byKey;
    for (const auto& visit : visits) {
        int key = -1;
        std::string label = "Earlier";
        if (visit.time > 0) {
            const std::tm visited = LocalTime(visit.time);
            key = DayKey(visited);
            label = key == DayKey(today) ? "Today" : key == DayKey(yesterday) ? "Yesterday" : DateLabel(visited);
        }
        auto [found, inserted] = byKey.try_emplace(key, days.size());
        if (inserted) days.push_back({label, { }});
        days[found->second].visits.push_back(visit);
    }
    return days;
}

static const char* kStyle = R"(<style>
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;color:#243f36;font:14px/1.45 system-ui,sans-serif;background:#f9fbf8}
main{max-width:880px;margin:auto;padding:36px 40px 48px}
header{display:flex;align-items:center;gap:16px;margin-bottom:22px}
h1{font-size:30px;letter-spacing:-.5px;margin:0;flex:1}
input{font:inherit;width:280px;padding:7px 11px;border:1px solid #d5e0d6;border-radius:8px;background:white;color:inherit}
input:focus{outline:2px solid #9cc3ac;border-color:#9cc3ac}
section{margin-bottom:22px}
h2{font-size:12px;letter-spacing:1.5px;text-transform:uppercase;color:#71877b;margin:0 0 6px;padding:0 10px}
.list{background:white;border:1px solid #e2e9e1;border-radius:10px;overflow:hidden}
.row{display:flex;align-items:center;gap:12px;padding:8px 12px;color:inherit;text-decoration:none;border-top:1px solid #eef2ed}
.row:first-child{border-top:0}a.row:hover{background:#eef5ed}
.icon{flex:none;width:16px;height:16px;background:center/16px 16px no-repeat}
.letter{border-radius:4px;background:#dfeade;color:#456c51;font-size:10px;font-weight:700;display:grid;place-items:center}
.text{flex:1;min-width:0;display:flex;gap:10px;align-items:baseline}
.title{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:60%}
.url{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:#7b897f;font-size:12px}
.time{flex:none;color:#7b897f;font-size:12px;font-variant-numeric:tabular-nums}
.empty{color:#63766b;padding:28px 12px;text-align:center}
</style>
)";

static const char* kFilterScript = R"(<script>
const filter = document.getElementById('filter');
filter.addEventListener('input', () => {
  const query = filter.value.trim().toLowerCase();
  let any = false;
  for (const section of document.querySelectorAll('section')) {
    let shown = 0;
    for (const row of section.querySelectorAll('.row')) {
      const match = !query || (row.textContent + ' ' + (row.href || '')).toLowerCase().includes(query);
      row.hidden = !match;
      if (match) ++shown;
    }
    section.hidden = !shown;
    any = any || shown > 0;
  }
  const none = document.getElementById('none');
  if (none) none.hidden = any;
});
</script>
)";

namespace {
class PageWriter {
public:
    PageWriter(const IconSource& icons) : fIcons(icons) { }

    void Row(const PageRecord& page, const std::string& trailing)
    {
        const bool link = page.url.rfind("https://", 0) == 0 || page.url.rfind("http://", 0) == 0
            || page.url.rfind("file://", 0) == 0;
        fBody += link ? "<a class=\"row\" href=\"" + EscapeHTML(page.url) + "\">" : std::string("<div class=\"row\">");
        fBody += Icon(page.url);
        auto shown = page.url;
        if (auto scheme = shown.find("://"); scheme != std::string::npos) shown.erase(0, scheme + 3);
        fBody += "<span class=\"text\"><span class=\"title\">" + EscapeHTML(page.title.empty() ? page.url : page.title)
            + "</span><span class=\"url\">" + EscapeHTML(shown) + "</span></span>";
        if (!trailing.empty()) fBody += "<span class=\"time\">" + EscapeHTML(trailing) + "</span>";
        fBody += link ? "</a>\n" : "</div>\n";
    }
    void Raw(const std::string& html) { fBody += html; }
    const std::string& Body() const { return fBody; }
    const std::string& IconStyles() const { return fIconStyles; }

    std::string Finish(const char* title, const char* placeholder, bool empty, const char* emptyText) const
    {
        std::string page = "<!doctype html>\n<html lang=\"en\">\n<meta charset=\"utf-8\">\n<title>";
        page += title;
        page += "</title>\n";
        page += kStyle;
        if (!fIconStyles.empty()) page += "<style>\n" + fIconStyles + "</style>\n";
        page += "<main>\n<header><h1>";
        page += title;
        page += "</h1>";
        if (!empty) page += std::string("<input id=\"filter\" type=\"search\" placeholder=\"") + placeholder + "\" autofocus>";
        page += "</header>\n";
        if (empty) page += std::string("<p class=\"empty\">") + emptyText + "</p>\n";
        else {
            page += fBody;
            page += "<p class=\"empty\" id=\"none\" hidden>Nothing matches your search.</p>\n";
        }
        page += "</main>\n";
        if (!empty) page += kFilterScript;
        page += "</html>\n";
        return page;
    }

    std::string Icon(const std::string& url)
    {
        std::string image = fIcons ? fIcons(url) : std::string();
        // Only data: images are emitted, so a page cannot be made to load anything.
        if (image.rfind("data:image/", 0) == 0 && image.find_first_of("\"'()\\<>") == std::string::npos) {
            auto [found, inserted] = fIconClasses.try_emplace(image, fIconClasses.size());
            if (inserted)
                fIconStyles += ".i" + std::to_string(found->second) + "{background-image:url(" + image + ")}\n";
            return "<span class=\"icon i" + std::to_string(found->second) + "\"></span>";
        }
        auto host = FaviconKey(url);
        if (host.rfind("www.", 0) == 0) host.erase(0, 4);
        char letter = '*';
        for (char c : host) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { letter = c; break; }
        if (letter >= 'a' && letter <= 'z') letter = char(letter - 'a' + 'A');
        return std::string("<span class=\"icon letter\">") + letter + "</span>";
    }

private:
    const IconSource& fIcons;
    std::map<std::string, size_t> fIconClasses;
    std::string fIconStyles;
    std::string fBody;
};
}

std::string RenderHistoryPage(const std::vector<PageRecord>& history, int64_t now, const IconSource& icons)
{
    PageWriter writer(icons);
    for (const auto& day : GroupHistoryByDay(history, now)) {
        writer.Raw("<section><h2>" + EscapeHTML(day.label) + "</h2><div class=\"list\">\n");
        for (const auto& visit : day.visits) {
            std::string time;
            if (visit.time > 0) {
                const std::tm visited = LocalTime(visit.time);
                char text[8];
                std::snprintf(text, sizeof text, "%02d:%02d", visited.tm_hour, visited.tm_min);
                time = text;
            }
            writer.Row(*visit.page, time);
        }
        writer.Raw("</div></section>\n");
    }
    return writer.Finish("History", "Search History", history.empty(), "Pages you visit will be listed here.");
}

namespace {
const char* kBookmarksStyle = R"(<style>
nav.actions{display:flex;gap:8px;align-items:center;margin:-8px 0 20px}
.button{font:inherit;font-size:13px;color:inherit;text-decoration:none;padding:5px 12px;border:1px solid #d5e0d6;border-radius:7px;background:white;cursor:pointer}
.button:hover{background:#eef5ed}.hint{color:#7b897f;font-size:12px;margin-left:auto}
.item{position:relative}.item>.list{margin-left:28px;border:0;border-radius:0;background:none;overflow:visible}
.item+.item>.row,.item+.item>form,.item>.list>.item:first-child>.row{border-top:1px solid #eef2ed}
.list .placeholder{padding:10px 12px;color:#9aa69f;font-size:13px}
.row{gap:10px;padding:0 8px 0 12px;-webkit-user-select:none;user-select:none}
.row:hover{background:#eef5ed}
.link,.toggle{flex:1;min-width:0;display:flex;align-items:center;gap:12px;padding:8px 0;color:inherit;text-decoration:none;
font:inherit;background:none;border:0;text-align:left;cursor:default;-webkit-user-drag:none}
a.link,.toggle{cursor:pointer}
.chevron{flex:none;width:10px;height:10px;position:relative}
.chevron:before{content:"";position:absolute;left:1px;top:1px;width:5px;height:5px;border:solid #71877b;border-width:0 1.6px 1.6px 0;transform:rotate(-45deg)}
.toggle[aria-expanded=true] .chevron:before{transform:rotate(45deg);top:0}
.foldericon{border-radius:2px;background:#9cc3ac;position:relative;height:12px;margin-top:2px}
.foldericon:before{content:"";position:absolute;left:0;top:-3px;width:7px;height:4px;border-radius:2px 2px 0 0;background:#9cc3ac}
.tools{flex:none;display:flex;gap:2px;opacity:.55}.row:hover .tools{opacity:1}
.tool{font:inherit;font-size:12px;color:#456c51;text-decoration:none;padding:3px 7px;border-radius:5px;border:0;background:none;cursor:pointer}
.tool:hover{background:#dfeade}
.separator .row{min-height:32px}.separator-line{flex:1;display:flex;align-items:center;gap:10px;color:#7b897f;font-size:12px}
.separator-line:before,.separator-line:after{content:"";flex:1;border-top:1px solid #9aa69f}
.metadata{display:block;font-size:12px;color:#63776a;white-space:pre-wrap;max-height:4.5em;overflow:hidden}
form.editor{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:center;padding:10px 12px;background:#f3f8f2}
form.editor label{display:flex;gap:6px;align-items:center;font-size:13px;color:#4c6357}
form.editor input{width:220px;padding:5px 8px}form.editor input.address{width:300px}
form.editor textarea{font:inherit;width:300px;min-height:64px;padding:5px 8px;resize:vertical}
form.editor select{font:inherit;font-size:13px;padding:4px 6px;border:1px solid #d5e0d6;border-radius:6px;background:white;color:inherit;max-width:260px}
form.editor .buttons{display:flex;gap:6px;margin-left:auto}
.moving,.moving *{cursor:grabbing!important}.dragging>.row{opacity:.45}
.drop-into{background:#dbeedf!important;outline:2px solid #9cc3ac;outline-offset:-2px}
.marker{position:absolute;height:2px;background:#2c7d68;border-radius:1px;pointer-events:none;z-index:2}
</style>
)";

const char* kBookmarksScript = R"(<script>
(() => {
const main = document.querySelector('main');
const token = main.dataset.token;
const filter = document.getElementById('filter');
const none = document.getElementById('none');
const folderOptions = document.getElementById('folder-options');
const store = (() => { try { return window.sessionStorage; } catch (e) { return null; } })();
const read = (key, fallback) => { try { const value = store && store.getItem(key); return value === null ? fallback : value; } catch (e) { return fallback; } };
const write = (key, value) => { try { if (store) store.setItem(key, value); } catch (e) { } };
let open;
try { open = new Set(JSON.parse(read('summit-open-folders', '[]'))); } catch (e) { open = new Set(); }
const ownList = item => item.querySelector(':scope > .list');
const ownRow = item => item.querySelector(':scope > .row');
const items = list => [...list.children].filter(child => child.classList.contains('item'));
function showFolder(item, shown) {
  ownList(item).hidden = !shown;
  ownRow(item).querySelector('.toggle').setAttribute('aria-expanded', shown ? 'true' : 'false');
}
function setOpen(item, shown) {
  showFolder(item, shown);
  if (shown) open.add(item.dataset.id); else open.delete(item.dataset.id);
  write('summit-open-folders', JSON.stringify([...open]));
}
for (const item of document.querySelectorAll('.item.folder')) showFolder(item, open.has(item.dataset.id));

function visit(item, query) {
  const row = ownRow(item);
  const own = !query || [row.dataset.title, row.dataset.url, row.dataset.tags, row.dataset.description]
    .filter(Boolean).join(' ').toLowerCase().includes(query);
  if (!item.classList.contains('folder')) { item.hidden = !own; return own; }
  let inside = false;
  for (const child of items(ownList(item))) inside = visit(child, query) || inside;
  // A folder whose name matches shows everything in it.
  if (query && own) for (const child of item.querySelectorAll('.item')) child.hidden = false;
  item.hidden = !(own || inside);
  showFolder(item, query ? own || inside : open.has(item.dataset.id));
  return own || inside;
}
function applyFilter() {
  if (!filter) return;
  const query = filter.value.trim().toLowerCase();
  write('summit-bookmarks-filter', filter.value);
  let any = false;
  for (const section of document.querySelectorAll('section.root')) {
    let shown = false;
    for (const item of items(section.querySelector(':scope > .list'))) shown = visit(item, query) || shown;
    section.hidden = !!query && !shown;
    any = any || shown;
  }
  if (none) none.hidden = !query || any;
}
if (filter) {
  filter.value = read('summit-bookmarks-filter', '');
  filter.addEventListener('input', applyFilter);
  if (filter.value) applyFilter();
}

// Changes go to the browser as summit-bookmarks: links; it changes the
// bookmarks and this page is written again.
const go = (action, fields) => {
  let url = 'summit-bookmarks:' + action + '?token=' + token;
  for (const [name, value] of Object.entries(fields)) url += '&' + name + '=' + encodeURIComponent(value);
  location.href = url;
};
function field(label, name, value, wide, multiline) {
  const wrapper = document.createElement('label');
  wrapper.textContent = label + ' ';
  const input = document.createElement(multiline ? 'textarea' : 'input');
  input.dataset.field = name;
  input.value = value;
  input.spellcheck = false;
  if (wide) input.className = 'address';
  wrapper.appendChild(input);
  return wrapper;
}
// The folders it can go in: not itself, nor a folder inside it.
function folderChooser(item, selected) {
  const wrapper = document.createElement('label');
  wrapper.textContent = 'Folder ';
  const select = document.createElement('select');
  select.dataset.field = 'folder';
  for (const option of folderOptions.content.querySelectorAll('option')) {
    if (item && item.classList.contains('folder')
      && (option.value === item.dataset.id || item.querySelector('.item.folder[data-id="' + option.value + '"]'))) continue;
    select.appendChild(option.cloneNode(true));
  }
  select.value = selected;
  wrapper.appendChild(select);
  return wrapper;
}
function editor(fields, submitLabel, onSubmit, onCancel) {
  const form = document.createElement('form');
  form.className = 'editor';
  for (const element of fields) form.appendChild(element);
  const buttons = document.createElement('span');
  buttons.className = 'buttons';
  const cancel = document.createElement('button');
  cancel.type = 'button';
  cancel.className = 'button';
  cancel.textContent = 'Cancel';
  cancel.addEventListener('click', () => { form.remove(); onCancel(); });
  const submit = document.createElement('button');
  submit.type = 'submit';
  submit.className = 'button';
  submit.textContent = submitLabel;
  buttons.append(cancel, submit);
  form.appendChild(buttons);
  form.addEventListener('submit', event => {
    event.preventDefault();
    const values = {};
    for (const input of form.querySelectorAll('[data-field]')) values[input.dataset.field] = input.value;
    onSubmit(values);
  });
  form.addEventListener('keydown', event => { if (event.key === 'Escape') { form.remove(); onCancel(); } });
  return form;
}
function edit(item) {
  if (item.querySelector(':scope > form')) return;
  const row = ownRow(item);
  const folder = item.classList.contains('folder');
  const fields = [field('Name', 'title', row.dataset.title)];
  if (!folder) fields.push(field('Address', 'url', row.dataset.url, true));
  fields.push(folderChooser(item, item.parentElement.dataset.folder));
  fields.push(field('Tags', 'tags', row.dataset.tags || ''));
  fields.push(field('Description', 'description', row.dataset.description || '', false, true));
  const form = editor(fields, 'Save', values => go('save', Object.assign({ id: item.dataset.id }, values)),
    () => { row.hidden = false; });
  row.hidden = true;
  item.insertBefore(form, row.nextSibling);
  const name = form.querySelector('input');
  name.focus();
  name.select();
}
const newFolder = document.getElementById('new-folder');
if (newFolder) newFolder.addEventListener('click', () => {
  if (document.getElementById('new-folder-form')) return;
  const form = editor([field('Name', 'title', 'New Folder'), folderChooser(null, newFolder.dataset.folder)], 'Create',
    values => go('new-folder', values), () => { });
  form.id = 'new-folder-form';
  newFolder.parentElement.insertAdjacentElement('afterend', form);
  const name = form.querySelector('input');
  name.focus();
  name.select();
});

// Moving: press on a row and drag it before or after another, or onto a folder.
const marker = document.createElement('div');
marker.className = 'marker';
marker.hidden = true;
document.body.appendChild(marker);
let drag = null, swallowClick = false, highlighted = null;
function highlight(row) {
  if (highlighted === row) return;
  if (highlighted) highlighted.classList.remove('drop-into');
  highlighted = row;
  if (row) row.classList.add('drop-into');
}
function target(x, y) {
  const element = document.elementFromPoint(x, y);
  if (!element) return null;
  const row = element.closest('.row');
  if (row) {
    const item = row.parentElement;
    if (item === drag.item || drag.item.contains(item)) return null;
    const rect = row.getBoundingClientRect();
    if (item.classList.contains('folder') && y > rect.top + rect.height / 4 && y < rect.bottom - rect.height / 4)
      return { folder: item.dataset.id, index: -1, into: row };
    const list = item.parentElement;
    const after = y > rect.top + rect.height / 2;
    return { folder: list.dataset.folder, index: items(list).indexOf(item) + (after ? 1 : 0),
      line: after ? rect.bottom : rect.top, left: rect.left, width: rect.width };
  }
  const section = element.closest('section.root');
  if (!section) return null;
  const list = section.querySelector(':scope > .list');
  const rect = list.getBoundingClientRect();
  return { folder: section.dataset.folder, index: -1, line: rect.bottom, left: rect.left, width: rect.width };
}
document.addEventListener('mousedown', event => {
  if (event.button !== 0 || event.target.closest('.tools, form')) return;
  const row = event.target.closest('.row');
  if (!row) return;
  drag = { item: row.parentElement, x: event.clientX, y: event.clientY, active: false, to: null };
  // No text selection, and no link drag (which this port cannot carry out).
  event.preventDefault();
});
document.addEventListener('mousemove', event => {
  if (!drag) return;
  if (!drag.active) {
    if (Math.abs(event.clientX - drag.x) + Math.abs(event.clientY - drag.y) < 5) return;
    drag.active = true;
    drag.item.classList.add('dragging');
    document.body.classList.add('moving');
  }
  drag.to = target(event.clientX, event.clientY);
  highlight(drag.to && drag.to.into ? drag.to.into : null);
  if (drag.to && !drag.to.into) {
    marker.style.top = (drag.to.line + window.scrollY - 1) + 'px';
    marker.style.left = (drag.to.left + window.scrollX) + 'px';
    marker.style.width = drag.to.width + 'px';
    marker.hidden = false;
  } else marker.hidden = true;
});
document.addEventListener('mouseup', () => {
  if (!drag) return;
  const { item, active, to } = drag;
  drag = null;
  marker.hidden = true;
  highlight(null);
  item.classList.remove('dragging');
  document.body.classList.remove('moving');
  if (!active) return;
  swallowClick = true;
  setTimeout(() => { swallowClick = false; }, 0);
  if (to) go('move', to.index >= 0 ? { id: item.dataset.id, folder: to.folder, index: to.index } : { id: item.dataset.id, folder: to.folder });
});
document.addEventListener('click', event => {
  if (swallowClick) { event.preventDefault(); event.stopPropagation(); swallowClick = false; return; }
  const tool = event.target.closest('.tool[data-action]');
  if (tool) {
    event.preventDefault();
    const item = tool.closest('.item');
    if (tool.dataset.action === 'edit') edit(item);
    else go('delete', { id: item.dataset.id });
    return;
  }
  const toggle = event.target.closest('.toggle');
  if (toggle) {
    const item = toggle.closest('.item');
    setOpen(item, ownList(item).hidden);
  }
}, true);
})();
</script>
)";

const char* kBookmarkTools = "<span class=\"tools\"><button class=\"tool\" type=\"button\" data-action=\"edit\">Edit</button>"
    "<button class=\"tool\" type=\"button\" data-action=\"delete\">Delete</button></span>";

std::string MetadataAttributes(const BookmarkNode& node)
{
    return " data-tags=\"" + EscapeHTML(BookmarkTagsText(node.metadata.tags)) + "\" data-description=\""
        + EscapeHTML(node.metadata.description) + "\"";
}

std::string MetadataSummary(const BookmarkNode& node)
{
    if (node.metadata.Empty()) return {};
    std::string text = BookmarkTagsText(node.metadata.tags);
    if (!node.metadata.description.empty()) text += (text.empty() ? "" : "\n") + node.metadata.description;
    return "<span class=\"metadata\">" + EscapeHTML(text) + "</span>";
}

void BookmarkItems(PageWriter& writer, const std::vector<BookmarkNode>& nodes)
{
    for (const auto& node : nodes) {
        const std::string id = std::to_string(node.id);
        if (node.separator) {
            const auto label = EscapeHTML(node.title.empty() ? "Separator" : node.title);
            writer.Raw("<div class=\"item separator\" data-id=\"" + id + "\"><div class=\"row\" data-title=\"" + label
                + "\"><span class=\"separator-line\" role=\"separator\" aria-label=\"" + label + "\">" + label
                + "</span><span class=\"tools\"><button class=\"tool\" type=\"button\" data-action=\"delete\">Delete</button>"
                "</span></div></div>\n");
            continue;
        }
        if (node.folder) {
            const size_t count = CountBookmarks(node);
            writer.Raw("<div class=\"item folder\" data-id=\"" + id + "\"><div class=\"row\" data-title=\"" + EscapeHTML(node.title)
                + "\"" + MetadataAttributes(node) + "><button class=\"toggle\" type=\"button\" aria-expanded=\"false\"><span class=\"chevron\"></span>"
                "<span class=\"icon foldericon\"></span><span class=\"text\"><span class=\"title\">" + EscapeHTML(node.title)
                + "</span><span class=\"url\">" + std::to_string(count) + (count == 1 ? " bookmark" : " bookmarks")
                + "</span>" + MetadataSummary(node) + "</span></button>" + kBookmarkTools + "</div>\n<div class=\"list\" data-folder=\"" + id + "\" hidden>\n");
            BookmarkItems(writer, node.children);
            if (node.children.empty()) writer.Raw("<div class=\"placeholder\">Empty</div>\n");
            writer.Raw("</div></div>\n");
            continue;
        }
        const bool link = IsBookmarkableURL(node.url);
        auto shown = node.url;
        if (auto scheme = shown.find("://"); scheme != std::string::npos) shown.erase(0, scheme + 3);
        writer.Raw("<div class=\"item\" data-id=\"" + id + "\"><div class=\"row\" data-title=\"" + EscapeHTML(node.title)
            + "\" data-url=\"" + EscapeHTML(node.url) + "\"" + MetadataAttributes(node) + ">");
        writer.Raw(link ? "<a class=\"link\" draggable=\"false\" href=\"" + EscapeHTML(node.url) + "\">" : std::string("<span class=\"link\">"));
        writer.Raw(writer.Icon(node.url) + "<span class=\"text\"><span class=\"title\">"
            + EscapeHTML(node.title.empty() ? node.url : node.title) + "</span><span class=\"url\">" + EscapeHTML(shown)
            + "</span>" + MetadataSummary(node) + "</span>" + (link ? "</a>" : "</span>") + kBookmarkTools + "</div></div>\n");
    }
}

std::string PercentDecode(std::string_view text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            result += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else result += text[i];
    }
    return result;
}
}

std::string BookmarkPageToken(const std::string& token)
{
    std::string clean;
    for (char c : token) if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) clean += c;
    return clean.substr(0, 64);
}

std::optional<BookmarkPageLink> ParseBookmarkPageLink(std::string_view url)
{
    const std::string prefix = std::string(kBookmarksPageScheme) + ":";
    if (url.size() > 300000 || url.size() < prefix.size()) return std::nullopt;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) return std::nullopt;
    url.remove_prefix(prefix.size());
    BookmarkPageLink link;
    const size_t query = url.find('?');
    link.action = std::string(url.substr(0, query));
    if (link.action.empty() || link.action.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
        return std::nullopt;
    std::string_view rest = query == std::string_view::npos ? std::string_view() : url.substr(query + 1);
    auto number = [](std::string_view text, int64_t& value) {
        if (text.empty() || text.size() > 18 || text.find_first_not_of("0123456789") != std::string_view::npos) return false;
        value = std::stoll(std::string(text));
        return true;
    };
    while (!rest.empty()) {
        const size_t end = rest.find('&');
        const auto field = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        const size_t equals = field.find('=');
        if (equals == std::string_view::npos) continue;
        const auto name = field.substr(0, equals), value = field.substr(equals + 1);
        int64_t parsed = 0;
        if (name == "token") link.token = std::string(value);
        else if (name == "id" && number(value, parsed)) link.id = parsed;
        else if (name == "folder" && number(value, parsed)) link.folder = parsed;
        else if (name == "index" && number(value, parsed)) link.index = static_cast<size_t>(parsed);
        else if (name == "title") { link.title = PercentDecode(value); link.hasTitle = true; }
        else if (name == "url") { link.url = PercentDecode(value); link.hasURL = true; }
        else if (name == "description") { link.description = PercentDecode(value); link.hasDescription = true; }
        else if (name == "tags") { link.tags = PercentDecode(value); link.hasTags = true; }
    }
    if (link.token.empty() || link.token != BookmarkPageToken(link.token)) return std::nullopt;
    return link;
}

std::string RenderBookmarksPage(const Bookmarks& bookmarks, const IconSource& icons, const std::string& pageToken)
{
    const std::string token = BookmarkPageToken(pageToken);
    PageWriter writer(icons);
    const bool empty = bookmarks.Empty();
    for (const auto* root : {&bookmarks.Bar(), &bookmarks.Other()}) {
        if (empty) break;
        const std::string id = std::to_string(root->id);
        writer.Raw("<section class=\"root\" data-folder=\"" + id + "\"><h2>" + EscapeHTML(root->title)
            + "</h2><div class=\"list\" data-folder=\"" + id + "\">\n");
        BookmarkItems(writer, root->children);
        if (root->children.empty())
            writer.Raw(std::string("<div class=\"placeholder\">") + (root->id == kBookmarksBarFolder
                ? "Drag bookmarks here to show them on the bookmarks bar." : "Drag bookmarks here.") + "</div>\n");
        writer.Raw("</div></section>\n");
    }
    std::string options;
    for (const auto& folder : bookmarks.Folders()) {
        std::string indent;
        for (size_t i = 0; i < folder.depth; ++i) indent += "\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0";
        options += "<option value=\"" + std::to_string(folder.id) + "\">" + indent + EscapeHTML(folder.title) + "</option>";
    }
    std::string page = "<!doctype html>\n<html lang=\"en\">\n<meta charset=\"utf-8\">\n<title>Bookmarks</title>\n";
    page += kStyle;
    page += kBookmarksStyle;
    if (!writer.IconStyles().empty()) page += "<style>\n" + writer.IconStyles() + "</style>\n";
    page += "<main data-token=\"" + token + "\">\n<header><h1>Bookmarks</h1>";
    if (!empty) page += "<input id=\"filter\" type=\"search\" placeholder=\"Search Bookmarks\" autofocus>";
    page += "</header>\n<nav class=\"actions\">";
    page += "<button class=\"button\" type=\"button\" id=\"new-folder\" data-folder=\"" + std::to_string(kOtherBookmarksFolder)
        + "\">New Folder…</button>";
    page += "<a class=\"button\" href=\"" + EscapeHTML(std::string(kBookmarksPageScheme) + ":import?token=" + token) + "\">Import…</a>";
    if (!empty) page += "<a class=\"button\" href=\"" + EscapeHTML(std::string(kBookmarksPageScheme) + ":export?token=" + token)
        + "\">Export…</a><span class=\"hint\">Drag bookmarks to move them.</span>";
    page += "</nav>\n<template id=\"folder-options\">" + options + "</template>\n";
    if (empty)
        page += "<p class=\"empty\">No bookmarks yet. Use Bookmarks › Bookmark This Page, or the star in the toolbar, "
            "or import them from another browser.</p>\n";
    else {
        page += writer.Body();
        page += "<p class=\"empty\" id=\"none\" hidden>Nothing matches your search.</p>\n";
    }
    page += "</main>\n";
    page += kBookmarksScript;
    page += "</html>\n";
    return page;
}

std::string RenderPrivateStartPage(const std::string& searchEngineName, const std::string& searchPrefix)
{
    // Every engine takes the query as "q"; the form goes to the address before it.
    const auto action = searchPrefix.substr(0, searchPrefix.find('?'));
    std::string page = R"(<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>New Private Tab</title>
<style>
*{box-sizing:border-box}html{background:#2a1a45}body{margin:0;color:#f1ebfb;font:16px/1.55 system-ui,sans-serif;
background:linear-gradient(160deg,#3b2566 0%,#2a1a45 55%,#20133a 100%);min-height:100vh}
main{max-width:720px;margin:auto;padding:64px 40px 40px}
.mask{width:72px;height:36px;border-radius:18px;background:#fff;position:relative;margin-bottom:26px}
.mask:before,.mask:after{content:"";position:absolute;top:11px;width:18px;height:13px;border-radius:50%;background:#3b2566}
.mask:before{left:13px}.mask:after{right:13px}
h1{font-size:38px;line-height:1.15;margin:0 0 14px;letter-spacing:-.5px}
p{margin:0 0 12px;color:#d6caec}
form{display:flex;margin:30px 0 34px;background:#fff;border-radius:10px;overflow:hidden;box-shadow:0 6px 24px rgba(0,0,0,.28)}
input{flex:1;border:0;padding:14px 16px;font:inherit;color:#1e1530;outline:none;min-width:0}
button{border:0;background:#8460c4;color:#fff;font:inherit;font-weight:600;padding:0 20px;cursor:pointer}
button:hover{background:#9670d6}
.cards{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.12);border-radius:10px;padding:16px 18px}
.card h2{font-size:14px;margin:0 0 6px;color:#fff;letter-spacing:.3px}.card p{font-size:14px;margin:0}
.note{margin-top:26px;font-size:13px;color:#b7a8d4}
@media(max-width:620px){main{padding:40px 22px}.cards{grid-template-columns:1fr}h1{font-size:30px}}
</style>
<main>
<div class="mask" aria-hidden="true"></div>
<h1>Private Browsing</h1>
<p>Pages you open in private windows are not added to your history. Their cookies, site data and cache
are kept apart from your other windows and forgotten when you close the last private window.</p>
<form action=")" + EscapeHTML(action) + R"(" method="get">
<input name="q" type="search" placeholder="Search with )" + EscapeHTML(searchEngineName) + R"(" aria-label="Search" autofocus>
<button type="submit">Search</button>
</form>
<div class="cards">
<div class="card"><h2>SUMMIT FORGETS</h2><p>The pages you visit, what you search for, cookies, site data, cached files and zoom levels you set here.</p></div>
<div class="card"><h2>SUMMIT KEEPS</h2><p>Files you download and bookmarks you create. Extensions do not run in private windows.</p></div>
</div>
<p class="note">Private browsing does not hide you from the websites you visit, your employer or school, or your internet provider.</p>
</main>
</html>
)";
    return page;
}
}
