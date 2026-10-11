// Browser regression coverage for the user menu. No game process is opened.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const require = createRequire(new URL('../gui/package.json', import.meta.url));
const { chromium } = require('playwright-core');
const root = fileURLToPath(new URL('../', import.meta.url));
const output = join(root, 'tests/artifacts/gui');
const executablePath = [
  process.env.GUI_TEST_BROWSER,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
].find(path => path && existsSync(path));
assert(executablePath, 'Install Chrome/Edge or provide GUI_TEST_BROWSER.');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath, headless: true });
const page = await browser.newPage({ viewport: { width: 1080, height: 720 } });
const errors = [];
page.on('pageerror', error => errors.push(error.message));
page.setDefaultTimeout(15000);
try {
  await page.goto(`${process.argv[2] || 'http://127.0.0.1:1420'}/?preview=1`);
  await page.waitForFunction(() => !!window.__VELORA_PREVIEW__?.cache.size);
  assert.deepEqual(await page.locator('#main-nav button').allTextContents(),
    ['Combat', 'Movement', 'Players', 'Local', 'Skins', 'World', 'Effects', 'View']);
  await page.evaluate(() => window.__VELORA_PREVIEW__.selectPage(23));
  assert.deepEqual(await page.locator('#sub-nav button').allTextContents(), ['Rage', 'Anti Aim']);
  const enabled = page.getByRole('switch', { name: 'Enable ragebot', exact: true });
  await enabled.click();
  await page.waitForFunction(() => window.__VELORA_PREVIEW__.cache.get(23)?.controls.find(control => control.id === 'hvh-enabled')?.value === true);
  await page.waitForTimeout(950);
  assert.equal(await enabled.getAttribute('aria-checked'), 'true');
  await enabled.click();
  await page.waitForFunction(() => window.__VELORA_PREVIEW__.cache.get(23)?.controls.find(control => control.id === 'hvh-enabled')?.value === false);
  console.log('PASS navigation and master switch state across polling');

  await page.evaluate(() => window.__VELORA_PREVIEW__.selectPage(22));
  const originalCard = await page.locator('[data-control-id="model-1"]').elementHandle();
  await page.getByRole('button', { name: '[Ruzh] 2017 leet agent', exact: true }).click();
  await page.waitForFunction(() => document.querySelector('[data-control-id="model-1"]')?.getAttribute('aria-pressed') === 'true');
  assert(await originalCard.evaluate(node => node.isConnected), 'Selection sync preserves the existing catalog DOM');
  await page.waitForTimeout(950);
  assert(await originalCard.evaluate(node => node.isConnected), 'Ordinary polling does not restart the page entry animation');
  for (const [width, height] of [[1080,720],[880,640],[640,480]]) {
    await page.setViewportSize({ width, height });
    await page.waitForFunction(() => [...document.querySelectorAll('#content .page-view,#content .skin-browser')].every(node => Number(getComputedStyle(node).opacity) >= .99));
    const layout = await page.evaluate(() => {
      const content = document.getElementById('content');
      const names = [...content.querySelectorAll('.catalog-name')].map(node => {
        const style = getComputedStyle(node), box = node.getBoundingClientRect();
        return { text: node.textContent, width: box.width, height: box.height, lineHeight: Number.parseFloat(style.lineHeight), overflow: node.scrollWidth > node.clientWidth + 1 };
      });
      return { overflow: document.documentElement.scrollWidth > innerWidth + 1 || content.scrollWidth > content.clientWidth + 1, emptyArt: content.querySelectorAll('.catalog-art').length, names };
    });
    assert.equal(layout.overflow, false, `Horizontal overflow at ${width}px`);
    assert.equal(layout.emptyArt, 0, 'Image-less model choices reserve no artwork space');
    assert(layout.names.length >= 7);
    for (const name of layout.names) {
      assert(name.width >= 90, `${name.text} has a narrow label column at ${width}px`);
      assert(!name.overflow, `${name.text} overflows its card at ${width}px`);
      assert(name.height <= name.lineHeight * 2 + 1, `${name.text} wraps into a vertical column at ${width}px`);
    }
    await page.screenshot({ path: join(output, `models-${width}x${height}.png`) });
  }
  console.log('PASS model selection, full-width labels, and responsive layouts');

  await page.setViewportSize({ width: 1080, height: 720 });
  await page.evaluate(() => window.__VELORA_PREVIEW__.selectPage(6));
  await page.getByRole('button', { name: 'Box options', exact: true }).click();
  await page.getByRole('combobox', { name: 'Style', exact: true }).selectOption({ label: 'Rounded' });
  await page.waitForFunction(() => window.__VELORA_PREVIEW__.cache.get(6)?.controls.find(control => control.id === 'shapes')?.children.find(control => control.id === 'box')?.optionsControls?.find(control => control.id === 'box-style')?.value === '2');
  await page.screenshot({ path: join(output, 'settings-popup-1080x720.png') });
  await page.getByRole('button', { name: 'Close options', exact: true }).click();

  const components = await page.evaluate(async () => {
    const { Controls } = await import('/src/controls.js');
    const host = document.createElement('div'); host.style.cssText = 'position:fixed;left:-2000px;width:400px'; document.body.append(host);
    const calls = [], failures = [], requests = [];
    const controls = new Controls({
      write: (control, value, page) => { calls.push({ op: 'set', page, id: control.id, value }); return Promise.resolve(); },
      action: (control, page) => { calls.push({ op: 'action', page, id: control.id }); return Promise.resolve(); },
      catalog: async () => ({ items: [] }), error: error => failures.push(String(error)),
    });
    const original = [{ id:'original-toggle', type:'bool', label:'Original toggle', value:false }, { id:'original-model', type:'catalog', label:'Original model' }];
    controls.renderPage(original, host, 22);
    const oldToggle = host.querySelector('.switch'), oldModel = host.querySelector('.catalog-card');
    controls.renderPage([], host, 6);
    oldToggle.click(); oldModel.click();
    await Promise.resolve();
    const provenance = calls.slice();

    const single = { id:'single-card',type:'catalog',label:'Selection row',selected:false };
    controls.renderPage([single],host,22);
    controls.sync([{...single,selected:true,disabled:true}]);
    const syncedCard = host.querySelector('.catalog-card');
    const cardState = { selected:syncedCard.getAttribute('aria-pressed'),disabled:syncedCard.disabled,disabledClass:syncedCard.classList.contains('disabled') };

    const choice = { id:'catalog-choice', type:'catalog', label:'Model choice', value:0, options:[{ value:0,label:'First model' },{ value:1,label:'Second model' },{ value:2,label:'Unavailable model',disabled:true }] };
    controls.renderPage([choice], host, 22);
    controls.sync([{ ...choice, value:1 }]);
    const selectedAfterSync = host.querySelector('.catalog-row[aria-pressed="true"]')?.textContent;
    const unavailableDisabled = [...host.querySelectorAll('.catalog-row')].find(node => node.textContent === 'Unavailable model')?.disabled;
    const search = host.querySelector('input[type="search"]'); search.value='Second'; search.dispatchEvent(new Event('input'));
    await new Promise(resolve => setTimeout(resolve,150));
    controls.sync([{ ...choice, value:0 }]);
    const filteredAfterSync = [...host.querySelectorAll('.catalog-row')].map(node => node.textContent);

    controls.write = (control, value, page) => new Promise((resolve,reject) => requests.push({ resolve,reject }));
    const rapid = { id:'rapid-toggle', type:'bool', label:'Rapid toggle', value:false };
    controls.renderPage([rapid], host, 6);
    const toggle = host.querySelector('.switch'); toggle.click(); toggle.click(); toggle.click();
    requests[0].reject(new Error('First change failed')); requests[1].resolve(); requests[2].resolve();
    await new Promise(resolve => setTimeout(resolve,0));
    const latestChecked = toggle.getAttribute('aria-checked');

    requests.length=0;
    const rejected = { id:'rejected-toggle',type:'bool',label:'Rejected toggle',value:false };
    controls.renderPage([rejected],host,6);
    const rejectedToggle=host.querySelector('.switch'); rejectedToggle.click(); rejectedToggle.click();
    requests[0].reject(new Error('First rejected change')); requests[1].reject(new Error('Second rejected change'));
    await new Promise(resolve => setTimeout(resolve,0));
    const rejectedChecked=rejectedToggle.getAttribute('aria-checked');

    controls.renderPage([{ id:'picture', type:'catalog', label:'Weapon with artwork', image:'data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>' }],host,22);
    const imageWidth = host.querySelector('.catalog-art').getBoundingClientRect().width;
    const captionWidth = host.querySelector('.catalog-name').getBoundingClientRect().width;
    controls.dispose(); host.remove();
    return { provenance, cardState, selectedAfterSync, unavailableDisabled, filteredAfterSync, latestChecked, rejectedChecked, failures, imageWidth, captionWidth };
  });
  assert.deepEqual(components.provenance.map(call => [call.op,call.page]), [['set',22],['action',22]], 'Old controls keep their owning page after navigation');
  assert.deepEqual(components.cardState, {selected:'true',disabled:true,disabledClass:true}, 'Catalog selection and disabled state update together');
  assert.match(components.selectedAfterSync, /Second model/);
  assert.equal(components.unavailableDisabled, true, 'Polling does not enable an unavailable catalog option');
  assert.deepEqual(components.filteredAfterSync, ['Second model'], 'Catalog state updates preserve the search filter');
  assert.equal(components.latestChecked, 'true', 'An older failed mutation cannot undo the latest successful change');
  assert.equal(components.rejectedChecked, 'false', 'Two failed mutations restore the last acknowledged value');
  assert.equal(components.failures.length, 3);
  assert(components.imageWidth >= 150 && components.captionWidth >= 150, 'Artwork and captions use the full card width');
  assert.deepEqual(errors, []);
  console.log('PASS mutation provenance, catalog state sync, search, and async rollback');
  console.log(`Screenshots: ${output}`);
} finally { await browser.close(); }
