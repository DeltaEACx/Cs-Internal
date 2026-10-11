export class CharacterPreview {
  constructor({ request, canPoll, error, isConnected }) {
    this.request = request; this.canPoll = canPoll; this.error = error; this.isConnected = isConnected; this.busy = false; this.opened = false; this.rotation = false; this.drag = null; this.accumulated = { dx: 0, dy: 0 }; this.wheel = 0; this.commands = [];
    const $ = id => document.getElementById(id); this.$ = $;
    this.opened = true;
    $('preview-rotate').addEventListener('click', () => this.command({ action: 'rotate', value: !this.rotation }));
    $('preview-reset').addEventListener('click', () => this.command({ action: 'reset' })); $('preview-reload').addEventListener('click', () => this.command({ action: 'reload' }));
    $('preview-equipped').addEventListener('click', () => this.command({ action: 'equipped' }));
    const stage = $('preview-stage');
    stage.addEventListener('pointerdown', event => { if (event.button !== 0 || !this.isConnected()) return; this.drag = { x: event.clientX, y: event.clientY }; stage.setPointerCapture(event.pointerId); stage.classList.add('dragging'); });
    stage.addEventListener('pointermove', event => { if (!this.drag) return; this.accumulated.dx += event.clientX - this.drag.x; this.accumulated.dy += event.clientY - this.drag.y; this.drag = { x: event.clientX, y: event.clientY }; });
    const release = () => { this.drag = null; stage.classList.remove('dragging'); }; stage.addEventListener('pointerup', release); stage.addEventListener('pointercancel', release);
    stage.addEventListener('wheel', event => { if (!this.isConnected()) return; event.preventDefault(); this.wheel += Math.sign(-event.deltaY); }, { passive: false });
    stage.addEventListener('keydown', event => { const directions = { ArrowLeft: [-12, 0], ArrowRight: [12, 0], ArrowUp: [0, -12], ArrowDown: [0, 12] }; if (directions[event.key]) { event.preventDefault(); this.accumulated.dx += directions[event.key][0]; this.accumulated.dy += directions[event.key][1]; } else if (event.key === '+' || event.key === '=') { event.preventDefault(); this.wheel++; } else if (event.key === '-') { event.preventDefault(); this.wheel--; } });
    setInterval(() => this.tick(), 125);
  }

  setOpen(open) {
    this.opened = true; this.$('character-preview').hidden = false; this.tick();
  }

  setConnected(connected) {
    for (const id of ['preview-rotate', 'preview-reset', 'preview-reload', 'preview-equipped']) this.$(id).disabled = !connected;
    if (!connected) { this.$('preview-image').hidden = true; this.status('Waiting for game'); }
  }

  status(message) { this.$('preview-status').textContent = message; this.$('preview-empty').hidden = false; }

  async tick() {
    if (!this.opened || this.busy || !this.canPoll() || !this.isConnected()) return;
    let command = {};
    if (this.accumulated.dx || this.accumulated.dy) { command = { action: 'drag', dx: Math.max(-2000, Math.min(2000, this.accumulated.dx)), dy: Math.max(-2000, Math.min(2000, this.accumulated.dy)) }; this.accumulated = { dx: 0, dy: 0 }; }
    else if (this.wheel) { command = { action: 'zoom', value: Math.max(-20, Math.min(20, this.wheel)) }; this.wheel = 0; }
    await this.command(command, true);
  }

  async command(command, quiet = false) {
    if (!this.isConnected()) return;
    if (this.busy) { if (!quiet) this.commands.push(command); return; }
    this.busy = true;
    try {
      const stage = this.$('preview-stage').getBoundingClientRect();
      const scale = Math.min(2, 1024 / Math.max(stage.width, stage.height, 1));
      const width = Math.max(128, Math.min(1024, Math.round(stage.width * scale)));
      const height = Math.max(128, Math.min(1024, Math.round(stage.height * scale)));
      const response = await this.request({ op: 'preview', width, height, ...command });
      this.$('preview-name').textContent = response.name || '';
      this.$('preview-weapon').textContent = response.weapon || '';
      this.rotation = !!response.rotate; this.$('preview-rotate').setAttribute('aria-checked', this.rotation);
      const usable = typeof response.image === 'string' && response.image.startsWith('data:image/png;base64,');
      if (usable) { this.$('preview-image').src = response.image; this.$('preview-image').hidden = false; this.$('preview-empty').hidden = true; }
      else { this.$('preview-image').hidden = true; this.status(response.message || ({ loading: 'Loading character...', empty: 'Choose a character to preview.', error: 'Preview is unavailable.' })[response.status] || 'Waiting for a character.'); }
    } catch (error) { this.$('preview-image').hidden = true; this.status('Preview is unavailable.'); if (!quiet) this.error(error); }
    finally { this.busy = false; if (this.opened && this.commands.length) this.command(this.commands.shift()); }
  }
}
