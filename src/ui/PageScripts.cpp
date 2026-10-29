#include "PageScripts.h"

namespace summit {
const char* const kPopupScript = R"js((() => {
if (window.__summitPopups) return;
window.__summitPopups = true;
const handlers = window.webkit && window.webkit.messageHandlers;
const handler = handlers && handlers.summitPopups;
if (!handler) return;
const post = handler.postMessage.bind(handler);
const open = window.open;
window.open = function (url, target, features) {
  const opened = open.apply(this, arguments);
  if (!opened) {
    let href = 'about:blank';
    try {
      if (url !== undefined && url !== null && String(url) !== '') href = new URL(String(url), document.baseURI).href;
    } catch (error) {
      href = String(url);
    }
    post(JSON.stringify({ url: href, target: target ? String(target) : '' })).catch(() => {});
  }
  return opened;
};
})();)js";

const char* const kPictureInPictureScript = R"js((() => {
if (window.__summitPip) return;
window.__summitPip = true;
const handlers = window.webkit && window.webkit.messageHandlers;
const handler = handlers && handlers.summitPip;
if (!handler || !window.HTMLVideoElement || HTMLVideoElement.prototype.requestPictureInPicture) return;
const post = (message) => handler.postMessage(JSON.stringify(message)).catch(() => {});
const prefix = String(Math.floor(Math.random() * 1e8) + 1e8);
let current = null, next = 0;
const tokenOf = (video) => {
  if (!video.dataset.summitPip) video.dataset.summitPip = prefix + String(++next);
  return video.dataset.summitPip;
};
const pipWindow = { width: 0, height: 0, onresize: null, addEventListener() {}, removeEventListener() {}, dispatchEvent() { return true; } };
Object.defineProperty(Document.prototype, 'pictureInPictureEnabled', { configurable: true, get: () => true });
Object.defineProperty(Document.prototype, 'pictureInPictureElement', {
  configurable: true, get() { return current && current.isConnected && current.ownerDocument === this ? current : null; }
});
Object.defineProperty(HTMLVideoElement.prototype, 'disablePictureInPicture', {
  configurable: true,
  get() { return this.hasAttribute('disablepictureinpicture'); },
  set(value) { if (value) this.setAttribute('disablepictureinpicture', ''); else this.removeAttribute('disablepictureinpicture'); }
});
const request = HTMLVideoElement.prototype.requestPictureInPicture = function () {
  const video = this;
  if (video.readyState === 0) return Promise.reject(new DOMException('The video has no data yet.', 'InvalidStateError'));
  if (video.disablePictureInPicture) return Promise.reject(new DOMException('Picture in Picture is turned off for this video.', 'InvalidStateError'));
  if (current === video) return Promise.resolve(pipWindow);
  return new Promise((resolve, reject) => {
    const entered = () => { clearTimeout(timer); resolve(pipWindow); };
    const timer = setTimeout(() => {
      video.removeEventListener('enterpictureinpicture', entered);
      reject(new DOMException('Picture in Picture did not start.', 'NotAllowedError'));
    }, 5000);
    video.addEventListener('enterpictureinpicture', entered, { once: true });
    post({ type: 'enter', token: tokenOf(video) });
  });
};
Document.prototype.exitPictureInPicture = function () {
  const video = current;
  if (!video) return Promise.reject(new DOMException('No video is in Picture in Picture.', 'InvalidStateError'));
  return new Promise((resolve) => {
    video.addEventListener('leavepictureinpicture', () => resolve(), { once: true });
    setTimeout(resolve, 2000);
    post({ type: 'exit' });
  });
};
document.addEventListener('enterpictureinpicture', (event) => { current = event.target; }, true);
document.addEventListener('leavepictureinpicture', (event) => { if (current === event.target) current = null; }, true);
// The Picture in Picture button of the engine's own video controls (#83),
// which cannot call this script from their world: they ask first whether
// the browser shows videos in their own window, then toggle.
document.addEventListener('summitpictureinpicturequery', (event) => {
  if (event.target instanceof HTMLVideoElement) event.preventDefault();
}, true);
const requestPip = HTMLVideoElement.prototype.requestPictureInPicture, exitPip = Document.prototype.exitPictureInPicture;
document.addEventListener('summitpictureinpicturetoggle', (event) => {
  const video = event.target;
  if (!(video instanceof HTMLVideoElement)) return;
  if (current === video) exitPip.call(document).catch(() => {});
  else requestPip.call(video).catch(() => {});
}, true);
// Players lay their controls over the video: find it under the pointer.
document.addEventListener('contextmenu', (event) => {
  const video = document.elementsFromPoint(event.clientX, event.clientY).find((element) => element instanceof HTMLVideoElement);
  if (video && video.readyState > 0) post({ type: 'context', token: tokenOf(video) });
}, true);
window.__summitPipLargest = () => {
  let best = null, area = 0;
  for (const video of document.querySelectorAll('video')) {
    const rect = video.getBoundingClientRect();
    const size = rect.width * rect.height;
    const score = size * (video.paused ? 1 : 4);
    if (video.readyState > 0 && size > 0 && score > area && !video.disablePictureInPicture) { best = video; area = score; }
  }
  return best ? tokenOf(best) : '';
};
// A Picture in Picture button at the right edge of the video under the
// pointer: players such as YouTube cover the video and take its right
// clicks. It lives in a closed shadow root on a fixed host of its own, out of
// the page's layout and styles, and the page never sees its clicks (these
// listeners on the window are the first to run).
let host = null, button = null, shownFor = null, overButton = false, checkTimer = 0, idleTimer = 0, x = 0, y = 0;
const css = ':host{all:initial}' +
  'button{all:initial;position:fixed;display:flex;align-items:center;box-sizing:border-box;height:32px;padding:0 9px;' +
  'transform:translate(-100%,-50%);border-radius:8px;background:rgba(18,18,20,.78);color:#fff;cursor:pointer;' +
  'box-shadow:0 1px 4px rgba(0,0,0,.35),inset 0 0 0 1px rgba(255,255,255,.14);font:500 13px/1 system-ui,"Noto Sans",sans-serif;' +
  'white-space:nowrap;pointer-events:auto;user-select:none;-webkit-user-select:none;transition:background .15s}' +
  'button:hover{background:rgba(12,12,14,.92)}button.off{display:none}' +
  'svg{display:block;width:20px;height:16px;flex:none}' +
  'span{display:block;max-width:0;overflow:hidden;transition:max-width .2s,margin .2s}' +
  'button:hover span{max-width:170px;margin-left:7px}';
