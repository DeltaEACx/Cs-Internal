export class Bridge {
  constructor() {
    this.preview = new URLSearchParams(location.search).get('preview') === '1';
    this.fixture = null;
    this.queue = Promise.resolve();
  }

  request(request) {
    const task = this.queue.catch(() => {}).then(() => this.send(request));
    this.queue = task;
    return task;
  }

  async send(request) {
    if (this.preview) {
      this.fixture ||= import('./preview.js');
      return (await this.fixture).request(request);
    }
    const invoke = window.__TAURI__?.core?.invoke;
    if (!invoke) throw { code: 'desktop_required', message: 'Open the DeltaEAC desktop app to connect to the game.' };
    const response = await invoke('bridge_request', { request });
    if (!response?.ok) throw { code: 'request_failed', message: response?.error || 'The change was not applied.' };
    return response;
  }
}
