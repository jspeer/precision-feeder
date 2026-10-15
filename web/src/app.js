// The shell: header, banner, tabs and routing, then start the connection. Tabs appear only for what this build
// contains (the entities each needs) and only once its page exists; nothing is configured in the app itself.

const TABS = [
  { id: 'dashboard', title: 'Dashboard', short: 'Home', icon: 'grid', shown: () => store.has(E.trayState) },
  { id: 'schedule', title: 'Schedule', short: 'Schedule', icon: 'calendar', shown: () => store.has(E.editProfile) },
  { id: 'configuration', title: 'Configuration', short: 'Config', icon: 'sliders', shown: () => store.has(E.dailyCap) },
  { id: 'maintenance', title: 'Maintenance', short: 'Service', icon: 'tool',
    shown: () => store.has(E.calibrationMode) || store.has(E.declarePosition) || store.has(E.portions24h) },
  { id: 'bluetooth', title: 'Bluetooth', short: 'BLE', icon: 'bluetooth', shown: () => store.has(E.bleSession) },
  { id: 'diagnostics', title: 'Diagnostics', short: 'Diag', icon: 'activity', shown: () => true },
];
// The initial list of entities arrives in a burst; it has settled once nothing new has come for this long.
const SETTLE_MS = 700;

const app = {
  current: null,  // {id, page}
  tabsKey: '',

  start() {
    // ESPHome's index page has no viewport tag, so phones would lay the page out 980 px wide and shrink it.
    if (!document.querySelector('meta[name="viewport"]')) {
      document.head.appendChild(h('meta', { name: 'viewport', content: 'width=device-width, initial-scale=1, viewport-fit=cover' }));
    }
    document.documentElement.lang = navigator.language || 'en';
    const root = document.querySelector('esp-app') || document.body;
    this.titleEl = h('span', { class: 'brand-name' }, 'Precision Feeder');
    this.clockEl = h('span', { class: 'header-clock' });
    this.connEl = h('span', { class: 'pill' });
    this.tabsEl = h('nav', { class: 'tabs', 'aria-label': 'Pages' });
    this.bannerEl = h('div', { class: 'banner-slot' });
    this.pageEl = h('main', { class: 'page', id: 'page', tabindex: '-1' });
    append(root,
      h('header', { class: 'header' },
        h('div', { class: 'header-inner' },
          h('span', { class: 'brand' }, icon('fish', 24), this.titleEl), h('span', { class: 'spacer' }),
          this.clockEl, this.connEl)),
      this.tabsEl,
      h('div', { class: 'container' }, this.bannerEl, this.pageEl),
      h('div', { id: 'toasts', class: 'toasts', 'aria-live': 'polite' }));
    banners.mount(this.bannerEl);
    window.addEventListener('hashchange', () => this.route());
    store.subscribe(changed => this.update(changed));
    store.start();
    this.update(new Set(['*all']));
    // Re-check the route once the first burst of entities has settled.
    const settle = setInterval(() => {
      if (this.settled()) {
        clearInterval(settle);
        this.update(new Set(['*settled']));
      }
    }, 200);
  },

  settled() {
    return store.status === 'online' && store.entities.size > 0 && Date.now() - store.lastNewEntity > SETTLE_MS;
  },

  visibleTabs() {
    return TABS.filter(t => PAGES[t.id] && t.shown());
  },

  update(changed) {
    this.updateHeader();
    this.updateTabs();
    banners.update();
    this.route();
    if (this.current && this.current.page.update) this.current.page.update(changed);
  },

  updateHeader() {
    const title = store.title || 'Precision Feeder';
    setText(this.titleEl, title);
    if (document.title !== title) document.title = title;
    const t = parseDeviceTime(store.text(E.deviceTime)) || parseLocalDateTime(store.text(E.deviceTime));
    setText(this.clockEl, t ? formatClock(t.hour, t.minute) : '');
    const online = store.status === 'online';
    setText(this.connEl, online ? 'Online' : store.everOnline ? 'Offline' : 'Connecting');
    this.connEl.className = `pill ${online ? 'pill-ok' : store.everOnline ? 'pill-danger' : 'pill-muted'}`;
  },

  updateTabs() {
    const tabs = this.visibleTabs();
    const active = this.current ? this.current.id : '';
    const key = tabs.map(t => t.id).join() + '|' + active;
    if (key === this.tabsKey) return;
    this.tabsKey = key;
    clear(this.tabsEl);
    for (const t of tabs) {
      this.tabsEl.appendChild(h('a', { href: `#${t.id}`, class: t.id === active ? 'tab tab-active' : 'tab',
                                      'aria-current': t.id === active ? 'page' : null },
        icon(t.icon, 20), h('span', { class: 'tab-long' }, t.title), h('span', { class: 'tab-short' }, t.short)));
    }
  },

  /** Show the page the address asks for, or the first one there is once the build is known. */
  route() {
    const [want, ...rest] = location.hash.replace(/^#/, '').split('/');
    const tabs = this.visibleTabs();
    let tab = tabs.find(t => t.id === want);
    if (!tab) {
      if (!this.settled()) {  // its entities may still be on their way
        if (!this.current) this.showLoading();
        return;
      }
      tab = tabs[0];
      if (want) history.replaceState(null, '', `#${tab.id}`);
    }
    const sub = rest.join('/');
    if (this.current && this.current.id === tab.id && this.current.sub === sub) return;
    if (this.current && this.current.page.unmount) this.current.page.unmount();
    clear(this.pageEl);
    this.current = { id: tab.id, sub, page: PAGES[tab.id] };
    this.current.page.mount(this.pageEl, sub);
    this.updateTabs();
  },

  showLoading() {
    if (this.pageEl.querySelector('.loading')) return;
    clear(this.pageEl);
    this.pageEl.appendChild(h('div', { class: 'loading' }, h('span', { class: 'spinner', 'aria-hidden': 'true' }),
      h('span', {}, 'Connecting to the feeder…')));
  },
};

app.start();
