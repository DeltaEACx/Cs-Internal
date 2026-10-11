
export const element = (tag, className, text) => {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
};
const button = (className, label) => { const node = element('button', className, label); node.type = 'button'; return node; };
const number = (value, fallback = 0) => Number.isFinite(Number(value)) ? Number(value) : fallback;
const decimals = step => Math.min(5, (String(step).split('.')[1] || '').length);
const clamp = (value, min, max) => Math.max(min, Math.min(max, value));
const safeId = value => `field-${String(value).replace(/[^\w-]/g, '-')}`;
const colorCss = value => `rgba(${value.slice(0, 3).map(channel => Math.round(channel * 255)).join(',')},${value[3] ?? 1})`;
const colorHex = value => `#${value.slice(0, 3).map(channel => Math.round(clamp(channel, 0, 1) * 255).toString(16).padStart(2, '0')).join('')}`;
// Short visual labels keep full wording for accessible names and hover help.
const shortLabel = label => ({
  'Enable overlays': 'Enabled', 'Offscreen markers': 'Offscreen', 'Head marker': 'Head',
  'Status labels': 'Status', 'Visible material': 'Visible', 'Behind cover': 'Occluded',
  'Information': 'Info', 'Activation key': 'Key', 'Field of view': 'FOV',
  'Minimum damage': 'Min. damage', 'Automatic fire': 'Auto fire',
  'Recoil correction': 'Recoil', 'Wall penetration': 'Penetration',
  'Back to items': 'Back', 'Equip finish': 'Equip', 'Apply stock knife': 'Use stock',
}[label] || label);