const svg = (name, attributes) => {
  const element = document.createElementNS('http://www.w3.org/2000/svg', name);
  for (const key in attributes) element.setAttribute(key, attributes[key]);
  return element;
};
const ensureHost = (video) => {
  const fullscreen = document.fullscreenElement || document.webkitFullscreenElement;
  if (fullscreen && (fullscreen === video || !fullscreen.contains(video))) return false;
  const parent = fullscreen || document.documentElement;
  if (!parent) return false;
  if (!host) {
    host = document.createElement('summit-pip-button');
    const fixed = { all: 'initial', position: 'fixed', left: '0', top: '0', width: '0', height: '0', 'z-index': '2147483647',
      display: 'block', overflow: 'visible', 'pointer-events': 'none', visibility: 'visible', opacity: '1', transform: 'none' };
    for (const key in fixed) host.style.setProperty(key, fixed[key], 'important');
    const root = host.attachShadow({ mode: 'closed' });
    if ('adoptedStyleSheets' in root && window.CSSStyleSheet) {
      const sheet = new CSSStyleSheet();
      sheet.replaceSync(css);
      root.adoptedStyleSheets = [sheet];
    } else {
      const style = document.createElement('style');
      style.textContent = css;
      root.appendChild(style);
    }
    button = document.createElement('button');
    button.className = 'off';
    button.tabIndex = -1;
    button.title = 'Picture in Picture';
    button.setAttribute('aria-label', 'Picture in Picture');
    const icon = svg('svg', { viewBox: '0 0 20 16', 'aria-hidden': 'true' });
    icon.append(svg('rect', { x: '1', y: '1', width: '18', height: '14', rx: '2', fill: 'none', stroke: 'currentColor', 'stroke-width': '1.6' }),
      svg('rect', { x: '9.5', y: '7.5', width: '7', height: '5', rx: '1', fill: 'currentColor' }));
    const label = document.createElement('span');
    label.textContent = 'Picture in Picture';
    button.append(icon, label);
    root.appendChild(button);
  }
  if (host.parentNode !== parent) parent.appendChild(host);
  return true;
};
const hide = () => {
  clearTimeout(idleTimer);
  shownFor = null;
  overButton = false;
  if (button) button.className = 'off';
};
const videos = document.getElementsByTagName('video');
// Big enough to be the page's video rather than a decoration, and loaded.
const eligible = (video) => video.readyState > 0 && video !== current && !video.disablePictureInPicture && !(video.duration < 15);
const videoAt = () => {
  let found = null;
  try {
    found = document.elementsFromPoint(x, y).find((element) => element instanceof HTMLVideoElement) || null;
  } catch (error) { }
  // A video that takes no pointer events is still found by its box.
  if (!found) {
    for (const video of videos) {
      const rect = video.getBoundingClientRect();
      if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom && getComputedStyle(video).visibility === 'visible') { found = video; break; }
    }
  }
  return found;
};
const check = () => {
  checkTimer = 0;
  // Most pages have no video: no hit test (and no layout) for them.
  if (!videos.length) { if (shownFor) hide(); return; }
  if (overButton && shownFor && eligible(shownFor)) { clearTimeout(idleTimer); return; }
  const video = videoAt();
  const rect = video && video.getBoundingClientRect();
  if (!video || !eligible(video) || rect.width < 200 || rect.height < 110 || !ensureHost(video)) { hide(); return; }
  const view = document.documentElement;
  const right = Math.min(rect.right, view.clientWidth || innerWidth), top = Math.max(rect.top, 0);
  const bottom = Math.min(rect.bottom, view.clientHeight || innerHeight);
  if (bottom - top < 48) { hide(); return; }
  button.style.left = Math.round(right - 10) + 'px';
  button.style.top = Math.round((top + bottom) / 2) + 'px';
  button.className = '';
  shownFor = video;
  // It goes when the pointer rests, as the players' own controls do.
  clearTimeout(idleTimer);
  idleTimer = setTimeout(() => { if (!overButton) hide(); }, 3000);
};
window.addEventListener('mousemove', (event) => {
  x = event.clientX;
  y = event.clientY;
  overButton = !!host && event.target === host;
  if (!checkTimer) checkTimer = setTimeout(check, overButton ? 0 : 90);
}, { capture: true, passive: true });
const press = (event) => {
  if (!host || event.target !== host) return;
  event.stopImmediatePropagation();
  // Not on pointer events: a cancelled pointerdown would lose the click.
  if (!event.type.startsWith('pointer')) event.preventDefault();
  if (event.type === 'click' && event.button === 0 && shownFor) {
    const video = shownFor;
    hide();
    request.call(video).catch(() => {});
  }
};
for (const type of ['pointerdown', 'pointerup', 'mousedown', 'mouseup', 'click', 'dblclick', 'auxclick'])
  window.addEventListener(type, press, true);
