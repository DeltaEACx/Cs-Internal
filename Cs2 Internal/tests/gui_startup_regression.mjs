// Startup scheduling proof with the production app and a controlled native
// boundary. No game process is opened. Optional --baseline records old timing.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const require = createRequire(new URL('../gui/package.json', import.meta.url));
const { chromium } = require('playwright-core');
const base = process.argv.find(argument => /^https?:/.test(argument)) || 'http://127.0.0.1:1420';
const baseline = process.argv.includes('--baseline');
const executablePath = [process.env.GUI_TEST_BROWSER,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(path => path && existsSync(path));
assert(executablePath, 'Install Chrome/Edge or provide GUI_TEST_BROWSER.');
const browser = await chromium.launch({ executablePath, headless: true });
const errors = [];
const measurements = [];

async function fixture(options = {}) {
  const page = await browser.newPage();
  page.setDefaultTimeout(10000);
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(options => {
    const fixture = window.__STARTUP_FIXTURE__ = {
      hidden: !!options.hidden, active: 0, maxActive: 0, calls: [], nativeReadyAt: null,
      firstSettingsAt: null, guiReadyCompletedAt: null,
    };
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => fixture.hidden });
    new MutationObserver(() => {
      if (fixture.firstSettingsAt == null && document.querySelector('[data-control-id="startup-switch"]') &&
          document.getElementById('startup-loading')?.hidden) fixture.firstSettingsAt = performance.now();
    }).observe(document, { subtree: true, childList: true, attributes: true });
    window.__TAURI__ = { core: { invoke: async (command, { request }) => {
      if (command !== 'bridge_request') throw new Error('Unexpected native command');
      const started = performance.now();
      fixture.calls.push({ op: request.op, started });
      ++fixture.active; fixture.maxActive = Math.max(fixture.maxActive, fixture.active);
      try {
        if (request.op === 'gui_ready') fixture.nativeReadyAt = started + (options.readyDelay ?? 210);
        await new Promise(resolve => setTimeout(resolve,
          request.op === 'gui_ready' ? (options.handshakeDelay ?? 8) :
            request.op === 'state' ? (options.stateDelay ?? 8) : 8));
        if (request.op === 'gui_ready') fixture.guiReadyCompletedAt = performance.now();
        if (request.op === 'preview') return { ok: true, connected: true, status: 'empty' };
        const loading = fixture.nativeReadyAt == null || performance.now() < fixture.nativeReadyAt;
        if (request.op !== 'state' || loading)
          return { ok: true, connected: true, startup: { phase: loading ? 'loading' : 'ready', progress: loading ? 5 : 100 } };
        return { ok: true, connected: true, revision: 1,
          page: { id: request.page, controls: [{ id: 'startup-switch', type: 'bool', label: 'Ready setting', value: true }] } };
      } finally { --fixture.active; }
    } } };
  }, options);
  await page.goto(base);
  return page;
}

