// Setup: the first-run wizard (2.0.0 N6, DESIGN.md section 9.4). Not a tab: the app opens #setup by itself while the
// feeder reports that setup is not finished (GET /feeder/network), and Configuration links to it. Every step uses the
// same controls as Configuration and saves as it goes; nothing waits for the last step, and every step can be skipped.

PAGES.setup = {
  STEPS: ['Welcome', 'Network', 'Time and place', 'Board and pins', 'Keys', 'Settings password', 'Calibration'],

  mount(root) {
    this.root = root;
    this.step = 0;
    this.rows = [];
    this.timeRows = [];
    this.offAuth = () => this.refreshAuth();
    auth.onChange(this.offAuth);
    auth.loadNetwork();
    this.render();
  },

  unmount() {
    this.root = null;
    const i = auth._listeners.indexOf(this.offAuth);
    if (i >= 0) auth._listeners.splice(i, 1);
  },

  /** Steps this build has: no board step without the hardware package, no keys step with no key to manage. */
  shown(i) {
    const name = this.STEPS[i];
    if (name === 'Board and pins') return store.has(E.board);
    if (name === 'Keys') return auth.keys.api !== 'off' || auth.keys.ble !== 'off';
    if (name === 'Calibration') return store.has(E.calibrationMode);
    return true;
  },
  move(dir) {
    let i = this.step + dir;
    while (i > 0 && i < this.STEPS.length - 1 && !this.shown(i)) i += dir;
    this.step = Math.max(0, Math.min(this.STEPS.length - 1, i));
    this.render();
    window.scrollTo(0, 0);
  },

  async finish(where) {
    try {
      await auth.setSetupDone(true);
    } catch (e) {
      toastError(e);
      return;
    }
    toast('Setup finished. Everything here is in Configuration too.', 'ok');
    location.hash = where;
  },

  render() {
    if (!this.root) return;
    clear(this.root);
    this.rows = [];
    this.timeRows = [];
    this.after = null;
    const visible = this.STEPS.map((_, i) => i).filter(i => this.shown(i));
    const at = visible.indexOf(this.step) + 1;
    const head = h('div', { class: 'wizard-head' },
      h('div', { class: 'wizard-count' }, `Step ${at} of ${visible.length}`),
      h('div', { class: 'wizard-bar', role: 'progressbar', 'aria-valuemin': 1, 'aria-valuemax': visible.length,
                 'aria-valuenow': at, style: `--done: ${(at / visible.length) * 100}%` }));
    const body = this[`step${this.step}`]();
    const first = this.step === 0, last = this.step === this.STEPS.length - 1;
    const nav = h('div', { class: 'actions wizard-nav' },
      first ? null : h('button', { class: 'btn', onclick: () => this.move(-1) }, 'Back'),
      h('span', { class: 'spacer' }),
      last ? null : h('button', { class: 'btn btn-primary', onclick: () => this.move(1) }, first ? 'Start' : 'Next'));
    append(this.root, head, card(this.STEPS[this.step], body, nav),
           first || last ? null : h('p', { class: 'hint wizard-skip' }, 'Nothing here is required. ',
             h('a', { href: '#', onclick: ev => { ev.preventDefault(); this.finish('#dashboard'); } },
               'Skip the rest of setup'), '.'));
    if (this.after) this.after();  // parts that fill themselves once they are on the page
    this.update();
  },

  step0() {
    return [
      h('p', {}, 'A few steps to get the feeder ready: its name and network, where it is, how it is wired, the keys ' +
                 'for Home Assistant and Bluetooth, a settings password, and lining up the tray.'),
      h('p', {}, 'Each step saves as you go and can be skipped. All of it is in Configuration and Maintenance later.'),
      h('p', { class: 'hint' }, h('a', { href: '#', onclick: ev => { ev.preventDefault(); this.finish('#dashboard'); } },
        'Skip setup'), ': the feeder already works with what it was built with.'),
    ];
  },

  step1() {
    const n = auth.network;
    if (!n) return h('p', {}, 'The feeder did not report its network settings. You can set them later in Configuration.');
    const cfg = PAGES.configuration;
    const btn = (label, onclick, primary) => h('button', { class: primary ? 'btn btn-small btn-primary' : 'btn btn-small', onclick }, label);
    const out = [
      h('div', { class: 'key-part' }, h('div', { class: 'setting-label' }, 'Name'),
        h('p', {}, `${n.name_saved || n.name}: the feeder answers at ${n.name_saved || n.name}.local` +
                   `${n.name_saved && n.name_saved !== n.name ? ' after a restart' : ''}. Give each feeder its own name.`),
        h('div', { class: 'actions' }, btn('Change the name', () => cfg.nameDialog()))),
    ];
    if (n.wifi) {
      out.push(h('div', { class: 'key-part' }, h('div', { class: 'setting-label' }, 'Wi-Fi'),
        h('p', {}, store.text(E.wifiNetwork) ? `Connected to ${store.text(E.wifiNetwork)}. ` : '',
          'To move it to another network, join it here: the feeder switches at once and this page loses it.'),
        h('div', { class: 'actions' }, btn('Join another network', () => cfg.wifiDialog()))));
    }
    if (n.ap !== 'off') {
      out.push(h('div', { class: 'key-part' }, h('div', { class: 'setting-label' }, 'Access point password'),
        h('p', {}, 'When the feeder cannot join Wi-Fi it opens its own access point. ' +
                   (n.ap === 'device' ? 'Its password is set here.'
                                      : 'It still has the password it was built with: change it, especially on a ' +
                                        'prebuilt binary, where that password is published.')),
        h('div', { class: 'actions' }, btn('Change the password', () => cfg.apDialog(), n.ap !== 'device'))));
    }
    return out;
  },

  step2() {
    const box = h('div', {});
    const rows = [timezonePicker(), setting('Latitude', null, E.latitude), setting('Longitude', null, E.longitude)]
      .filter(Boolean);
    this.rows.push(...rows);
    append(box, h('p', {}, 'The time zone decides when "08:00" is, and when daylight saving changes. Latitude and ' +
                           'longitude are only needed for sunrise and sunset events: decimal degrees, north and east ' +
                           'positive (a map app shows them for a place).'), rows);
    this.timeRows.push(boundRow(box, 'Feeder time', () => {
      const t = parseDeviceTime(store.text(E.deviceTime));
      return t ? `${formatClock(t.hour, t.minute)} (UTC${t.offset})` : 'Not set yet';
    }));
    return box;
  },

  step3() {
    // Configuration's own card, kept up to date from here.
    const hw = PAGES.configuration.hardwareCard.call(this);
    if (!hw) return h('p', {}, 'This build has fixed pins.');
    const parts = [...hw.children].slice(1);  // without the card's own title
    return [h('p', {}, 'Which board this is, and where the servo, the button and the LED are wired. The pins are ' +
                       'used after a restart.'), parts];
  },

  step4() {
    // Configuration's keys card fills everything after its first child (its title there; a placeholder here).
    const box = h('div', {}, h('span', { hidden: true }));
    this.keys = box;
    this.keyRows = [setting('Bluetooth may change the safety limits', null, E.bleGuard),
                    setting('Bluetooth may change the calibration', null, E.bleCalibration)].filter(Boolean);
    this.rows.push(...this.keyRows);
    this.newKey = PAGES.configuration.newKey;
    this.after = () => PAGES.configuration.updateKeys.call(this);
    return [h('p', {}, 'Home Assistant and Bluetooth clients prove themselves with keys. Home Assistant sets its own ' +
                       'when you add the feeder (Settings, Devices and services). A Bluetooth client needs one made ' +
                       'here.'), box];
  },

  step5() {
    const box = h('div', {}, h('span', { hidden: true }));
    this.pw = box;
    this.passwordDialog = PAGES.configuration.passwordDialog;
    this.unlockDialog = PAGES.configuration.unlockDialog;
    this.after = () => PAGES.configuration.updatePassword.call(this);
    return [h('p', {}, 'Optional. With a password, changing the safety limits, the calibration, the pins, the keys ' +
                       'or the network from this app needs it. Feeding and the schedule never do.'), box];
  },

  step6() {
    const restart = store.bool(E.restartNeeded) === true || (auth.network && auth.network.restart);
    return [
      h('p', {}, 'Last, line the tray up with the opening: Maintenance has a walkthrough (set the servo range, ' +
                 'then nudge each slot). Then load food with Tray reload and set a schedule.'),
      restart ? h('p', {}, 'A change from setup is waiting for a restart; Configuration has the Restart button.') : null,
      h('div', { class: 'actions' },
        h('button', { class: 'btn btn-primary', onclick: () => this.finish('#maintenance') }, 'Finish and calibrate'),
        h('button', { class: 'btn', onclick: () => this.finish('#dashboard') }, 'Finish')),
    ];
  },

  refreshAuth() {
    if (!this.root) return;
    const name = this.STEPS[this.step];
    if (name === 'Network') this.render();
    else if (name === 'Keys') PAGES.configuration.updateKeys.call(this);
    else if (name === 'Settings password') PAGES.configuration.updatePassword.call(this);
  },

  update() {
    for (const r of this.rows) r._update();
    for (const u of this.timeRows) u();
  },
};
