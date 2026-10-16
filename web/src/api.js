// Changes to the device: ESPHome's web server actions (POST /<domain>/<name>/<action>), one at a time, so the
// device's few HTTP sockets are never flooded and writes arrive in the order they were made. The device answers
// 200 as soon as it has queued an action; whether it took effect shows up as the entity's next state, which the
// firmware publishes within about a second (waitFor). Nothing here decides anything about feeding: the device's
// feed guard does, for every request. Which settings need the settings password is the device's list, too.

const api = {
  _queue: Promise.resolve(),

  /** Run an action on an entity, after every earlier one. Rejects if the device refuses or cannot be reached. */
  call(id, action, params) {
    const slash = id.indexOf('/');
    const domain = id.slice(0, slash), name = id.slice(slash + 1);
    const query = params ? Object.entries(params)
      .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(String(v))}`).join('&') : '';
    const url = `/${domain}/${encodeURIComponent(name)}/${action}${query ? '?' + query : ''}`;
    // From the origin, not relative: a page opened as http://user:pass@feeder/ would otherwise put the credentials
    // into every request URL, which browsers refuse. The browser still sends the cached credentials.
    // A protected setting (guard limits, calibration) carries the settings password's proof (auth.js), made just
    // before sending because each challenge is good for one request.
    const run = async () => {
      let headers = {};
      if (auth.isProtected(id)) {
        const value = params ? (params.value !== undefined ? params.value : params.option) : undefined;
        try {
          headers = await auth.headersFor(`/${domain}/${name}/${action}`, value);
        } catch (e) {
          throw e instanceof TypeError ? new Error('The feeder could not be reached.') : e;
        }
      }
      let r;
      try {
        r = await fetch(location.origin + url, { method: 'POST', headers });
      } catch (e) {
        throw new Error('The feeder could not be reached.');
      }
      if (r.status === 403 || r.status === 429) throw await auth.refusal(r);
      if (!r.ok) throw new Error(r.status === 404 ? 'This build does not have that control.' : `The feeder answered ${r.status}.`);
    };
    const done = this._queue.then(run, run);
    this._queue = done.catch(() => {});
    return done;
  },
  press(id) { return this.call(id, 'press'); },
  turn(id, on) { return this.call(id, on ? 'turn_on' : 'turn_off'); },
  setNumber(id, value) { return this.call(id, 'set', { value }); },
  setOption(id, option) { return this.call(id, 'set', { option }); },
  setText(id, value) { return this.call(id, 'set', { value }); },

  /** Resolve with the entity once pred(entity) holds (now or after an update); reject after `ms`. */
  waitFor(id, pred, ms = 4000) {
    return new Promise((resolve, reject) => {
      let timer = 0;
      const unwatch = store.watch(changed => {
        if (changed !== id) return;
        const e = store.get(id);
        if (e && pred(e)) finish(e);
      });
      const finish = e => { clearTimeout(timer); unwatch(); resolve(e); };
      const now = store.get(id);
      if (now && pred(now)) return finish(now);
      timer = setTimeout(() => { unwatch(); reject(new Error('The feeder did not confirm the change.')); }, ms);
    });
  },

  /** Resolve true once done() holds (now, or after one of `ids` changes), false after `ms` (never rejects).
   *  done() is checked first: the device's update can arrive before the request that caused it has returned. */
  waitForChange(ids, done, ms = 3500) {
    return new Promise(resolve => {
      if (done()) {
        resolve(true);
        return;
      }
      const timer = setTimeout(() => { unwatch(); resolve(false); }, ms);
      const unwatch = store.watch(id => {
        if (ids.includes(id) && done()) {
          clearTimeout(timer);
          unwatch();
          resolve(true);
        }
      });
    });
  },

  /** Turn a switch on or off and wait until the device reports it. */
  turnAndConfirm(id, on, ms = 4000) {
    return this.turn(id, on).then(() => this.waitFor(id, x => x.value === on, ms));
  },

  /** Set a number, select or text, then wait until the device reports the new value. */
  setAndConfirm(id, value, ms = 4000) {
    const e = store.get(id);
    if (!e) return Promise.reject(new Error('This build does not have that setting.'));
    let write, same;
    if (e.domain === 'number' || id.startsWith('number/')) {
      const step = parseFloat(e.step) || 1;
      write = this.setNumber(id, value);
      same = x => Math.abs(parseFloat(x.value) - parseFloat(value)) < step / 2;
    } else if (id.startsWith('select/')) {
      write = this.setOption(id, value);
      same = x => x.value === value;
    } else {
      write = this.setText(id, value);
      same = x => x.value === value;
    }
    return write.then(() => this.waitFor(id, same, ms));
  },
};