try {
  for (let repeat = 0; repeat < 5; ++repeat) {
    const page = await fixture();
    try { await page.waitForFunction(() => window.__STARTUP_FIXTURE__.firstSettingsAt != null); }
    catch(error) {
      console.error(await page.evaluate(() => ({ fixture: window.__STARTUP_FIXTURE__, content: document.getElementById('content')?.textContent, loading: document.getElementById('startup-loading')?.hidden })), errors);
      throw error;
    }
    const result = await page.evaluate(() => {
      const state = window.__STARTUP_FIXTURE__;
      return { nativeReadyAt: state.nativeReadyAt, firstSettingsAt: state.firstSettingsAt,
        propagationMs: state.firstSettingsAt - state.nativeReadyAt, maxActive: state.maxActive,
        stateCalls: state.calls.filter(call => call.op === 'state').length,
        startupCalls: state.calls.filter(call => call.op === 'startup').length,
        handshakeCalls: state.calls.filter(call => call.op === 'gui_ready').length };
    });
    assert.equal(result.maxActive, 1, 'Startup requests overlapped the native boundary');
    assert.equal(result.handshakeCalls, 1, 'Loading-paint handshake was repeated');
    if (!baseline) {
      assert.equal(result.startupCalls, 0, 'Startup state was requested redundantly before the handshake');
      assert(result.propagationMs < 300, `Native readiness took ${result.propagationMs.toFixed(1)}ms to reach the menu`);
      const count = result.stateCalls;
      await page.waitForTimeout(450);
      assert.equal(await page.evaluate(() => window.__STARTUP_FIXTURE__.calls.filter(call => call.op === 'state').length), count,
        'Ready state kept polling at the accelerated startup cadence');
    }
    measurements.push(result); await page.close();
  }
  if (!baseline) {
    const alreadyReady = await fixture({ readyDelay: 0 });
    await alreadyReady.waitForFunction(() => window.__STARTUP_FIXTURE__.firstSettingsAt != null);
    const immediate = await alreadyReady.evaluate(() => {
      const state = window.__STARTUP_FIXTURE__;
      return { stateCalls: state.calls.filter(call => call.op === 'state').length,
        afterHandshakeMs: state.firstSettingsAt - state.guiReadyCompletedAt };
    });
    assert.equal(immediate.stateCalls, 1, 'Native work completed first but settings waited for an extra startup poll');
    assert(immediate.afterHandshakeMs < 150, 'Ready handshake did not immediately fetch usable settings');
    await alreadyReady.close();

    const hidden = await fixture({ hidden: true });
    await hidden.waitForFunction(() => window.__STARTUP_FIXTURE__.guiReadyCompletedAt != null);
    await hidden.waitForTimeout(450);
    assert.equal(await hidden.evaluate(() => window.__STARTUP_FIXTURE__.calls.filter(call => call.op === 'state').length), 1,
      'An initially hidden menu repeatedly polled startup state');
    await hidden.evaluate(() => { window.__STARTUP_FIXTURE__.hidden = false; document.dispatchEvent(new Event('visibilitychange')); });
    await hidden.getByRole('switch', { name: 'Ready setting', exact: true }).waitFor();
    await hidden.close();

    const slow = await fixture({ handshakeDelay: 1100, readyDelay: 1800, stateDelay: 220 });
    await slow.waitForFunction(() => window.__STARTUP_FIXTURE__.calls.some(call => call.op === 'gui_ready'));
    assert.deepEqual(await slow.evaluate(() => ['main-nav','sub-nav','content','character-preview'].map(id => document.getElementById(id).inert)),
      [true,true,true,true], 'Native controls became interactive before the slow readiness handshake completed');
    await slow.waitForFunction(() => window.__STARTUP_FIXTURE__.firstSettingsAt != null);
    const serialized = await slow.evaluate(() => {
      const fixture = window.__STARTUP_FIXTURE__;
      return { maxActive: fixture.maxActive, guiReadyCompletedAt: fixture.guiReadyCompletedAt,
        states: fixture.calls.filter(call => call.op === 'state').map(call => call.started) };
    });
    assert.equal(serialized.maxActive, 1, 'Slow native responses caused overlapping requests');
    assert(serialized.states.every(start => start >= serialized.guiReadyCompletedAt),
      'State requests began before the mount handshake completed');
    assert(serialized.states.length >= 2);
    for (let index = 1; index < serialized.states.length; ++index)
      assert(serialized.states[index] - serialized.states[index - 1] >= 320,
        'Startup polls stacked behind a slow request instead of waiting for completion');
    await slow.close();
  }
  assert.deepEqual(errors, []);
  const report = { source: 'Production frontend with controlled native ready transition', baseline,
    medianPropagationMs: [...measurements].sort((left, right) => left.propagationMs - right.propagationMs)[2].propagationMs,
    measurements, ok: true };
  const output = join(fileURLToPath(new URL('../', import.meta.url)), 'tests/artifacts/gui');
  await mkdir(output, { recursive: true });
  await writeFile(join(output, `startup-${baseline ? 'baseline' : 'current'}.json`), JSON.stringify(report, null, 2));
  console.log(`PASS startup propagation median ${report.medianPropagationMs.toFixed(1)}ms${baseline ? ' (baseline)' : '; hidden startup, bounded polling and serialized slow responses'}`);
} finally { await browser.close(); }
