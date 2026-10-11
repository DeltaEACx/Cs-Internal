import { Controls, element } from './controls.js';
import { Bridge } from './transport.js';
import { CharacterPreview } from './character-preview.js';
import { pages as initialPages, groups as initialGroups, groupOrder } from './pages.js';

const bridge = new Bridge();
const $ = id => document.getElementById(id);
let pages = initialPages, groups = initialGroups.map((label, id) => ({ id, label }));
let currentPage = 6, revision = 0, connected = false, requesting = false, visible = !document.hidden, pageSignature = '', navigationSignature = '', toastTimer;
let initialized = false, loading = true, pollTimer;
let writeQueue = Promise.resolve();
const pending = new Set(), pendingCounts = new Map(), memory = new Map(), cache = new Map();
const normalize = (controls, parentDisabled = false) => {
  const normalized = [];
  for (const control of controls || []) {
    const entry = { ...control, disabled: parentDisabled || !!control.disabled };
    if (entry.type === 'cards') { entry.type = 'group'; entry.wide = true; entry.collection = true; }
    if (entry.type === 'modal') { entry.type = 'group'; entry.wide = true; entry.detail = true; }
    if (entry.children) entry.children = normalize(entry.children, entry.disabled);
    if (entry.type === 'popup') {
      const previous = normalized.at(-1);
      if (previous && previous.type === 'bool') previous.optionsControls = entry.children || [];
      else { entry.type = 'group'; normalized.push(entry); }
      continue;
    }
    if (entry.type === 'number' && !entry.step) entry.step = Number.isInteger(entry.value) && Number.isInteger(entry.min) && Number.isInteger(entry.max) ? 1 : .01;
    if (entry.format && !entry.unit) {
      const suffix = entry.format.replace(/%[-+ #0]*\d*(?:\.\d+)?[a-zA-Z]/g, '').replace(/%%/g, '%').trim();
      entry.unit = ({ degrees: '°', deg: '°', units: 'u' })[suffix] || suffix;
    }
    if (entry.type === 'keybind') entry.display = entry.value === 0 ? (entry.zeroLabel || 'Unbound') : keyLabel(entry.value);
    normalized.push(entry);
  }
  return normalized;
};
const signature = controls => JSON.stringify(controls, (key, value) => ['value', 'display', 'displayValue', 'disabled', 'selected'].includes(key) ? undefined : key === 'image' && typeof value === 'string' ? `${value.length}:${value.slice(-32)}` : value);

function keyLabel(vk) {
  const names = { 0: 'Always', 1: 'Mouse 1', 2: 'Mouse 2', 4: 'Mouse 3', 5: 'Mouse 4', 6: 'Mouse 5', 8: 'Backspace', 9: 'Tab', 13: 'Enter', 16: 'Shift', 17: 'Ctrl', 18: 'Alt', 20: 'Caps Lock', 27: 'Escape', 32: 'Space', 33: 'Page Up', 34: 'Page Down', 35: 'End', 36: 'Home', 37: 'Left', 38: 'Up', 39: 'Right', 40: 'Down', 45: 'Insert', 46: 'Delete' };
  return names[vk] || (vk >= 48 && vk <= 90 ? String.fromCharCode(vk) : vk >= 112 && vk <= 135 ? `F${vk - 111}` : `Key ${vk}`);
}

function notify(error) {
  const message = typeof error === 'string' ? error : error?.message || error?.error || 'The change could not be applied.';
  $('toast').textContent = message; $('toast').classList.add('visible'); clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('toast').classList.remove('visible'), 4800);
}

function connectionState(next, preview = false) {
  connected = next;
  characterPreview.setConnected(next && !preview);
}

function startupState(startup) {
  loading = !!startup && startup.phase !== 'ready';
  $('startup-loading').hidden = !loading;
  $('content').setAttribute('aria-busy', String(loading));
  for (const id of ['main-nav', 'sub-nav', 'content', 'character-preview']) $(id).inert = loading;
  if (loading) {
    connectionState(false);
    const progress = Math.max(0, Math.min(100, Number(startup.progress) || 0));
    $('startup-message').textContent = 'Loading';
    $('startup-progress').style.width = `${progress}%`;
    $('startup-track').setAttribute('aria-valuenow', String(progress));
  }
  return loading;
}

function markStartup(name) {
  const key = `deltaeac:${name}`;
  if (!performance.getEntriesByName(key, 'mark').length) performance.mark(key);
}

function schedulePoll() {
  clearTimeout(pollTimer);
  if (initialized && visible) pollTimer = setTimeout(() => fetchPage(), loading ? 125 : 850);
}

function emptyState(title, description, retry = false) {
  controls.dispose(); pageSignature = '';
  const view = element('div', 'page-view'); const empty = element('div', 'empty-state');
  empty.append(element('h1', '', title), element('p', '', description));
  if (retry) { const button = element('button', 'action-button', 'Reconnect'); button.type = 'button'; button.addEventListener('click', () => fetchPage(true)); empty.append(button); }
  else empty.append(element('div', 'loading-line'));
  view.append(empty); $('content').replaceChildren(view);
}

const controls = new Controls({
  write: (control, value, page) => queueMutation({ op: 'set', page, id: control.id, value }),
  action: (control, page) => queueMutation({ op: 'action', page, id: control.id }),
  catalog: async () => { throw new Error('This catalog does not support remote search.'); },
  changed: () => fetchPage(), error: notify,
});
const characterPreview = new CharacterPreview({ request: request => bridge.request(request), canPoll: () => visible && !requesting && !pending.size, isConnected: () => connected && !bridge.preview, error: notify });

function queueMutation(request) {
  if (!connected) return Promise.reject(new Error('Reconnect before changing a setting.'));
  pending.add(String(request.id));
  pendingCounts.set(String(request.id), (pendingCounts.get(String(request.id)) || 0) + 1);
  const task = writeQueue.catch(() => {}).then(async () => {
    try { const response = await bridge.request(request); acceptResponse(response, false, true); }
    catch (error) { if (['not_connected', 'game_not_running', 'timeout'].includes(error?.code)) connectionState(false); throw error; }
    finally {
      const remaining = (pendingCounts.get(String(request.id)) || 1) - 1;
      if (remaining) pendingCounts.set(String(request.id), remaining);
      else { pending.delete(String(request.id)); pendingCounts.delete(String(request.id)); }
      const page = cache.get(currentPage);
      if (page) controls.sync(page.controls, pending);
    }
  });
  writeQueue = task; return task;
}

function acceptResponse(response, force = false, mutation = false) {
  if (startupState(response.startup)) return;
  if (response.page) markStartup('native-ready');
  connectionState(response.connected !== false, bridge.preview);
  revision = response.revision || revision;
  if (response.previewRequested) characterPreview.setOpen(true);
  if (response.groups?.length) groups = response.groups.map(group => ({ ...group, label: initialGroups[Number(group.id)] ?? group.label }));
  if (response.pages?.length) pages = initialPages.flatMap(entry => {
    const native = response.pages.find(page => Number(page.id) === entry.id);
    return native ? [{ ...native, label: entry.label }] : [];
  });
  if (!response.page) return;
  const page = { ...response.page, controls: normalize(response.page.controls) };
  const previousGroups = new Set((cache.get(Number(page.id))?.controls || []).filter(control => control.type === 'group').map(control => control.id));
  const addedSettings = mutation ? page.controls.find(control => control.type === 'group' && /settings$/i.test(control.label) && !previousGroups.has(control.id)) : null;
  cache.set(Number(page.id), page);
  if (Number(page.id) !== currentPage) return;
  renderNavigation(); const nextSignature = signature(page.controls);
  const editing = $('content').contains(document.activeElement) && document.activeElement.matches('input,select');
  if ((force && !pageSignature) || (nextSignature !== pageSignature && (!editing || mutation))) {
    const scroll = $('content').scrollTop, focusId = document.activeElement?.id;
    controls.renderPage(page.controls, $('content'), currentPage); $('content').scrollTop = scroll; pageSignature = nextSignature;
    if (focusId && mutation) document.getElementById(focusId)?.focus({preventScroll:true});
    if (addedSettings && currentPage > 4) [...$('content').querySelectorAll('[data-group-id]')].find(node => node.dataset.groupId === addedSettings.id)?.scrollIntoView({behavior:'smooth',block:'start'});
  } else controls.sync(page.controls, pending);
  if (!page.controls.length) {
    const loadingCatalog = currentPage >= 0 && currentPage <= 4;
    const notice = element('div', 'empty-state'); notice.append(element('h1', '', loadingCatalog ? 'Loading collection' : 'No settings available'));
    if (loadingCatalog) notice.append(element('div', 'loading-line'));
    else notice.append(element('p', '', 'This page will update when its features are available.'));
    $('content').replaceChildren(notice);
  }
  else markStartup('first-settings');
}

async function fetchPage(force = false) {
  if (!initialized || requesting) return;
  if (pending.size || (!visible && !force)) { schedulePoll(); return; }
  clearTimeout(pollTimer);
  const requestedPage = currentPage; requesting = true;
  try { const response = await bridge.request({ op: 'state', page: requestedPage }); acceptResponse(response, force && requestedPage === currentPage); }
  catch (error) {
    if (requestedPage !== currentPage) return;
    connectionState(false);
    startupState(null);
    const explanation = error?.code === 'desktop_required' ? error.message : 'Open CS2 with the updated DLL, then reconnect.';
    emptyState('Ready when you are', explanation, true);
  } finally { requesting = false; schedulePoll(); }
}

function renderNavigation() {
  const nextSignature = JSON.stringify([currentPage, groups, pages]);
  if (nextSignature === navigationSignature) return;
  navigationSignature = nextSignature;
  const active = pages.find(page => Number(page.id) === currentPage) || pages[0];
  if (!active) return;
  $('page-title').textContent = active.label;
  $('main-nav').replaceChildren();
  [...groups].filter(group => pages.some(page => Number(page.group) === Number(group.id)))
    .sort((a, b) => groupOrder.indexOf(Number(a.id)) - groupOrder.indexOf(Number(b.id))).forEach(group => {
    const button = element('button', `nav-item${Number(group.id) === Number(active.group) ? ' active' : ''}`);
    button.type = 'button'; button.title = group.label; button.setAttribute('aria-label', group.label);
    button.setAttribute('aria-current', Number(group.id) === Number(active.group) ? 'page' : 'false');
    button.append(element('span', 'nav-label', group.label));
    button.addEventListener('click', () => selectPage(memory.get(Number(group.id)) ?? Number(pages.find(page => Number(page.group) === Number(group.id)).id)));
    $('main-nav').append(button);
  });
  $('sub-nav').replaceChildren();
  pages.filter(page => Number(page.group) === Number(active.group)).forEach(page => {
    const tab = element('button', `subtab${Number(page.id) === currentPage ? ' active' : ''}`, page.label); tab.type = 'button'; tab.setAttribute('aria-current', Number(page.id) === currentPage ? 'page' : 'false');
    tab.addEventListener('click', () => selectPage(Number(page.id))); $('sub-nav').append(tab);
  });
}

async function selectPage(id, highlight) {
  if (!pages.some(page => Number(page.id) === id)) return;
  currentPage = id; memory.set(Number(pages.find(page => Number(page.id) === id).group), id); controls.dispose(); pageSignature = ''; renderNavigation();
  $('content').scrollTop = 0;
  const cached = cache.get(id);
  if (cached && connected) { controls.renderPage(cached.controls, $('content'), currentPage); pageSignature = signature(cached.controls); }
  else emptyState('Loading settings', '');
  await writeQueue.catch(() => {});
  while (requesting) await new Promise(resolve => setTimeout(resolve, 20));
  await fetchPage(true);
  if (highlight) {
    const row = [...$('content').querySelectorAll('[data-control-id]')].find(node => node.dataset.controlId === highlight);
    row?.scrollIntoView({ behavior: 'smooth', block: 'center' }); row?.classList.add('highlight');
  }
}

$('minimize').textContent = '−';
async function windowAction(action) { const nativeWindow = window.__TAURI__?.window?.getCurrentWindow?.(); if (nativeWindow) { try { await nativeWindow[action](); } catch (error) { notify(error); } } else if (bridge.preview) notify('Window controls are available in the desktop app.'); }
$('minimize').addEventListener('click', () => windowAction('minimize'));
document.querySelectorAll('[data-tauri-drag-region]').forEach(region => region.addEventListener('mousedown', event => { if (event.button === 0 && event.target.hasAttribute('data-tauri-drag-region')) windowAction('startDragging'); }));
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && !controls.popover && !controls.listenCleanup) { document.activeElement?.blur(); }
});
document.addEventListener('visibilitychange', () => { visible = !document.hidden; if (visible) fetchPage(); else schedulePoll(); });
window.__TAURI__?.event?.listen('velora-visibility', event => { visible = !!event.payload.visible; if (visible) fetchPage(); else schedulePoll(); });
async function initialize() {
  renderNavigation(); emptyState('Connecting', '');
  if (!bridge.preview && window.__TAURI__?.core?.invoke) {
    startupState({ phase: 'loading', progress: 5 });
    try {
      // Two animation frames paint loading before acknowledging the frontend.
      // A hidden window mounts immediately and starts painting when shown.
      if (!document.hidden) await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      markStartup('loading-frame');
      const response = await bridge.request({ op: 'gui_ready' });
      markStartup('gui-ready');
      startupState(response.startup);
    } catch (error) { startupState(null); notify(error); }
  } else startupState(null);
  initialized = true;
  await fetchPage(true);
}
initialize();

// The explicit browser preview is the only context that exposes test inspection.
if (bridge.preview) window.__VELORA_PREVIEW__ = { selectPage, get currentPage() { return currentPage; }, get revision() { return revision; }, cache };
