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
// menu, and choosing one is the browser's, never the page's, doing. In every
// frame: a frame's field is placed in the page by its ancestors, each of
// which adds where the frame's element is (window.postMessage up the chain;
// an embedding page can only misplace the list, not choose in it).
const char* const kPasskeyFieldScript = R"js((() => {
if (window.__summitPasskeys) return;
window.__summitPasskeys = true;
const handlers = window.webkit && window.webkit.messageHandlers;
const handler = handlers && handlers.summitPasskeys;
if (!handler) return;
const post = (message) => handler.postMessage(JSON.stringify(message)).catch(() => {});
const isPasskeyField = (field) => field instanceof HTMLInputElement
  && (field.getAttribute('autocomplete') || '').toLowerCase().split(/\s+/).includes('webauthn');
const tag = '__summitPasskeyFrame';
// Where a child frame's content starts in this frame's viewport.
const frameBox = (source) => {
  for (const element of document.querySelectorAll('iframe, frame')) {
    if (element.contentWindow !== source) continue;
    const rect = element.getBoundingClientRect();
    const style = getComputedStyle(element);
    return { x: rect.left + element.clientLeft + (parseFloat(style.paddingLeft) || 0),
      y: rect.top + element.clientTop + (parseFloat(style.paddingTop) || 0) };
  }
  return null;
};
const waiting = new Map();
let queries = 0;
// This frame's place in the page's viewport, and the page's width.
const placeInPage = () => new Promise((resolve) => {
  if (window === window.top) { resolve({ x: 0, y: 0, width: window.innerWidth }); return; }
  const id = String(++queries) + Math.random().toString(36).slice(2);
  waiting.set(id, resolve);
  setTimeout(() => { if (waiting.delete(id)) resolve(null); }, 1000);
  window.parent.postMessage({ [tag]: 'where', id }, '*');
});
window.addEventListener('message', (event) => {
  const data = event.data;
  if (!data || typeof data !== 'object' || !Object.prototype.hasOwnProperty.call(data, tag)) return;
  // The browser's own messages: the page's listeners need not see them.
  event.stopImmediatePropagation();
  const source = event.source;
  if (data[tag] === 'where' && source) {
    const box = frameBox(source);
    if (!box) return;
    placeInPage().then((place) => {
      if (place) source.postMessage({ [tag]: 'here', id: data.id, x: place.x + box.x, y: place.y + box.y, width: place.width }, '*');
    });
  } else if (data[tag] === 'here' && source === window.parent) {
    const resolve = waiting.get(data.id);
    if (!resolve) return;
    waiting.delete(data.id);
    resolve({ x: Number(data.x) || 0, y: Number(data.y) || 0, width: Number(data.width) || 0 });
  }
}, true);
const report = (event) => {
  const field = event.target;
  if (!isPasskeyField(field)) return;
  const rect = field.getBoundingClientRect();
  placeInPage().then((place) => {
    if (!place || !place.width) return;
    post({ type: 'field', left: rect.left + place.x, top: rect.top + place.y, right: rect.right + place.x,
      bottom: rect.bottom + place.y, width: place.width });
  });
};
document.addEventListener('focusin', report, true);
document.addEventListener('mousedown', report, true);
// The list follows the field: it goes when the field is left or moves. A
// click in the list does not take the field's focus (the list takes none).
const leave = (event) => {
  if (event.type === 'focusout' && !isPasskeyField(event.target)) return;
  post({ type: 'leave' });
};
document.addEventListener('focusout', leave, true);
window.addEventListener('scroll', leave, { capture: true, passive: true });
window.addEventListener('resize', leave, { passive: true });
})();)js";

