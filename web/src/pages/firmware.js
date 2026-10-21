// Firmware (2.0.0 N7, DESIGN.md section 9.5): the update check and the upload. Not a tab: #firmware, linked from
// Diagnostics. The check is the browser's, not the feeder's: it reads the release manifest from GitHub Pages when this
// page opens (unless turned off here), so a feeder with no internet access loses nothing. The upload is signed with
// the settings password, over the image's SHA-256, which the feeder checks again before it switches.

const MANIFEST_URL = 'https://jspeer.github.io/precision-feeder/manifest.json';
const RELEASES_URL = 'https://github.com/jspeer/precision-feeder/releases';
const CHECK_PREF = 'pf.updateCheck';  // 'off' turns the check off in this browser
// A network that drops packets silently would leave the check "Checking…" for minutes: give up after this long.
const CHECK_TIMEOUT_MS = 10000;

function updateCheckOn() {
  try { return localStorage.getItem(CHECK_PREF) !== 'off'; } catch (e) { return true; }
}

PAGES.firmware = {
  mount(root) {
    this.manifest = null;
    this.checkEl = h('div', { 'aria-live': 'polite' });
    const toggle = h('input', { type: 'checkbox', checked: updateCheckOn(), onchange: () => {
      try { localStorage.setItem(CHECK_PREF, toggle.checked ? 'on' : 'off'); } catch (e) { /* not kept */ }
      this.check();
    } });
    this.installed = row('Installed', '');
    this.fileEl = h('input', { type: 'file', accept: '.bin', onchange: () => this.choose() });
    this.fileInfo = h('p', { class: 'hint', 'aria-live': 'polite' });
    this.uploadBtn = h('button', { class: 'btn btn-primary', disabled: true, onclick: () => this.upload() }, 'Upload and restart');
    this.progress = h('progress', { max: 1, value: 0, hidden: true });
    append(root,
      card('Updates', this.installed.row, this.checkEl,
        h('label', { class: 'check' }, toggle, ' Check for updates when this page opens (this browser only)')),
      card('Upload firmware',
        h('p', { class: 'hint' }, 'Choose the .ota.bin for this feeder\'s variant. The feeder checks it, then ' +
          'restarts into it. Settings and calibration are kept.'),
        this.fileEl, this.fileInfo, this.progress, h('div', { class: 'actions' }, this.uploadBtn)));
    this.update();
    this.check();
  },

  update() {
    const variant = store.text(E.firmwareVariant);
    setText(this.installed.value, [store.text(E.firmware) || 'unknown', variant].filter(Boolean).join(', '));
  },

  async check() {
    const link = text => h('a', { href: RELEASES_URL, target: '_blank', rel: 'noopener' }, text);
    if (!updateCheckOn()) {
      this.show(h('p', { class: 'hint' }, 'The update check is off. ', link('Releases')));
      return;
    }
    this.show(h('p', { class: 'hint' }, 'Checking…'));
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), CHECK_TIMEOUT_MS);
    try {
      const r = await fetch(MANIFEST_URL, { cache: 'no-store', signal: abort.signal });
      this.manifest = r.ok ? await r.json() : null;
    } catch (e) {
      this.manifest = null;
    }
    clearTimeout(timer);
    const u = updateState(this.manifest, store.text(E.firmware), store.text(E.firmwareVariant));
    const notes = this.manifest && this.manifest.notes;
    if (!this.manifest) this.show(h('p', {}, 'Couldn\'t check for updates. ', link('Releases')));
    else if (u.state === 'update') {
      this.show(h('p', {}, h('strong', {}, `Version ${u.latest} is available. `),
        notes ? h('a', { href: notes, target: '_blank', rel: 'noopener' }, 'Release notes') : link('Releases')));
    } else if (u.state === 'current') this.show(h('p', {}, `Up to date (latest release ${u.latest}).`));
    else if (u.state === 'yaml') {
      this.show(h('p', {}, `Latest release ${u.latest}. Built from your own YAML: update with ESPHome.`));
    } else if (u.state === 'unlisted') {
      this.show(h('p', {}, `No published updates for this variant (${store.text(E.firmwareVariant)}): it is built ` +
        `from its own branch. Latest release of the main firmware: ${u.latest}.`));
    } else this.show(h('p', {}, `Latest release ${u.latest}. `, link('Releases')));
    if (this.bytes) this.choose();  // name the chosen file now that the manifest is known
  },

  show(node) { clear(this.checkEl); this.checkEl.appendChild(node); },

  async choose() {
    const file = this.fileEl.files[0];
    this.bytes = null;
    this.uploadBtn.disabled = true;
    if (!file) { setText(this.fileInfo, ''); return; }
    setText(this.fileInfo, 'Checking the file…');
    const bytes = new Uint8Array(await file.arrayBuffer());
    // An app image: the image magic, and the app description's magic at byte 32 (a .factory.bin starts with the
    // bootloader instead). The feeder checks the same.
    if (bytes[0] !== 0xe9 || bytes[32] !== 0x32 || bytes[33] !== 0x54 || bytes[34] !== 0xcd || bytes[35] !== 0xab) {
      setText(this.fileInfo, bytes[0] === 0xe9 ? 'That is a first-flash image (.factory.bin): choose the .ota.bin.'
                                               : 'That is not an ESP32 firmware image.');
      return;
    }
    this.bytes = bytes;
    this.sha = toHex(sha256(bytes));
    this.image = identifyImage(this.manifest, this.sha);
    const variant = store.text(E.firmwareVariant);
    let text;
    if (!this.image) {
      text = 'Not a released image: upload it only if you built it yourself.';
    } else {
      text = `Release ${this.image.version}, ${this.image.variant}: checked against the release.` +
        (variant && variant !== 'yaml' && this.image.variant !== variant ? ` This feeder runs ${variant}.` : '');
    }
    setText(this.fileInfo, text);
    this.uploadBtn.disabled = !this.bytes;
  },

  async upload() {
    const variant = store.text(E.firmwareVariant);
    const warn = !this.image ? 'Not a released image. A wrong one can stop the feeder starting (recover over USB).'
      : this.image.variant !== variant ? `This feeder runs ${variant || 'another build'}; this image is ` +
                                         `${this.image.variant}.` : '';
    const ok = await confirmDialog({ title: 'Upload firmware?', danger: !!warn, ok: 'Upload',
      body: `${warn ? warn + ' ' : ''}The feeder restarts into it when the upload ends.` });
    if (!ok) return;
    this.uploadBtn.disabled = true;
    this.progress.hidden = false;
    try {
      await auth.uploadFirmware(this.bytes, this.sha, p => { this.progress.value = p; });
      toast('Uploaded. The feeder is restarting…', 'ok', 8000);
      setText(this.fileInfo, 'Uploaded: the feeder restarts into it and this page reconnects.');
    } catch (e) {
      toastError(e);
      this.uploadBtn.disabled = false;
    }
    this.progress.hidden = true;
  },
};
