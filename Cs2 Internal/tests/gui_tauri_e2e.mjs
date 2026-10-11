import { createRequire } from 'node:module';
import { writeFile } from 'node:fs/promises';
import assert from 'node:assert/strict';
import {pages as menuPages, groups as menuGroups} from '../gui/src/pages.js';
const require = createRequire(new URL('../gui/package.json', import.meta.url));
const { chromium } = require('playwright-core');
const endpoint = process.argv[2] || 'http://127.0.0.1:9224';
const browser = await chromium.connectOverCDP(endpoint);
const page = browser.contexts().flatMap(context => context.pages()).find(candidate => candidate.url().startsWith('http://tauri.localhost/'));
assert(page, 'The isolated native Tauri WebView was not found.');
page.setDefaultTimeout(15000);
const errors = [];
page.on('pageerror', error => errors.push(error.message));
const report = { source: 'Packaged Tauri WebView -> Rust invoke -> Windows pipe -> production C++ menu fixture', pages: [], checks: [] };
const invoke = request => page.evaluate(request => window.__TAURI__.core.invoke('bridge_request', { request }), request);
const flatten = controls => (controls || []).flatMap(control => [control, ...flatten(control.children)]);
const get = async id => {
  const response = await invoke({ op: 'state', page: id });
  assert.equal(response.ok, true, response.error);
  return response;
};
const find = (snapshot, label, type) => {
  const control = flatten(snapshot.page.controls).find(control => control.label === label && (!type || control.type === type));
  assert(control, `Missing native control ${label}`);
  return control;
};
const waitNative = async (id, label, expected, type) => {
  for (let attempt = 0; attempt < 40; attempt++) {
    const snapshot = await get(id);
    if (JSON.stringify(find(snapshot, label, type).value) === JSON.stringify(expected)) return snapshot;
    await page.waitForTimeout(30);
  }
  assert.fail(`Native ${label} did not acknowledge ${JSON.stringify(expected)}`);
};
const navigate = async id => {
  // Visibility is nested beneath Camera; page IDs remain stable in the native API.
  const destination = menuPages.find(info => info.id === (id === 19 ? 5 : id));
  assert(destination, `Missing visible navigation entry for native page ${id}`);
  await page.locator('#main-nav').getByRole('button', {name: menuGroups[destination.group], exact:true}).click({force:true});
  await page.locator('#sub-nav').getByRole('button', {name: destination.label, exact:true}).click({force:true});
};
try {
  await page.waitForFunction(() => typeof window.__TAURI__?.core?.invoke === 'function', null, { polling: 100 });
  await page.waitForFunction(() => performance.getEntriesByName('deltaeac:first-settings', 'mark').length > 0);
  report.startup = await page.evaluate(() => ({
    hidden: document.hidden,
    stagesMs: Object.fromEntries(['loading-frame', 'gui-ready', 'native-ready', 'first-settings'].map(name =>
      [name, performance.getEntriesByName(`deltaeac:${name}`, 'mark')[0]?.startTime ?? null])),
  }));
  report.checks.push('Packaged frontend recorded loading acknowledgement and first usable native settings timings');
  const first = await get(6);
  assert.equal(first.connected, true);
  assert(first.pages.length > 0, 'The native navigation is empty.');
  assert.equal(new Set(first.pages.map(info => info.id)).size, first.pages.length, 'Native navigation repeats a page ID.');
  assert.deepEqual(first.pages, menuPages, 'Native and frontend navigation metadata differ.');
  assert.deepEqual(first.groups, menuGroups.map((label,id) => ({id,label})));
  let catalogReady = false;
  for (let attempt = 0; attempt < 160; attempt++) {
    if (flatten((await get(0)).page.controls).some(control => control.type === 'catalog')) {
      catalogReady = true; break;
    }
    await page.waitForTimeout(50);
  }
  assert(catalogReady, 'The real item catalog did not complete its asynchronous load.');
  for (const info of first.pages) {
    const state = await get(info.id);
    assert(flatten(state.page.controls).length > 0, `Native page ${info.label} returned no controls after warmup.`);
    report.pages.push({ id: info.id, label: info.label, controls: flatten(state.page.controls).length });
  }
  report.checks.push(`All ${first.pages.length} visible production pages returned through the actual Rust bridge`);
  console.log(`PASS all ${first.pages.length} visible native pages with warmed catalogs`);

  await navigate(6);
  await page.getByRole('switch', { name: 'Box', exact: true }).waitFor();
  const box = find(await get(6), 'Box', 'bool');
  await page.getByRole('switch', { name: 'Box', exact: true }).click({ force: true });
  await waitNative(6, 'Box', !box.value, 'bool');
  report.checks.push('Visible ESP toggle changed the production native setting');
  if (!box.value) {
    await page.getByRole('switch', { name: 'Box', exact: true }).click({ force: true });
    await waitNative(6, 'Box', box.value, 'bool');
  } else {
    await page.getByRole('switch', { name: 'Box', exact: true }).click({ force: true });
    await waitNative(6, 'Box', true, 'bool');
  }

  const boxPopup = find(await get(6), 'Box', 'popup');
  const boxStyle = boxPopup.children.find(control => control.label === 'Style');
  const newStyle = boxStyle.options.find(option => option.value !== boxStyle.value && !option.disabled);
  await page.getByRole('button', { name: 'Box options', exact: true }).click({ force: true });
  await page.getByRole('combobox', { name: 'Style', exact: true }).selectOption({ label: newStyle.label });
  await waitNative(6, 'Style', newStyle.value, 'select');
  const closeOptions = page.getByRole('button', { name: 'Close options', exact: true });
  if (await closeOptions.count()) await closeOptions.click({ force: true });
  assert.equal((await invoke({ op: 'set', page: 6, id: boxStyle.id, value: boxStyle.value })).ok, true);
  report.checks.push('Three-dot ESP popup and style dropdown changed the native box style');
  console.log('PASS native ESP toggle and three-dot style selection');

  await navigate(19);
  await page.getByRole('switch', { name: 'Hide default crosshair', exact: true }).waitFor();
  const view = await get(19);
  const crosshair = find(view, 'Hide default crosshair', 'bool');
  await page.getByRole('switch', { name: 'Hide default crosshair', exact: true }).click({ force: true });
  await waitNative(19, 'Hide default crosshair', !crosshair.value, 'bool');
  const fov = find(await get(28), 'Viewmodel FOV', 'number');
  const changedFov = fov.value === 72 ? 71 : 72;
  await navigate(28);
  const numeric = page.getByRole('spinbutton', { name: 'Viewmodel FOV value', exact: true });
  await numeric.fill(String(changedFov));
  await numeric.press('Enter');
  await waitNative(28, 'Viewmodel FOV', changedFov, 'number');
  report.checks.push('Navigation, boolean, and typed slider value acknowledged by production C++ settings');

  const positionEnabled = find(await get(28), 'Custom viewmodel position', 'bool');
  if (!positionEnabled.value) {
    await page.getByRole('switch', { name: 'Custom viewmodel position', exact: true }).click({ force: true });
    await waitNative(28, 'Custom viewmodel position', true, 'bool');
  }
  const vector = find(await get(28), 'Gun offset X / Y / Z', 'vector');
  const changedVector = [...vector.value]; changedVector[0] = changedVector[0] === 1 ? 2 : 1;
  const x = page.getByRole('spinbutton', { name: 'Gun offset X / Y / Z X', exact: true });
  await x.fill(String(changedVector[0])); await x.press('Tab');
  await waitNative(28, 'Gun offset X / Y / Z', changedVector, 'vector');
  report.checks.push('Vector component edit preserved the remaining native components');
  console.log('PASS native viewmodel toggle, numeric slider, and vector field');

  const bad = await invoke({ op: 'set', page: 28, id: fov.id, value: 1000000 });
  assert.equal(bad.ok, false);
  assert.equal(find(await get(28), 'Viewmodel FOV', 'number').value, changedFov);
  assert.equal((await invoke({ op: 'state', page: 4294967296 })).ok, false);
  report.checks.push('Invalid range and oversized page index rejected without mutating native state');

  for (const [pageId, control, value] of [[19, crosshair, crosshair.value], [28, fov, fov.value], [28, vector, vector.value], [28, positionEnabled, positionEnabled.value]]) {
    assert.equal((await invoke({ op: 'set', page: pageId, id: control.id, value })).ok, true);
  }
  // The GUI refreshes from native state on its poll interval; no manual refresh control remains.
  await page.waitForFunction(expected => document.querySelector('[aria-label="Viewmodel FOV value"]')?.value === String(expected), fov.value, { polling: 100 });
  await navigate(3);
  const search = page.getByRole('searchbox', { name: 'Search collection...', exact: true });
  await search.fill('SAS'); await search.press('Enter');
  const agentPage = await waitNative(3, 'Search collection...', 'SAS', 'text');
  const candidate = flatten(agentPage.page.controls).find(control => control.type === 'catalog' && control.label.includes('SAS') && !control.disabled);
  assert(candidate, 'The native agent search did not return an SAS character.');
  await page.getByRole('button', { name: candidate.label, exact: true }).click({ force: true });
  await page.getByRole('button', { name: 'Preview agent', exact: true }).click({ force: true });
  await page.getByRole('button', { name: 'Back to items', exact: true }).click({ force: true });
  await page.waitForFunction(() => {
    const image = document.getElementById('preview-image');
    return image && !image.hidden && image.complete && image.naturalWidth > 100;
  }, null, { timeout: 20000, polling: 100 });
  const beforeRotate = await invoke({ op: 'preview' });
  assert.equal(beforeRotate.status, 'ready', beforeRotate.message);
  assert(beforeRotate.image.startsWith('data:image/png;base64,'));
  await page.locator('#preview-rotate').click({ force: true });
  for (let attempt = 0; attempt < 30; attempt++) {
    if ((await invoke({ op: 'preview' })).rotate !== beforeRotate.rotate) break;
    await page.waitForTimeout(25);
    if (attempt === 29) assert.fail('The preview Rotate switch did not update the native renderer.');
  }
  const invalidPreview = await invoke({ op: 'preview', action: 'zoom', value: 1000 });
  assert.equal(invalidPreview.ok, false);
  report.checks.push('Live catalog search and agent preview action produced a real native 3D PNG in Tauri');
  report.checks.push('Preview Rotate control changed renderer state and out-of-range camera input was rejected');
  const ping = await invoke({ op: 'ping' });
  assert.equal(ping.ok, true);
  assert.equal(ping.menuVisible, false, 'Hidden test window must release game input.');
  report.checks.push('Hidden companion session reports no game-input ownership');
  assert.equal(errors.length, 0, errors.join('\n'));
  report.checks.push('No WebView JavaScript exceptions');
  await page.screenshot({ path: 'tests/artifacts/velora_tauri_native.png' });
  report.ok = true;
  await writeFile('tests/artifacts/gui-tauri-native-e2e.json', JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report, null, 2));
} finally {
  await browser.close();
}