// AutoFill (#149). The page only says which kind of field the user went
// into and where it is; the saved entries are shown in a native list, and
// only the one the user chooses there comes back to the page, which fills
// the form with it. Card numbers never reach a page the user did not
// choose a card on. Other text fields offer their form history the same
// way (a chosen value fills that field only), and what the user typed into
// a sent form's text fields is reported for it; typing in the field tells
// the browser what to filter the list by.
const char* const kAutoFillScript = R"js((() => {
if (window.__summitAutoFill) return;
window.__summitAutoFill = true;
const handlers = window.webkit && window.webkit.messageHandlers;
const handler = handlers && handlers.summitAutoFill;
if (!handler) return;
const post = (message) => handler.postMessage(JSON.stringify(message));
// autocomplete's field names (its last token) to the browser's keys.
const tokens = {
  'name': 'name', 'given-name': 'givenName', 'family-name': 'familyName', 'organization': 'organization',
  'street-address': 'street', 'address-line1': 'line1', 'address-line2': 'line2', 'address-line3': 'line3',
  'address-level1': 'region', 'address-level2': 'city', 'postal-code': 'postalCode', 'country': 'country',
  'country-name': 'country', 'email': 'email', 'tel': 'tel', 'tel-national': 'tel',
  'cc-name': 'ccName', 'cc-given-name': 'ccGivenName', 'cc-family-name': 'ccFamilyName', 'cc-number': 'ccNumber',
  'cc-exp': 'ccExp', 'cc-exp-month': 'ccMonth', 'cc-exp-year': 'ccYear'
};
// Field names autocomplete knows that AutoFill leaves alone (a security
// code is never stored or filled).
const ignored = new Set(['additional-name', 'honorific-prefix', 'honorific-suffix', 'nickname', 'address-level3',
  'address-level4', 'cc-csc', 'cc-type', 'cc-additional-name', 'username', 'current-password', 'new-password',
  'one-time-code', 'bday', 'bday-day', 'bday-month', 'bday-year', 'sex', 'url', 'photo', 'language',
  'organization-title', 'transaction-amount', 'transaction-currency', 'tel-country-code', 'tel-area-code',
  'tel-local', 'tel-local-prefix', 'tel-local-suffix', 'tel-extension', 'impp', 'off']);
const cardKeys = new Set(['ccName', 'ccGivenName', 'ccFamilyName', 'ccNumber', 'ccExp', 'ccMonth', 'ccYear']);
// Without autocomplete: the field's name, id, label and placeholder, in
// English and a few common languages. The first rule that matches wins;
// "card" rules count only in a form that has a card number.
const rules = [
  ['skip', /\b(cvc|cvv2?|csc|cid|security code|card verification|verification code|captcha|coupon|promo|voucher|gift|search|username|user name|login|password|passwort|subject|message|comment)\b/],
  ['ccNumber', /\b(card ?number|card ?no|card ?num|cc ?num(ber)?|ccnum|credit ?card|debit ?card|kartennummer|n[uú]mero de (la )?tarjeta|num[ée]ro de (la )?carte)\b/],
  ['ccName', /\b(name on (the )?card|card ?holder|holder name|cc ?name|nameoncard|karteninhaber|titular|titulaire)\b/],
  // Month and year in one field ("MM/YY"), then each on its own ("Expiry
  // month"), then any other expiry field.
  ['ccExp', /\b(mm ?(yy|yyyy|jj|aa)|exp ?date|expiration date|expiry date)\b/, true],
  ['ccMonth', /\b(exp\w* ?month|cc ?month|card ?month|month|mm|monat|mes|mois)\b/, true],
  ['ccYear', /\b(exp\w* ?year|cc ?year|card ?year|year|yy|yyyy|jahr|a[nñ]o|ann[ée]e)\b/, true],
  ['ccExp', /\b(expir(y|ation|es)?|valid (thru|through|until)|ablaufdatum|g[uü]ltig bis|caducidad|vencimiento|date d.expiration)\b/, true],
  ['email', /\b(e ?mail|courriel|correo)\b/],
  ['tel', /\b(phone|tel|telephone|mobile|cell|telefon|t[ée]l[ée]phone|tel[ée]fono|handy)\b/],
  ['organization', /\b(company|organi[sz]ation|business|firm|firma|unternehmen|empresa|entreprise|soci[ée]t[ée])\b/],
  ['line2', /\b(address ?(line ?)?2|addr ?2|line ?2|street ?2|apartment|apt|suite|unit|flat|building|adresszusatz|apartamento|appartement|compl[ée]ment)\b/],
  ['line3', /\b(address ?(line ?)?3|addr ?3|line ?3)\b/],
  ['postalCode', /\b(zip( ?code)?|postal ?code|post ?code|postcode|plz|postleitzahl|c[oó]digo postal|code postal)\b/],
  ['city', /\b(city|town|locality|suburb|ort|stadt|ciudad|ville|localit[ée]|municipality)\b/],
  ['region', /\b(state|province|region|county|prefecture|bundesland|provincia|estado|r[ée]gion|d[ée]partement)\b/],
  ['country', /\b(country|nation|land|pa[ií]s|pays)\b/],
  ['givenName', /\b(first ?name|given ?name|fname|forename|vorname|nombre|pr[ée]nom)\b/],
  ['familyName', /\b(last ?name|family ?name|surname|lname|nachname|apellidos?|nom de famille)\b/],
  ['line1', /\b(address ?(line ?)?1|addr ?1|line ?1|street|address|addr|stra(ss|ß)e|anschrift|direcci[oó]n|calle|adresse)\b/],
  ['name', /\b(full ?name|your ?name|name|contact name|nom|nombre completo)\b/]
];
const shown = (field) => !!(field && (field.offsetWidth || field.offsetHeight || field.getClientRects().length))
  && getComputedStyle(field).visibility !== 'hidden';
