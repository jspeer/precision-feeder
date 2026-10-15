// DOM helpers and the small widgets every page shares. No framework: pages build their DOM once with h() and
// update the parts that change.

/** Pages register here: PAGES.<tab id> = {mount(el, route), update(changed), unmount()}. */
const PAGES = {};

/** h('div', {class: 'x', onclick: fn}, child, 'text', [more]) -> an element. */
function h(tag, props, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
    else if (k === 'value') el.value = v;
    else if (k === 'checked') el.checked = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  append(el, children);
  return el;
}

/** Append children: nodes, text, or (nested) arrays of them; null, undefined and false are skipped. */
function append(el, ...children) {
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

function clear(el) {
  while (el.firstChild) el.removeChild(el.firstChild);
  return el;
}

/** Set an element's text only when it changes (keeps the DOM quiet). */
function setText(el, text) {
  const t = text === null || text === undefined ? '' : String(text);
  if (el.textContent !== t) el.textContent = t;
}

// ---- icons: 24 x 24 outline paths, drawn for this app ----
const ICONS = {
  grid: 'M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z',
  calendar: 'M4 6h16v14H4zM4 10h16M8 3v5M16 3v5',
  sliders: 'M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1M15 4v4M9 10v4M17 16v4',
  tool: 'M14.5 4.5a4.5 4.5 0 0 0-5.6 5.6L3.5 15.5l5 5 5.4-5.4a4.5 4.5 0 0 0 5.6-5.6l-2.8 2.8-2.6-.4-.4-2.6z',
  bluetooth: 'M7 7l10 10-5 5V2l5 5L7 17',
  activity: 'M3 12h4l3-8 4 16 3-8h4',
  fish: 'M2.5 12c3-5 9.5-6.5 14 0-4.5 6.5-11 5-14 0zM16.5 12l5-3.5v7zM8 11h.01',
  alert: 'M12 3l10 18H2zM12 10v4M12 17.5h.01',
  wifiOff: 'M3 3l18 18M8.5 16.5a5 5 0 0 1 7 0M5 13a10 10 0 0 1 4.5-2.4M19 13a10 10 0 0 0-2.2-1.6M2 9.5a15 15 0 0 1 4-2.6M22 9.5A15 15 0 0 0 11 5.6M12 20h.01',
  clock: 'M12 3a9 9 0 1 0 0 18 9 9 0 1 0 0-18zM12 7v5l3 2',
  pause: 'M12 3a9 9 0 1 0 0 18 9 9 0 1 0 0-18zM10 9v6M14 9v6',
  bowl: 'M3 11h18a9 9 0 0 1-18 0zM9 7.5c0-1.2.8-2 1.8-2.4M14 7c0-1.2.8-2 1.8-2.4',
  tune: 'M12 3a9 9 0 1 0 0 18 9 9 0 1 0 0-18zM12 3v4M12 17v4M3 12h4M17 12h4M12 12l3.5-3.5',
  check: 'M5 12.5l4.5 4.5L19 7.5',
  x: 'M6 6l12 12M18 6L6 18',
  info: 'M12 3a9 9 0 1 0 0 18 9 9 0 1 0 0-18zM12 11v6M12 7.5h.01',
  search: 'M10.5 4a6.5 6.5 0 1 0 0 13 6.5 6.5 0 1 0 0-13zM15.5 15.5L20 20',
  copy: 'M9 9h11v11H9zM5 15V4h11',
};

function icon(name, size = 20) {
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('width', size);
  svg.setAttribute('height', size);
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', 'icon');
  const path = document.createElementNS(ns, 'path');
  path.setAttribute('d', ICONS[name] || ICONS.info);
  svg.appendChild(path);
  return svg;
}

// ---- toasts ----
function toast(message, kind = 'info', ms = 4000) {
  const box = document.getElementById('toasts');
  if (!box) return;
  const el = h('div', { class: `toast toast-${kind}`, role: kind === 'error' ? 'alert' : 'status' },
               icon(kind === 'error' ? 'alert' : kind === 'ok' ? 'check' : 'info', 18), h('span', {}, message));
  box.appendChild(el);
  setTimeout(() => {
    el.classList.add('toast-out');
    setTimeout(() => el.remove(), 300);
  }, ms);
}

/** Toast an error from a rejected api call. */
function toastError(err) {
  toast(err && err.message ? err.message : String(err), 'error', 6000);
}

// ---- dialogs ----
/** Show a dialog whose form has buttons valued "ok" and "cancel"; resolve true for OK, false otherwise. Resolves on
 *  the submit itself (and on Escape): browsers may hold back a dialog's "close" event while the page is not being
 *  drawn (a hidden tab), so that event is only a fallback. validate() may return a message to keep it open. */
function runDialog(dlg, form, validate, error) {
  return new Promise(resolve => {
    let done = false;
    const finish = ok => {
      if (done) return;
      done = true;
      if (dlg.open) dlg.close(ok ? 'ok' : 'cancel');
      setTimeout(() => dlg.remove(), 0);
      resolve(ok);
    };
    form.addEventListener('submit', ev => {
      const ok = !!ev.submitter && ev.submitter.value === 'ok';
      const message = ok && validate ? validate() : '';
      ev.preventDefault();
      if (message) {
        setText(error, message);
        return;
      }
      finish(ok);
    });
    dlg.addEventListener('cancel', () => finish(false));
    dlg.addEventListener('close', () => finish(dlg.returnValue === 'ok'));
    document.body.appendChild(dlg);
    dlg.showModal();
  });
}

/** A modal question. body: text or nodes. Resolves true for OK, false for Cancel or Escape. */
function confirmDialog({ title, body, ok = 'OK', cancel = 'Cancel', danger = false }) {
  const form = h('form', { method: 'dialog' },
    h('h2', { id: 'dlg-title' }, title),
    h('div', { class: 'dialog-body' }, typeof body === 'string' ? h('p', {}, body) : body),
    h('div', { class: 'dialog-actions' },
      h('button', { value: 'cancel', class: 'btn' }, cancel),
      h('button', { value: 'ok', class: danger ? 'btn btn-danger' : 'btn btn-primary' }, ok)));
  return runDialog(h('dialog', { class: 'dialog', 'aria-labelledby': 'dlg-title' }, form), form);
}

/** A modal form. body: nodes with the inputs. validate() returns an error message to keep it open, or ''.
 *  Resolves true for OK (after validation), false for Cancel or Escape. */
function formDialog({ title, body, ok = 'Save', cancel = 'Cancel', validate = () => '' }) {
  const error = h('p', { class: 'dialog-error', role: 'alert' });
  const form = h('form', { method: 'dialog' },
    h('h2', { id: 'dlg-title' }, title),
    h('div', { class: 'dialog-body dialog-form' }, body),
    error,
    h('div', { class: 'dialog-actions' },
      h('button', { value: 'cancel', class: 'btn', formnovalidate: true }, cancel),
      h('button', { value: 'ok', class: 'btn btn-primary' }, ok)));
  // Enter in a field means OK (the browser would otherwise use the first button, Cancel).
  form.addEventListener('keydown', ev => {
    if (ev.key === 'Enter' && ev.target.tagName === 'INPUT') {
      ev.preventDefault();
      form.requestSubmit(form.querySelector('button[value="ok"]'));
    }
  });
  form.addEventListener('input', () => setText(error, ''));
  return runDialog(h('dialog', { class: 'dialog', 'aria-labelledby': 'dlg-title' }, form), form, validate, error);
}

// ---- weekday chips (Monday first; names from the browser's locale; masks as the firmware: bit 0 = Sunday) ----
const WEEK_ORDER = [1, 2, 3, 4, 5, 6, 0];
function weekdayName(day, style) {
  return new Date(Date.UTC(2023, 0, 1 + day)).toLocaleDateString(undefined, { weekday: style, timeZone: 'UTC' });
}

/** Seven day chips for a weekday mask. Editable chips toggle; read the result from .mask. */
function dayChips(mask, editable = false, label = 'Days') {
  // Read-only chips are hidden from screen readers; the page states the days in words next to them.
  const box = h('div', { class: editable ? 'chips chips-edit' : 'chips', role: editable ? 'group' : null,
                         'aria-label': editable ? label : null, 'aria-hidden': editable ? null : 'true' });
  box.mask = mask & 0x7f;
  for (const day of WEEK_ORDER) {
    const on = () => !!(box.mask & (1 << day));
    const chip = h(editable ? 'button' : 'span', {
      type: editable ? 'button' : null, class: 'chip', title: weekdayName(day, 'long'),
      'aria-label': editable ? weekdayName(day, 'long') : null,
      onclick: editable ? () => {
        box.mask ^= 1 << day;
        chip.classList.toggle('chip-on', on());
        chip.setAttribute('aria-pressed', on() ? 'true' : 'false');
        box.dispatchEvent(new Event('input', { bubbles: true }));
      } : null,
    }, weekdayName(day, 'narrow'));
    chip.classList.toggle('chip-on', on());
    if (editable) chip.setAttribute('aria-pressed', on() ? 'true' : 'false');
    box.appendChild(chip);
  }
  return box;
}

/** Days in words: "Every day", "Weekdays", "Mon, Wed, Fri", "No days". */
function daysWords(mask) {
  mask &= 0x7f;
  if (mask === 0x7f) return 'Every day';
  if (mask === 0) return 'No days';
  if (mask === 0x3e) return 'Weekdays';
  if (mask === 0x41) return 'Weekends';
  return WEEK_ORDER.filter(d => mask & (1 << d)).map(d => weekdayName(d, 'short')).join(', ');
}

/** A row of choices; .value is the chosen one. options: [[value, label], ...]. */
function segmented(options, value, label, onchange) {
  const box = h('div', { class: 'segmented', role: 'radiogroup', 'aria-label': label });
  box.value = value;
  const buttons = options.map(([v, text]) => h('button', { type: 'button', role: 'radio', class: 'seg',
    'aria-checked': v === value ? 'true' : 'false', onclick: () => {
      box.value = v;
      buttons.forEach((b, i) => b.setAttribute('aria-checked', options[i][0] === v ? 'true' : 'false'));
      if (onchange) onchange(v);
      box.dispatchEvent(new Event('input', { bubbles: true }));
    } }, text));
  append(box, buttons);
  return box;
}

/** A minus / value / plus stepper between lo and hi; .value is the number. */
function stepper(value, lo, hi, label) {
  const shown = h('span', { class: 'stepper-value', 'aria-live': 'polite' }, String(value));
  const box = h('div', { class: 'stepper', role: 'group', 'aria-label': label });
  box.value = value;
  const set = v => {
    box.value = Math.min(hi, Math.max(lo, v));
    setText(shown, String(box.value));
  };
  append(box, h('button', { type: 'button', class: 'btn btn-small', 'aria-label': `${label}: less`, onclick: () => set(box.value - 1) }, '\u2212'),
         shown, h('button', { type: 'button', class: 'btn btn-small', 'aria-label': `${label}: more`, onclick: () => set(box.value + 1) }, '+'));
  return box;
}

/** A labelled field for dialogs. */
function field(label, control, hint) {
  return h('div', { class: 'form-field' }, h('span', { class: 'form-label' }, label), control,
           hint ? h('span', { class: 'hint' }, hint) : null);
}

// ---- layout helpers ----
function card(title, ...children) {
  return h('section', { class: 'card' }, title ? h('h2', { class: 'card-title' }, title) : null, ...children);
}

/** A label / value row; returns {row, value} so the value can be updated. */
function row(label, value = '') {
  const v = h('span', { class: 'row-value' }, value);
  return { row: h('div', { class: 'row' }, h('span', { class: 'row-label' }, label), v), value: v };
}

/** A label row whose value is kept in step with an entity: fn(store) -> text. Returns an updater. */
function boundRow(parent, label, fn) {
  const r = row(label);
  parent.appendChild(r.row);
  return () => setText(r.value, fn());
}

// ---- controls for any entity (the All entities list; reused by settings pages) ----

/** A control for entity `id` that writes through api and reports the outcome; null for read-only entities. */
function entityControl(id) {
  const e = store.get(id);
  if (!e) return null;
  const domain = e.domain || id.slice(0, id.indexOf('/'));
  const label = e.name || id;
  if (domain === 'button') {
    return h('button', { class: 'btn btn-small', onclick: async () => {
      if (!await confirmDialog({ title: `Press "${label}"?`, body: 'This acts on the feeder straight away.', ok: 'Press' })) return;
      api.press(id).then(() => toast(`Pressed ${label}`, 'ok'), toastError);
    } }, 'Press');
  }
  if (domain === 'switch') {
    const btn = h('button', { class: 'btn btn-small', role: 'switch', onclick: () => {
      const on = !store.bool(id);
      api.turn(id, on).then(() => api.waitFor(id, x => x.value === on)).catch(toastError);
    } });
    btn._update = () => {
      const on = store.bool(id);
      btn.setAttribute('aria-checked', on ? 'true' : 'false');
      setText(btn, on ? 'On' : 'Off');
      btn.classList.toggle('btn-on', !!on);
    };
    btn._update();
    return btn;
  }
  if (domain === 'select') {
    const sel = h('select', { class: 'input', 'aria-label': label, onchange: () => {
      api.setAndConfirm(id, sel.value).catch(err => { toastError(err); sel._update(); });
    } }, (e.option || []).map(o => h('option', { value: o }, o)));
    sel._update = () => { if (document.activeElement !== sel) sel.value = store.text(id); };
    sel._update();
    return sel;
  }
  if (domain === 'number' || domain === 'text') {
    const isNum = domain === 'number';
    const input = h('input', isNum
      ? { class: 'input input-num', type: 'number', inputmode: 'decimal', min: e.min_value, max: e.max_value,
          step: e.step, 'aria-label': label }
      : { class: 'input', type: 'text', maxlength: e.max_length, 'aria-label': label });
    const err = h('span', { class: 'field-error', 'aria-live': 'polite' });
    const save = () => {
      setText(err, '');
      const v = input.value.trim();
      if (isNum) {
        const x = parseFloat(v), lo = parseFloat(e.min_value), hi = parseFloat(e.max_value);
        if (v === '' || isNaN(x)) return setText(err, 'Enter a number');
        if (x < lo || x > hi) return setText(err, `From ${e.min_value} to ${e.max_value}`);
      }
      api.setAndConfirm(id, isNum ? parseFloat(v) : v)
        .then(() => toast(`${label} saved`, 'ok'), x => { toastError(x); input._update(true); });
    };
    input.addEventListener('keydown', ev => { if (ev.key === 'Enter') save(); });
    input.addEventListener('input', () => setText(err, ''));
    input._update = force => {
      if (!force && document.activeElement === input) return;
      const n = parseFloat(store.get(id).value);
      input.value = isNum ? (isNaN(n) ? '' : n) : store.text(id);
    };
    input._update();
    const wrap = h('span', { class: 'field' }, input,
                   e.uom ? h('span', { class: 'unit' }, e.uom) : null,
                   h('button', { class: 'btn btn-small', onclick: save }, 'Set'), err);
    wrap._update = () => input._update();
    return wrap;
  }
  return null;
}

/** A settings row: label, explanation, and the entity's own control (limits and unit from the device). Nothing if
 *  the entity is not in this build. Returns the row; its control keeps itself current through ._update. */
function setting(label, hint, id) {
  const control = entityControl(id);
  if (!control) return null;
  const el = h('div', { class: 'setting' },
    h('div', { class: 'setting-text' }, h('div', { class: 'setting-label' }, label), hint ? h('div', { class: 'hint' }, hint) : null),
    h('div', { class: 'setting-control' }, control));
  el._update = () => { if (control._update) control._update(); };
  return el;
}