export class Controls {
  constructor({ write, action, changed, catalog, error }) {
    this.write = write; this.action = action; this.changed = changed; this.catalog = catalog; this.error = error;
    this.bindings = new Map(); this.popover = null; this.listenCleanup = null;
    document.addEventListener('pointerdown', event => {
      if (this.popover && !this.popover.node.contains(event.target) && !this.popover.anchor.contains(event.target)) this.closePopover();
    });
    window.addEventListener('resize', () => this.closePopover());
    document.addEventListener('keydown', event => { if (event.key === 'Escape') this.closePopover(); });
    document.addEventListener('keydown', event => {
      if (event.key !== 'Tab' || !this.popover) return;
      const focusable = [...this.popover.node.querySelectorAll('button:not(:disabled),input:not(:disabled),select:not(:disabled),[tabindex="0"]')];
      const first = focusable[0], last = focusable.at(-1);
      if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last?.focus(); }
      else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first?.focus(); }
    });
  }

  dispose() { this.closePopover(); this.listenCleanup?.(); this.bindings.clear(); }

  renderPage(controls, container, page) {
    this.dispose();
    this.page = page;
    this.pageControls = controls;
    const skins = page >= 0 && page <= 4;
    container.classList.toggle('skin-page', skins);
    if (skins) { this.renderSkins(controls, container, page); return; }
    const grid = element('div', 'page-view');
    if (page === 20) {
      const names = ['Flashbang','HE Grenade','Smoke','Molotov','Decoy','Incendiary'];
      const throwables = controls.filter(control => control.type === 'group' && names.includes(control.label));
      if (throwables.length) {
        const general = this.section({ children: controls.filter(control => !throwables.includes(control)) });
        general.classList.add('flat'); grid.append(general);
        const selected = throwables.find(control => control.label === this.throwable) || throwables[0];
        this.throwable = selected.label;
        const tabs = element('nav', 'throwable-tabs'); tabs.setAttribute('aria-label', 'Throwable types');
        for (const control of throwables) {
          const tab = button(`subtab${control === selected ? ' active' : ''}`, control.label);
          tab.setAttribute('aria-current', control === selected ? 'page' : 'false');
          tab.addEventListener('click', () => { this.throwable = control.label; this.renderPage(this.pageControls, container, page); });
          tabs.append(tab);
        }
        grid.append(tabs); const settings = this.section({ ...selected, label: '' }); settings.classList.add('full-width'); grid.append(settings);
        container.replaceChildren(grid); return;
      }
    }
    let ungrouped = []; let sectionCount = 0;
    const flush = () => {
      if (!ungrouped.length) return;
      const section = this.section({ label: '', children: ungrouped }, sectionCount++);
      if (ungrouped.some(control => control.type === 'catalog')) section.classList.add('full-width', 'catalog-section');
      if (ungrouped.length <= 3 || ungrouped.every(control => ['action','text','catalog'].includes(control.type))) section.classList.add('flat');
      grid.append(section); ungrouped = [];
    };
    for (const control of controls) {
      if (control.type === 'group') { flush(); grid.append(this.section(control, sectionCount++)); }
      else ungrouped.push(control);
    }
    flush(); container.replaceChildren(grid);
  }

  renderSkins(controls, container, page) {
    const view = element('div', 'skin-browser');
    const toolbar = element('div', 'skin-toolbar');
    const filters = element('div', 'skin-filters');
    const pager = element('div', 'skin-pager');
    const collection = controls.find(control => control.collection) || controls.find(control => control.type === 'group' && !control.detail && control.children?.some(child => child.type === 'catalog'));
    const detail = controls.find(control => control.detail);
    const looseCards = controls.filter(control => control.type === 'catalog');
    const cards = collection?.children || looseCards;
    const collectionKey = `${page}:${cards.map(control => control.id).join(',')}`;
    const scroll = this.skinCollectionKey === collectionKey ? container.querySelector('.skin-gallery')?.scrollTop || 0 : 0;
    this.skinCollectionKey = collectionKey;
    for (const control of controls) {
      if (control === collection || control === detail || control.type === 'catalog' || control.label === 'Preview equipped') continue;
      const row = this.render(control); if (!row) continue;
      if (['Previous','Next'].includes(control.label)) pager.append(row);
      else if (control.type === 'text' && /search/i.test(control.label)) {
        row.classList.add('skin-search'); const input = row.querySelector('input');
        input.type = 'search'; input.placeholder = 'Search...'; input.setAttribute('aria-label', control.label); toolbar.append(row);
      } else if (['Back to items','Refresh'].includes(control.label)) toolbar.append(row);
      else filters.append(row);
    }
    const heading = element('div', 'skin-toolbar-stack'); heading.append(toolbar);
    if (filters.childElementCount) heading.append(filters);
    view.append(heading);
    const body = element('div', `skin-body${detail ? ' has-editor' : ''}`);
    const gallery = element('div', 'skin-gallery'); gallery.setAttribute('aria-label', 'Collection');
    this.renderChildren(cards, gallery);
    if (!gallery.childElementCount) gallery.append(element('div', 'empty-catalog', 'No matching items'));
    body.append(gallery);
    if (detail) {
      const editor = this.section({ ...detail, label: '' }); editor.classList.add('skin-editor'); editor.setAttribute('aria-label', detail.label);
      for (const action of editor.querySelectorAll('.action-button')) if (/^Equip|^Apply/.test(action.textContent)) action.classList.add('primary');
      body.append(editor);
    }
    view.append(body);
    if (pager.childElementCount) view.append(pager);
    container.replaceChildren(view);
    gallery.scrollTop = scroll;
  }

  section(control, index = 0) {
    const section = element('section', `section${control.wide ? ' full-width' : ''}`);
    if (control.children?.some(child => child.type === 'catalog')) section.classList.add('full-width', 'catalog-section');
    if (control.id) section.dataset.groupId = control.id;
    if (control.label) {
      const heading = element('div', 'section-heading'); heading.append(element('h2', '', shortLabel(control.label)));
      heading.title = control.label;
      section.append(heading);
    }
    const body = element('div', 'section-body');
    this.renderChildren(control.children || [], body);
    section.append(body); return section;
  }

  renderChildren(children, body) {
    let catalog;
    for (const child of children) {
      const row = this.render(child); if (!row) continue;
      if (child.type === 'catalog' && !child.options && !child.items) {
        if (!catalog) { catalog = element('div', 'catalog-list'); body.append(catalog); }
        catalog.append(row);
      } else { catalog = null; body.append(row); }
    }
  }

  render(control) {
    if (control.hidden) return null;
    if (control.type === 'group') {
      const details = element('details', 'subsection'); details.open = control.open !== false;
      const summary = element('summary'); summary.append(document.createTextNode(shortLabel(control.label))); summary.title = control.label;
      const body = element('div', 'section-body'); this.renderChildren(control.children || [], body);
      details.append(summary, body); return details;
    }
    if (control.type === 'separator') return null;
    if (control.type === 'catalog') return this.renderCatalog(control);
    const row = element('div', 'setting-row'); row.dataset.controlId = control.id; if (control.disabled) row.classList.add('disabled');
    const main = element('div', 'setting-main');
    const label = element('label', 'setting-label', shortLabel(control.label)); label.htmlFor = safeId(control.id); label.title = control.description || control.label; main.append(label);
    if (control.description) row.title = control.description;
    const actions = element('div', 'setting-actions'); row.append(main, actions);
    let update = () => {};
    const page = this.page;
    let sequence = 0, acknowledgedSequence = 0, inFlight = 0;
    let acknowledged = structuredClone(control.value);
    const acknowledge = value => { if (!inFlight) acknowledged = structuredClone(value); };
    const commit = async value => {
      if (control.disabled) return;
      const mutation = ++sequence;
      control.value = value; update(value); row.classList.add('pending');
      inFlight++;
      try { await this.write(control, value, page); if (mutation >= acknowledgedSequence) { acknowledgedSequence = mutation; acknowledged = structuredClone(value); } }
      catch (err) { if (mutation === sequence) { control.value = structuredClone(acknowledged); update(control.value); } this.error(err); }
      finally { if (!--inFlight) row.classList.remove('pending'); }
    };
    if (control.type === 'bool') {
      const toggle = button('switch'); toggle.id = safeId(control.id); toggle.setAttribute('role', 'switch'); toggle.setAttribute('aria-label', control.label);
      update = value => toggle.setAttribute('aria-checked', !!value);
      toggle.addEventListener('click', () => commit(!control.value)); toggle.disabled = !!control.disabled; actions.append(toggle);
    } else if (control.type === 'number' || control.type === 'range') {
      if (!Number.isFinite(Number(control.min)) || !Number.isFinite(Number(control.max)) || control.min == null || control.max == null) {
        const field = element('div', 'number-field'); const input = element('input'); input.type = 'number'; input.id = safeId(control.id); input.step = control.step || 1;
        if (control.min != null) input.min = control.min; if (control.max != null) input.max = control.max;
        input.disabled = !!control.disabled; input.setAttribute('aria-label', `${control.label} value`); field.append(input); actions.append(field);
        update = value => { if (document.activeElement !== input) input.value = Number.isFinite(Number(value)) ? value : ''; };
        input.addEventListener('change', () => { if (input.value === '' || !Number.isFinite(input.valueAsNumber)) { input.value = control.value; return; } commit(clamp(input.valueAsNumber, control.min ?? -Infinity, control.max ?? Infinity)); });
        input.addEventListener('keydown', event => { if (event.key === 'Enter') input.blur(); });
        update(control.value); this.bindings.set(String(control.id), { control, row, update, acknowledge }); return row;
      }
      row.classList.add('slider-row'); row.replaceChildren();
      const top = element('div', 'slider-top'); const field = element('div', 'number-field'); const numeric = element('input');
      numeric.type = 'number'; numeric.setAttribute('aria-label', `${control.label} value`);
      const min = number(control.min, 0), max = number(control.max, 100), step = number(control.step, 1) || 1;
      numeric.min = min; numeric.max = max; numeric.step = step; field.append(numeric);
      if (control.unit) field.append(element('span', 'unit', control.unit)); top.append(main, field);
      const track = element('div', 'slider-track'); const range = element('input'); range.type = 'range'; range.id = safeId(control.id);
      range.min = min; range.max = max; range.step = step; range.setAttribute('aria-label', control.label); track.append(range);
      update = value => {
        const val = clamp(number(value), min, max); range.value = val;
        if (document.activeElement !== numeric) numeric.value = val.toFixed(decimals(step));
        track.style.setProperty('--progress', `${max > min ? (val - min) / (max - min) * 100 : 0}%`);
      };
      range.addEventListener('input', () => { numeric.value = Number(range.value).toFixed(decimals(step)); update(range.value); });
      range.addEventListener('change', () => commit(Number(range.value)));
      numeric.addEventListener('change', () => { if (!numeric.value || !Number.isFinite(numeric.valueAsNumber)) { update(control.value); return; } commit(clamp(numeric.valueAsNumber, min, max)); });
      numeric.addEventListener('keydown', event => { if (event.key === 'Enter') numeric.blur(); });
      range.disabled = numeric.disabled = !!control.disabled; row.append(top, track);
    } else if (control.type === 'vector') {
      row.classList.add('text-row'); const fields = element('div', 'vector-fields');
      const inputs = (control.value || []).map((value, index) => {
        const axis = element('label', 'vector-axis'); axis.append(element('span', '', ['X', 'Y', 'Z', 'W'][index] || String(index + 1)));
        const input = element('input'); input.type = 'number'; input.step = control.step || .1; if (control.min !== undefined) input.min = control.min; if (control.max !== undefined) input.max = control.max;
        input.setAttribute('aria-label', `${control.label} ${['X', 'Y', 'Z', 'W'][index]}`); input.disabled = !!control.disabled;
        input.addEventListener('change', () => { const value = [...control.value]; value[index] = clamp(number(input.value), control.min ?? -Infinity, control.max ?? Infinity); commit(value); }); axis.append(input); fields.append(axis); return input;
      });
      update = value => inputs.forEach((input, index) => { if (document.activeElement !== input) input.value = value[index]; }); row.append(fields);
    } else if (control.type === 'select') {
      const select = element('select', 'select-field'); select.id = safeId(control.id); select.setAttribute('aria-label', control.label);
      (control.options || []).forEach((option, index) => { const entry = typeof option === 'object' ? option : { value: index, label: String(option) }; const node = element('option', '', entry.label); node.value = JSON.stringify(entry.value); node.disabled = !!entry.disabled; select.append(node); });
      update = value => { select.value = JSON.stringify(value); }; select.disabled = !!control.disabled; select.addEventListener('change', () => commit(JSON.parse(select.value))); actions.append(select);
    } else if (control.type === 'text') {
      row.classList.add('text-row'); const input = element('input', 'text-field'); input.type = 'text'; input.id = safeId(control.id); input.value = control.value || ''; input.placeholder = control.placeholder || '';
      if (control.maxLength) input.maxLength = control.maxLength; input.disabled = !!control.disabled; input.spellcheck = false;
      update = value => { if (document.activeElement !== input) input.value = value || ''; }; input.addEventListener('change', () => commit(input.value)); input.addEventListener('keydown', event => { if (event.key === 'Enter') input.blur(); }); row.append(input);
    } else if (control.type === 'color') {
      const swatch = button('color-button'); swatch.id = safeId(control.id); swatch.setAttribute('aria-label', `${control.label} color`); swatch.disabled = !!control.disabled;
      update = value => swatch.style.setProperty('--swatch', colorCss(value || [1, 1, 1, 1]));
      swatch.addEventListener('click', () => this.openPopover(swatch, control.label, this.colorEditor(control, commit))); actions.append(swatch);
    } else if (control.type === 'keybind') {
      const key = button('keybind-button'); key.id = safeId(control.id); key.disabled = !!control.disabled;
      const labelForKey = value => {
        const names = { 0: control.zeroLabel || 'Unbound', 1: 'Mouse 1', 2: 'Mouse 2', 4: 'Mouse 3', 5: 'Mouse 4', 6: 'Mouse 5', 8: 'Backspace', 9: 'Tab', 13: 'Enter', 16: 'Shift', 17: 'Ctrl', 18: 'Alt', 20: 'Caps Lock', 27: 'Escape', 32: 'Space', 33: 'Page Up', 34: 'Page Down', 35: 'End', 36: 'Home', 37: 'Left', 38: 'Up', 39: 'Right', 40: 'Down', 45: 'Insert', 46: 'Delete' };
        return typeof value === 'object' ? (value.label || value.key || 'None') : names[value] || (value >= 48 && value <= 90 ? String.fromCharCode(value) : value >= 112 && value <= 135 ? `F${value - 111}` : `Key ${value}`);
      };
      update = value => { if (!key.classList.contains('listening')) key.textContent = labelForKey(value); };
      key.addEventListener('click', () => {
        this.listenCleanup?.(); key.classList.add('listening'); key.textContent = 'Press a key';
        const cancel = () => { key.classList.remove('listening'); update(control.value); window.removeEventListener('keydown', capture, true); window.removeEventListener('pointerdown', captureMouse, true); window.removeEventListener('contextmenu', suppressContext, true); this.listenCleanup = null; };
        const suppressContext = event => { event.preventDefault(); event.stopImmediatePropagation(); };
        const captureMouse = event => {
          const code = [1, 4, 2, 5, 6][event.button]; if (code === undefined) return;
          event.preventDefault(); event.stopImmediatePropagation(); cancel(); commit(code);
          if (event.button === 2) { window.addEventListener('contextmenu', suppressContext, { capture: true, once: true }); }
        };
        const capture = event => { event.preventDefault(); event.stopImmediatePropagation(); if (event.key === 'Escape') { cancel(); return; }
          const code = event.key === 'Backspace' || event.key === 'Delete' ? 0 : (event.keyCode || event.which); cancel(); commit(code); };
        this.listenCleanup = cancel; window.addEventListener('keydown', capture, true); window.addEventListener('pointerdown', captureMouse, true); window.addEventListener('contextmenu', suppressContext, true);
      }); actions.append(key);
    } else if (control.type === 'button' || control.type === 'action') {
      const action = button(`action-button${control.primary ? ' primary' : ''}`, shortLabel(control.label)); action.setAttribute('aria-label', control.label); action.id = safeId(control.id); action.disabled = !!control.disabled;
      update = () => { if (typeof control.selected === 'boolean') { action.setAttribute('aria-pressed', control.selected); action.classList.toggle('selected', control.selected); } };
      row.replaceChildren(action); row.classList.add('inline-actions'); action.addEventListener('click', async () => { action.disabled = true; try { await this.action(control, page); } catch (error) { this.error(error); } finally { action.disabled = !!control.disabled; } });
    } else if (control.type === 'label') {
      row.classList.add('read-only'); update = value => { if (value !== undefined) actions.textContent = String(value); };
    } else { return null; }
    if (control.optionsControls?.length) {
      const more = button('more-button'); more.textContent = '⋯'; more.setAttribute('aria-label', `${control.label} options`); more.setAttribute('aria-expanded', 'false'); more.disabled = !!control.disabled;
      more.addEventListener('click', () => {
        if (this.popover?.anchor === more) { this.closePopover(); return; }
        const content = element('div'); control.optionsControls.forEach(child => { const setting = this.render(child); if (setting) content.append(setting); }); this.openPopover(more, control.label, content);
      }); actions.prepend(more);
    }
    update(control.value); this.bindings.set(String(control.id), { control, row, update, acknowledge }); return row;
  }

  sync(controls, pending = new Set()) {
    this.pageControls = controls;
    const traverse = list => list.forEach(control => {
      const binding = this.bindings.get(String(control.id));
      const editing = binding?.row.contains(document.activeElement) && document.activeElement.matches('input,select');
      if (binding && !pending.has(String(control.id)) && !editing) {
        binding.acknowledge?.(control.value);
        binding.control.value = control.value; binding.control.disabled = control.disabled; binding.control.selected = control.selected;
        binding.row.classList.toggle('disabled', !!control.disabled); binding.update(control.value);
        if (!binding.managesDisabled) binding.row.querySelectorAll('input,button,select').forEach(node => { node.disabled = !!control.disabled; });
      }
      if (control.children) traverse(control.children); if (control.optionsControls) traverse(control.optionsControls);
    }); traverse(controls);
  }

  openPopover(anchor, title, content) {
    const box = anchor.getBoundingClientRect();
    this.closePopover(); const node = element('section', 'options-popover'); node.setAttribute('role', 'dialog'); node.setAttribute('aria-label', `${title} options`);
    const heading = element('div', 'popover-heading', title); const close = button(); close.textContent = '×'; close.setAttribute('aria-label', 'Close options'); close.addEventListener('click', () => this.closePopover()); heading.append(close);
    const body = element('div', 'popover-content'); body.append(content); node.append(heading, body); document.body.append(node);
    const height = node.getBoundingClientRect().height;
    node.style.left = `${Math.max(12, Math.min(window.innerWidth - 322, box.right - 310))}px`;
    node.style.top = `${Math.max(12, Math.min(window.innerHeight - height - 12, box.bottom + 9))}px`;
    anchor.setAttribute('aria-expanded', 'true'); this.popover = { node, anchor }; close.focus({ preventScroll: true });
  }

  closePopover() { if (this.popover) { const {node,anchor} = this.popover; const restore = node.contains(document.activeElement); anchor.setAttribute('aria-expanded', 'false'); node.remove(); this.popover = null; if (restore && anchor.isConnected) anchor.focus({preventScroll:true}); } }

  colorEditor(control, commit) {
    const editor = element('div', 'color-editor'); const picker = element('input'); picker.type = 'color'; picker.value = colorHex(control.value); picker.setAttribute('aria-label', `${control.label} RGB`); editor.append(picker);
    const channels = element('div', 'rgba-fields'); const values = [...control.value];
    const inputs = values.map((value, index) => { const label = element('label', '', ['Red', 'Green', 'Blue', 'Alpha'][index]); const input = element('input'); input.type = 'number'; input.min = 0; input.max = 255; input.step = 1; input.value = Math.round(value * 255); input.setAttribute('aria-label', `${control.label} ${['red', 'green', 'blue', 'alpha'][index]}`);
      input.addEventListener('change', () => { values[index] = clamp(number(input.value), 0, 255) / 255; picker.value = colorHex(values); commit([...values]); }); label.append(input); channels.append(label); return input; });
    picker.addEventListener('change', () => { for (let index = 0; index < 3; index++) { values[index] = parseInt(picker.value.slice(index * 2 + 1, index * 2 + 3), 16) / 255; inputs[index].value = Math.round(values[index] * 255); } commit([...values]); }); editor.append(channels); return editor;
  }

  renderCatalog(control) {
    const page = this.page;
    if (!control.options && !control.items) {
      const entry = button(`catalog-row catalog-card${control.selected ? ' selected' : ''}`); entry.dataset.controlId = control.id;
      entry.setAttribute('aria-pressed', !!control.selected);
      entry.setAttribute('aria-label', control.label); entry.title = control.label;
      if (control.image && /^(asset:|https:\/\/asset\.localhost\/|data:image\/)/.test(control.image)) {
        const art = element('div', 'catalog-art'); const image = element('img'); image.src = control.image; image.alt = ''; image.loading = 'lazy'; art.append(image); entry.append(art); entry.classList.add('has-art');
      } else entry.classList.add('catalog-selection');
      entry.append(element('span', 'catalog-name', control.label));
      const update = () => { entry.disabled = !!control.disabled; entry.classList.toggle('disabled', !!control.disabled); entry.classList.toggle('selected', !!control.selected); entry.setAttribute('aria-pressed', !!control.selected); };
      update(); this.bindings.set(String(control.id), { control, row: entry, managesDisabled: true, update });
      entry.addEventListener('click', async () => { entry.disabled = true; try { await this.action(control, page); } catch (error) { this.error(error); } finally { entry.disabled = !!control.disabled; } });
      return entry;
    }
    const wrapper = element('div', 'catalog'); wrapper.dataset.controlId = control.id;
    const searchBox = element('div', 'catalog-search'); const search = element('input', 'text-field'); search.type = 'search'; search.placeholder = `Search ${control.label.toLowerCase()}`; search.setAttribute('aria-label', search.placeholder); searchBox.append(search); wrapper.append(searchBox);
    const list = element('div', 'catalog-list'); const render = items => { list.replaceChildren();
      items.forEach(item => { const entry = button(`catalog-row${JSON.stringify(item.value) === JSON.stringify(control.value) ? ' selected' : ''}`); entry.setAttribute('aria-pressed', JSON.stringify(item.value) === JSON.stringify(control.value));
        if (item.image && /^(asset:|https:\/\/asset\.localhost\/|data:image\/)/.test(item.image)) { const image = element('img'); image.src = item.image; image.alt = ''; entry.append(image); }
        entry.append(element('span', '', item.label)); if (JSON.stringify(item.value) === JSON.stringify(control.value)) { const mark = element('span'); mark.textContent = 'Selected'; entry.append(mark); }
        entry.disabled = !!control.disabled || !!item.disabled; entry.addEventListener('click', async () => { entry.disabled = true; try { await this.write(control, item.value, page); control.value = item.value; render(items); } catch (err) { this.error(err); entry.disabled = !!control.disabled || !!item.disabled; } }); list.append(entry); });
      if (!items.length) list.append(element('div', 'empty-catalog', 'No matching items.'));
    }; render(control.options || control.items || []); let timer, requestSequence = 0;
    const filtered = () => (control.options || control.items || []).filter(item => item.label.toLowerCase().includes(search.value.toLowerCase()));
    search.addEventListener('input', () => { clearTimeout(timer); const generation = ++requestSequence; timer = setTimeout(async () => { if (control.remote) { try { const result = await this.catalog(control, search.value); if (generation === requestSequence && wrapper.isConnected) render(result.items || result.options || []); } catch (error) { if (generation === requestSequence) this.error(error); } } else render(filtered()); }, 120); });
    this.bindings.set(String(control.id), { control, row: wrapper, managesDisabled: true, update: () => { search.disabled = !!control.disabled; render(filtered()); } });
    wrapper.append(list); return wrapper;
  }
}