const textTypes = ['text', 'email', 'tel', 'number'];
const eligible = (field) => (field instanceof HTMLInputElement && textTypes.includes(field.type))
  || field instanceof HTMLSelectElement || field instanceof HTMLTextAreaElement;
// Text of a label, without the field (or a list's options) inside it.
const textOf = (node, field, depth) => {
  let text = '';
  for (const child of node.childNodes) {
    if (child === field || /^(SELECT|OPTION|SCRIPT|STYLE|TEXTAREA)$/.test(child.nodeName)) continue;
    if (child.nodeType === 3) text += child.textContent;
    else if (child.nodeType === 1 && depth < 4) text += ' ' + textOf(child, field, depth + 1);
  }
  return text.slice(0, 200);
};
const describe = (field) => {
  let text = [field.name, field.id, field.getAttribute('placeholder'), field.getAttribute('aria-label'),
    field.getAttribute('title')].join(' ');
  let labelled = false;
  try {
    for (const label of field.labels || []) { text += ' ' + textOf(label, field, 0); labelled = true; }
  } catch (error) { }
  const by = field.getAttribute('aria-labelledby');
  if (by) for (const id of by.split(/\s+/)) {
    const element = id && document.getElementById(id);
    if (element) { text += ' ' + textOf(element, field, 0); labelled = true; }
  }
  // A caption just before the field that is not tied to it.
  if (!labelled && field.previousElementSibling) text += ' ' + textOf(field.previousElementSibling, field, 0);
  return ' ' + text.replace(/([a-z])([A-Z])/g, '$1 $2').toLowerCase().replace(/[_\-.\[\]:*()\/#,]+/g, ' ').replace(/\s+/g, ' ') + ' ';
};
const keyOf = (field) => {
  if (!eligible(field)) return null;
  const words = (field.getAttribute('autocomplete') || '').toLowerCase().trim().split(/\s+/);
  if (words.includes('webauthn')) return null;
  for (let i = words.length - 1; i >= 0; --i) {
    if (tokens[words[i]]) return { key: tokens[words[i]], explicit: true };
    if (ignored.has(words[i]) && words[i] !== 'off') return null;
  }
  if (field instanceof HTMLInputElement && field.type === 'email') return { key: 'email', explicit: true };
  const text = describe(field);
  for (const [key, pattern, card] of rules)
    if (pattern.test(text)) {
      if (key === 'skip') return null;
      // A street field of several lines takes the whole street.
      return { key: key === 'line1' && field instanceof HTMLTextAreaElement ? 'street' : key, explicit: false, card: !!card };
    }
  if (field instanceof HTMLInputElement && field.type === 'tel') return { key: 'tel', explicit: false };
  return null;
};
// The recognised fields of a form (or of the page, for fields in none).
const classify = (scope) => {
  const found = [];
  for (const field of Array.from(scope.querySelectorAll('input, select, textarea')).slice(0, 300)) {
    if (scope === document && field.form) continue;
    const result = keyOf(field);
    if (result) found.push(Object.assign({ field }, result));
  }
  const number = found.some((item) => item.key === 'ccNumber');
  return found.filter((item) => item.explicit || !item.card || number);
};
const scopeOf = (field) => field.form || document;
const hasPassword = (scope) => scope !== document && !!scope.querySelector('input[type=password]');
// Form history: what the user typed into a form's ordinary text fields,
// remembered under the field's name (or id) and offered again under fields
// of that name. Never a sign-in form's fields (passwords are the password
// manager's), a field or form with autocomplete=off, card or one-time-code
// fields, or fields that look like they take a secret.
const historyTypes = ['text', 'search', 'email', 'tel', 'url', 'number'];
const historyNever = /\b(cvc|cvv2?|csc|cid|security code|card verification|verification code|captcha|otp|one time (code|password)|pin|tan|password|passwort|passcode|passphrase|secret|ssn|social security)\b/;
const formlessPassword = () => Array.from(document.querySelectorAll('input[type=password]')).some((field) => !field.form);
// The name a field's values are kept under, or null. cards: the scope's
// card fields (classify), which never count.
const historyName = (field, cards) => {
  if (!(field instanceof HTMLInputElement) || !historyTypes.includes(field.type)) return null;
  const own = (field.getAttribute('autocomplete') || '').toLowerCase().trim();
  if (own.split(/\s+/).some((word) => word === 'off' || word.startsWith('cc-') || word === 'one-time-code'
    || word.endsWith('-password') || word === 'webauthn')) return null;
  // The form's autocomplete=off covers its fields that say nothing.
  if (!own && field.form && (field.form.getAttribute('autocomplete') || '').toLowerCase().trim() === 'off') return null;
  const name = String(field.name || field.id || '').trim();
  if (!name || name.length > 100) return null;
  if (field.form ? hasPassword(field.form) : formlessPassword()) return null;
  if (cards && cards.has(field)) return null;
  if (historyNever.test(describe(field))) return null;
  return name;
};
// Fields the user typed in (a page's own values are not the user's), and
// those filled from the form history.
const typed = new WeakSet();
const tag = '__summitAutoFillFrame';
// Where a child frame's content starts in this frame's viewport.
const frameBox = (source) => {
  for (const element of document.querySelectorAll('iframe, frame')) {
    if (element.contentWindow !== source) continue;
    const rect = element.getBoundingClientRect();
    const style = getComputedStyle(element);
    return { x: rect.left + element.clientLeft + (parseFloat(style.paddingLeft) || 0),
      y: rect.top + element.clientTop + (parseFloat(style.paddingTop) || 0) };
  }
  return null;
};
const waiting = new Map();
let queries = 0;
// This frame's place in the page's viewport, and the page's width.
const placeInPage = () => new Promise((resolve) => {
  if (window === window.top) { resolve({ x: 0, y: 0, width: window.innerWidth }); return; }
  const id = String(++queries) + Math.random().toString(36).slice(2);
  waiting.set(id, resolve);
  setTimeout(() => { if (waiting.delete(id)) resolve(null); }, 1000);
  window.parent.postMessage({ [tag]: 'where', id }, '*');
});
window.addEventListener('message', (event) => {
  const data = event.data;
  if (!data || typeof data !== 'object' || !Object.prototype.hasOwnProperty.call(data, tag)) return;
  event.stopImmediatePropagation();
  const source = event.source;
  if (data[tag] === 'where' && source) {
    const box = frameBox(source);
    if (!box) return;
    placeInPage().then((place) => {
      if (place) source.postMessage({ [tag]: 'here', id: data.id, x: place.x + box.x, y: place.y + box.y, width: place.width }, '*');
    });
  } else if (data[tag] === 'here' && source === window.parent) {
    const resolve = waiting.get(data.id);
    if (!resolve) return;
    waiting.delete(data.id);
    resolve({ x: Number(data.x) || 0, y: Number(data.y) || 0, width: Number(data.width) || 0 });
  }
}, true);
const two = (number) => String(number).padStart(2, '0');
const norm = (text) => String(text == null ? '' : text).trim().toLowerCase().replace(/\s+/g, ' ');
const regionNames = (code) => {
  const names = [];
  for (const language of ['en', navigator.language]) {
    try { names.push(new Intl.DisplayNames([language], { type: 'region' }).of(code)); } catch (error) { }
  }
  return names;
};
// As the user's typing does: frameworks listen for input and change.
const dispatch = (field) => {
  field.dispatchEvent(new Event('input', { bubbles: true }));
  field.dispatchEvent(new Event('change', { bubbles: true }));
};
const setValue = (field, value) => {
  const prototype = field instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
  Object.getOwnPropertyDescriptor(prototype, 'value').set.call(field, value);
  dispatch(field);
  return true;
};
const setSelect = (select, key, value) => {
  const options = Array.from(select.options);
  let match = null;
  if (key === 'ccMonth') {
    match = options.find((option) => parseInt(option.value, 10) === value)
      || options.find((option) => parseInt(option.textContent, 10) === value);
  } else if (key === 'ccYear') {
    const names = [String(value), two(value % 100)];
    match = options.find((option) => names.includes(option.value.trim())) || options.find((option) => names.includes(option.textContent.trim()));
  } else {
    const wanted = new Set([norm(value)]);
    if (key === 'country') {
      if (/^[a-z]{2}$/i.test(value)) for (const name of regionNames(String(value).toUpperCase())) wanted.add(norm(name));
      for (const option of options) {
        const code = option.value.trim();
        if (/^[a-z]{2}$/i.test(code) && regionNames(code.toUpperCase()).some((name) => norm(name) === norm(value))) wanted.add(norm(code));
      }
    }
    match = options.find((option) => wanted.has(norm(option.value)) || wanted.has(norm(option.textContent)));
  }
  if (!match || match.disabled) return false;
  if (select.value !== match.value) {
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(select, match.value);
    dispatch(select);
  }
  return true;
};
// Fields AutoFill filled, which choosing another entry may change again;
// what the user typed stays.
const filled = new WeakSet();
const fill = (focused, group, values) => {
  const name = [values.givenName, values.familyName].filter(Boolean).join(' ');
  const lines = String(values.street || '').split('\n').map((line) => line.trim()).filter(Boolean);
  const cardName = String(values.ccName || '').trim();
  const cardWords = cardName ? cardName.split(/\s+/) : [];
  const month = Number(values.ccMonth) || 0, year = Number(values.ccYear) || 0;
  const third = group.some((item) => item.key === 'line3');
  let count = 0;
  for (const { field, key } of group) {
    if (field.disabled || field.readOnly || !shown(field)) continue;
    const select = field instanceof HTMLSelectElement;
    if (!select && field !== focused && field.value && !filled.has(field)) continue;
    let value;
    switch (key) {
    case 'name': value = name; break;
    case 'street': value = field instanceof HTMLTextAreaElement ? lines.join('\n') : lines.join(', '); break;
    case 'line1': value = lines[0]; break;
    case 'line2': value = lines.slice(1, third ? 2 : undefined).join(', '); break;
    case 'line3': value = lines.slice(2).join(', '); break;
    case 'ccGivenName': value = cardWords.length > 1 ? cardWords.slice(0, -1).join(' ') : cardName; break;
    case 'ccFamilyName': value = cardWords.length > 1 ? cardWords[cardWords.length - 1] : ''; break;
    case 'ccMonth': value = month ? (select ? month : two(month)) : ''; break;
    case 'ccYear':
      value = !year ? '' : select ? year
        : (field.maxLength === 2 || /^\s*yy\s*$/i.test(field.placeholder || '')) ? two(year % 100) : String(year);
      break;
    case 'ccExp': {
      const long = /yyyy/i.test(field.placeholder || '') || field.maxLength >= 7;
      value = month && year ? two(month) + '/' + (long ? String(year) : two(year % 100)) : '';
      break;
    }
    default: value = values[key];
    }
    if (value === undefined || value === null || value === '') continue;
    if (select ? setSelect(field, key, value) : setValue(field, String(value))) { filled.add(field); ++count; }
  }
  return count;
};
let offered = null, offers = 0, offeredAt = 0;
const leave = () => {
  if (!offered) return;
  offered = null;
  ++offers;
  post({ type: 'leave' }).catch(() => {});
};
const offer = (field) => {
  if (!(eligible(field) || (field instanceof HTMLInputElement && historyTypes.includes(field.type)))) return;
  if (field.disabled || field.readOnly || !shown(field)) return;
  const scope = scopeOf(field);
  // An address or card form's field offers its entries; any other text
  // field (and one of those, when nothing saved fits) its form history.
  let kind = null, mine = null, group = [], keys = [], card = false;
  if (keyOf(field) && !hasPassword(scope)) {
    const all = classify(scope);
    mine = all.find((item) => item.field === field);
    if (mine) {
      card = cardKeys.has(mine.key);
      group = all.filter((item) => cardKeys.has(item.key) === card);
      keys = Array.from(new Set(group.map((item) => item.key)));
      // A lone field that only looks like a name or a city is not a form to fill.
      if (mine.explicit || (card ? keys.includes('ccNumber') : keys.length >= 2)) kind = card ? 'card' : 'address';
    }
  }
  const history = card ? null : historyName(field, null);
  if (!kind && !history) return;
  const serial = ++offers;
  offered = field;
  offeredAt = Date.now();
  const rect = field.getBoundingClientRect();
  placeInPage().then((place) => {
    if (!place || !place.width || serial !== offers) return '';
    return post({ type: 'field', kind: kind || 'history', field: mine ? mine.key : '', keys, history: history || '',
      text: String(field.value || '').slice(0, 200),
      left: rect.left + place.x, top: rect.top + place.y, right: rect.right + place.x, bottom: rect.bottom + place.y,
      width: place.width });
  }).then((reply) => {
    if (!reply || serial !== offers) return;
    let values = null;
    try { values = JSON.parse(reply); } catch (error) { return; }
    offered = null;
    if (!values || typeof values !== 'object') return;
    // A remembered value: this field only.
    if (typeof values.historyValue === 'string') {
      if (setValue(field, values.historyValue)) typed.add(field);
    } else fill(field, group, values);
  }).catch(() => {});
};
document.addEventListener('focusin', (event) => { if (event.target !== offered) offer(event.target); }, true);
// Back into a field that has the focus already: the list again.
document.addEventListener('mousedown', (event) => {
  if (event.target === document.activeElement && !offered) offer(event.target);
}, true);
document.addEventListener('keydown', (event) => {
  if (event.key === 'ArrowDown' && !offered && event.target === document.activeElement && !event.altKey && !event.ctrlKey)
    offer(event.target);
}, true);
document.addEventListener('focusout', (event) => { if (event.target === offered) leave(); }, true);
// Typing filters the list, which stays open.
document.addEventListener('input', (event) => {
  if (!event.isTrusted || !event.target) return;
  typed.add(event.target);
  if (event.target === offered) post({ type: 'filter', text: String(offered.value || '').slice(0, 200) }).catch(() => {});
}, true);
// A field the page scrolls into view as it gets the focus keeps its list.
window.addEventListener('scroll', () => { if (Date.now() - offeredAt > 400) leave(); }, { capture: true, passive: true });
window.addEventListener('resize', leave, { passive: true });
window.addEventListener('pagehide', leave);
window.addEventListener('blur', (event) => { if (event.target === window) leave(); });
// A submitted form with an address or a card the browser may offer to save.
const luhn = (digits) => {
  let sum = 0;
  for (let i = 0; i < digits.length; ++i) {
    let digit = digits.charCodeAt(digits.length - 1 - i) - 48;
    if (i % 2) { digit *= 2; if (digit > 9) digit -= 9; }
    sum += digit;
  }
  return sum % 10 === 0;
};
let lastSent = '';
const capture = (scope) => {
  if (!scope || !scope.querySelectorAll || hasPassword(scope)) return;
  const raw = {};
  const recognised = classify(scope);
  // What the user typed into the scope's other text fields, for the form
  // history: only values they changed; the browser keeps none longer than
  // 200 characters or that look like a card number either.
  const history = [];
  const cards = new Set(recognised.filter((item) => cardKeys.has(item.key)).map((item) => item.field));
  for (const field of Array.from(scope.querySelectorAll('input')).slice(0, 300)) {
    if ((scope === document && field.form) || !typed.has(field) || !shown(field)) continue;
    const value = String(field.value || '').trim();
    if (!value || value.length > 200 || value === String(field.defaultValue || '').trim()) continue;
    const digits = value.replace(/[\s-]/g, '');
    if (/^\d{12,19}$/.test(digits) && luhn(digits)) continue;
    const name = historyName(field, cards);
    if (name && history.length < 50) history.push([name, value]);
  }
  for (const { field, key } of recognised) {
    if (!shown(field) || raw[key]) continue;
    let value = '';
    if (field instanceof HTMLSelectElement) {
      const option = field.selectedOptions[0];
      if (!option || !option.value) continue;
      // A list's value when it is a code or a name ("IL", "United States"),
      // its text when the value is a number only the site knows.
      value = (key === 'country' || key === 'region') && !/^[a-z][a-z .'-]{0,40}$/i.test(option.value.trim())
        ? (option.textContent.trim() || option.value) : option.value.trim();
    } else value = String(field.value || '').trim();
    if (value && value.length <= 500) raw[key] = value;
  }
  const address = {};
  if (raw.givenName || raw.familyName) {
    address.givenName = raw.givenName || '';
    address.familyName = raw.familyName || '';
  } else if (raw.name) {
    const words = raw.name.split(/\s+/);
    address.givenName = words.length > 1 ? words.slice(0, -1).join(' ') : raw.name;
    address.familyName = words.length > 1 ? words[words.length - 1] : '';
  }
  const lines = raw.street ? raw.street.split(/\r?\n/) : [raw.line1, raw.line2, raw.line3];
  address.street = lines.map((line) => String(line || '').trim()).filter(Boolean).join('\n');
  for (const key of ['organization', 'city', 'region', 'postalCode', 'country', 'email', 'tel']) if (raw[key]) address[key] = raw[key];
  let card = null;
  const digits = String(raw.ccNumber || '').replace(/[\s-]/g, '');
  if (/^\d{12,19}$/.test(digits) && luhn(digits)) {
    card = { number: digits, name: raw.ccName || [raw.ccGivenName, raw.ccFamilyName].filter(Boolean).join(' ') };
    let month = parseInt(raw.ccMonth, 10) || 0, year = parseInt(raw.ccYear, 10) || 0;
    const expiry = /^\s*(\d{1,2})\s*[\/\-. ]\s*(\d{4}|\d{2})\s*$/.exec(raw.ccExp || '');
    if (expiry) { month = parseInt(expiry[1], 10); year = parseInt(expiry[2], 10); }
    if (year > 0 && year < 100) year += 2000;
    if (month >= 1 && month <= 12) card.month = month;
    if (year >= 2000 && year < 2100) card.year = year;
  }
  const hasAddress = Object.keys(address).some((key) => address[key]);
  if (!hasAddress && !card && !history.length) return;
  const message = { type: 'submit', address: hasAddress ? address : null, card, history };
  const key = JSON.stringify(message);
  if (key === lastSent) return;
  lastSent = key;
  post(message).catch(() => {});
};
document.addEventListener('submit', (event) => capture(event.target), true);
// Pages that send their forms with script: a click on a button, or Enter.
document.addEventListener('click', (event) => {
  const button = event.target && event.target.closest && event.target.closest('button, input[type=submit], input[type=button], [role=button]');
  if (button) setTimeout(() => capture(button.form || document), 0);
}, true);
document.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && event.target instanceof HTMLInputElement) capture(event.target.form || document);
}, true);
})();)js";
}
