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
HTMLVideoElement.prototype.requestPictureInPicture = function () {
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
}