for (const type of ['scroll', 'resize', 'blur', 'fullscreenchange', 'webkitfullscreenchange', 'enterpictureinpicture'])
  window.addEventListener(type, (event) => { if (shownFor && (type !== 'blur' || event.target === window)) hide(); }, { capture: true, passive: true });
document.addEventListener('mouseout', (event) => { if (!event.relatedTarget && shownFor) hide(); }, { capture: true, passive: true });
})();)js";

const char* const kPasswordScript = R"js((() => {
if (window.__summitPasswords) return;
window.__summitPasswords = true;
const handlers = window.webkit && window.webkit.messageHandlers;
const handler = handlers && handlers.summitPasswords;
if (!handler) return;
const post = (message) => handler.postMessage(JSON.stringify(message));
const textTypes = ['text', 'email', 'tel', ''];
const isPassword = (input) => input instanceof HTMLInputElement && input.type === 'password';
const shown = (input) => !!(input && !input.disabled && (input.offsetWidth || input.offsetHeight || input.getClientRects().length));
const inputsOf = (root) => Array.from((root && root.querySelectorAll ? root : document).querySelectorAll('input'));
// The account name: a field marked as such, else the last text field
// before the password.
const usernameFor = (password, filled) => {
  const inputs = inputsOf(password.form || document);
  const marked = inputs.find((input) => /\busername\b|\bemail\b/.test(input.autocomplete || '') && (!filled || input.value));
  if (marked) return marked;
  const index = inputs.indexOf(password);
  for (let i = index - 1; i >= 0; --i) {
    const input = inputs[i];
    if (textTypes.includes((input.getAttribute('type') || '').toLowerCase()) && shown(input) && (!filled || input.value)) return input;
  }
  return null;
};
let lastSent = '';
const capture = (root) => {
  const passwords = inputsOf(root).filter((input) => isPassword(input) && input.value);
  if (!passwords.length) return;
  const fresh = passwords.find((input) => (input.autocomplete || '').includes('new-password'));
  const chosen = fresh || passwords[0];
  const username = usernameFor(passwords[0], true);
  const key = (username ? username.value : '') + '\n' + chosen.value;
  if (key === lastSent) return;
  lastSent = key;
  post({ type: 'submit', username: username ? username.value : '', password: chosen.value }).catch(() => {});
};
document.addEventListener('submit', (event) => capture(event.target), true);
// Pages that sign in with script: a click on a button, or Enter in a field.
document.addEventListener('click', (event) => {
  const button = event.target && event.target.closest && event.target.closest('button, input[type=submit], input[type=button], [role=button]');
  if (button) setTimeout(() => capture(button.form || document), 0);
}, true);
document.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && event.target instanceof HTMLInputElement) capture(event.target.form || document);
}, true);
const setValue = (input, value) => {
  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
  setter.call(input, value);
  input.dispatchEvent(new Event('input', { bubbles: true }));
  input.dispatchEvent(new Event('change', { bubbles: true }));
};
let menu = null;
const closeMenu = () => { if (menu) { menu.remove(); menu = null; } };
// A small list under a field: saved accounts, or a suggested password.
const showMenu = (anchor, items) => {
  closeMenu();
  const host = document.createElement('div');
  host.setAttribute('data-summit-passwords', '');
  const root = host.attachShadow({ mode: 'closed' });
  const rect = anchor.getBoundingClientRect();
  const style = document.createElement('style');
  style.textContent = ':host{all:initial}.menu{position:fixed;z-index:2147483647;min-width:200px;background:#fff;color:#1f2328;' +
    'border-radius:6px;box-shadow:0 6px 20px rgba(0,0,0,.2),0 0 0 1px rgba(0,0,0,.1);font:13px/1.3 system-ui,"Noto Sans",sans-serif;padding:4px 0}' +
    '.item{padding:6px 12px;cursor:pointer;white-space:nowrap}.item:hover{background:#e8f0fe}.detail{color:#59636e;font-size:12px}' +
    '@media (prefers-color-scheme: dark){.menu{background:#2b2f36;color:#e8eaed}.item:hover{background:#3b4252}.detail{color:#a7b0ba}}';
  const list = document.createElement('div');
  list.className = 'menu';
  list.style.left = Math.max(4, rect.left) + 'px';
  list.style.top = (rect.bottom + 2) + 'px';
  for (const item of items) {
    const row = document.createElement('div');
    row.className = 'item';
    row.textContent = item.label;
    if (item.detail) {
      const detail = document.createElement('div');
      detail.className = 'detail';
      detail.textContent = item.detail;
      row.appendChild(detail);
    }
    row.addEventListener('mousedown', (event) => { event.preventDefault(); item.choose(); closeMenu(); });
    list.appendChild(row);
  }
  root.append(style, list);
  (document.body || document.documentElement).appendChild(host);
  menu = host;
};
document.addEventListener('focusout', () => setTimeout(closeMenu, 150), true);
document.addEventListener('scroll', closeMenu, true);
const fillAccount = (password, account) => {
  const username = usernameFor(password, false);
  if (username && account.username) setValue(username, account.username);
  setValue(password, account.password);
};
let accounts = null;
// Saved accounts are asked for when the user starts on a sign-in form,
// never when the page loads.
const offer = (field) => {
  const passwords = inputsOf(field.form || document).filter((input) => isPassword(input) && shown(input));
  const fresh = passwords.filter((input) => (input.autocomplete || '').includes('new-password'));
  if (fresh.length && fresh.includes(field)) {
    const characters = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789-_!@#';
    const bytes = crypto.getRandomValues(new Uint8Array(20));
    const generated = Array.from(bytes, (byte) => characters[byte % characters.length]).join('');
    showMenu(field, [{ label: 'Use a strong password', detail: generated, choose: () => fresh.forEach((input) => setValue(input, generated)) }]);
    return;
  }
  const password = passwords.find((input) => !fresh.includes(input));
  if (!password) return;
  const show = () => {
    if (!accounts || !accounts.length) return;
    if (accounts.length === 1 && !password.value) { fillAccount(password, accounts[0]); return; }
    showMenu(field, accounts.map((account) => ({ label: account.username || '(no name)', detail: 'Saved password', choose: () => fillAccount(password, account) })));
  };
  if (accounts) { show(); return; }
  post({ type: 'query' }).then((reply) => {
    try { accounts = JSON.parse(reply || '[]'); } catch (error) { accounts = []; }
    show();
  }).catch(() => { accounts = []; });
};
document.addEventListener('focusin', (event) => {
  const field = event.target;
  if (!(field instanceof HTMLInputElement)) return;
  if (isPassword(field) || ((textTypes.includes((field.getAttribute('type') || '').toLowerCase()))
      && inputsOf(field.form || document).some(isPassword)))
    offer(field);
}, true);
})();)js";

// The page only says where the field is: the passkeys are offered in a native
// menu, and choosing one is the browser's, never the page's, doing.
const char* const kPasskeyFieldScript = R"js((() => {
if (window.__summitPasskeys || window !== window.top) return;
window.__summitPasskeys = true;
const handlers = window.webkit && window.webkit.messageHandlers;
const handler = handlers && handlers.summitPasskeys;
if (!handler) return;
const isPasskeyField = (field) => field instanceof HTMLInputElement
  && (field.getAttribute('autocomplete') || '').toLowerCase().split(/\s+/).includes('webauthn');
const report = (event) => {
  const field = event.target;
  if (!isPasskeyField(field)) return;
  const rect = field.getBoundingClientRect();
  handler.postMessage(JSON.stringify({ type: 'field', left: rect.left, top: rect.top, right: rect.right,
    bottom: rect.bottom, width: window.innerWidth, height: window.innerHeight })).catch(() => {});
};
document.addEventListener('focusin', report, true);
document.addEventListener('mousedown', report, true);
// The list follows the field: it goes when the field is left or moves. A
// click in the list does not take the field's focus (the list takes none).
const leave = (event) => {
  if (event.type === 'focusout' && !isPasskeyField(event.target)) return;
  handler.postMessage(JSON.stringify({ type: 'leave' })).catch(() => {});
};
document.addEventListener('focusout', leave, true);
window.addEventListener('scroll', leave, { capture: true, passive: true });
window.addEventListener('resize', leave, { passive: true });
})();)js";
}
