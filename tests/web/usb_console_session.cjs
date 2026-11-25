const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const elements = new Map();
const events = {};
const controls = {};
const requests = [];
const timers = new Map();
const context = vm.createContext({
  document: {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, {
        textContent: '', children: [], addEventListener: (name, cb) => { controls[`${id}:${name}`] = cb; },
        replaceChildren() { this.children = []; },
        append(item) { this.children.push(item); },
        classList: { toggle() {} },
      });
      return elements.get(id);
    },
    createElement: () => ({}),
  },
  navigator: { serial: { addEventListener: (name, cb) => { events[name] = cb; } } },
  api: (path) => new Promise((resolve, reject) => requests.push({ path, resolve, reject })),
  window: {
    setTimeout(cb) { const id = timers.size + 1; timers.set(id, cb); return id; },
    clearTimeout(id) { timers.delete(id); },
  },
  setInterval() {},
  requestAnimationFrame() { return 1; },
  TextDecoder,
});
vm.runInContext(fs.readFileSync(0, 'utf8'), context);
const run = (source) => vm.runInContext(source, context);
const status = () => elements.get('usb-console-status').textContent;
const summary = () => elements.get('usb-console-baseline').textContent;
const comparison = () => elements.get('usb-console-comparison');
const comparisonTitle = () => elements.get('usb-console-comparison-title').textContent;
const changes = () => elements.get('usb-console-changes').children.map((item) => item.textContent);

function port() {
  let finish;
  return {
    getInfo: () => ({ usbVendorId: 0x1a86, usbProductId: 0x55d3 }),
    async open(options) { assert.equal(options.baudRate, 115200); },
    async close() {},
    readable: {
      getReader: () => ({
        read: () => new Promise((resolve) => { finish = resolve; }),
        async cancel() { finish?.({ done: true }); },
        releaseLock() {},
      }),
    },
  };
}

const chunk = (line) => {
  context.chunk = line;
  run('appendUsbConsoleText(usbConsoleSession, chunk)');
};
const snapshot = (nodeId, diag) => ({
  node_id: nodeId,
  last_seen_at: new Date().toISOString(),
  reported: { payload: { node_id: nodeId, firmware_version: '0.8.39', uptime_s: 60, diag } },
});

(async () => {
  context.testPort = port();
  await run('openUsbConsole(testPort)');
  assert.match(status(), /Console open · 0x1a86:0x55d3 · Identifying/);
  [...timers.values()][0]();
  assert.match(status(), /Unidentified/);
  chunk('I (60000) platform_boot: node_id=EAR-0');
  assert.equal(requests.length, 0);
  chunk('01 firmware_version=0.8.39 uptime_s=60\r\n');
  assert.match(status(), /EAR-001 · Firmware 0.8.39 · Uptime 60s/);
  assert.equal(requests[0].path, '/api/v1/nodes/EAR-001');
  requests[0].resolve(snapshot('EAR-001', { ce_created: 10, ce_active: 1, sig_accepted: 1 }));
  await new Promise(setImmediate);
  chunk('I (60001) ear_obs: ce diag active=4 created=12 expired=4\n');
  chunk('I (60002) ae: diag sig=20\n');
  assert.deepEqual(changes(), ['ce_created: 10 → 12 (+2)']);
  chunk('I (60003) ear_obs: ce diag created=11\n');
  assert.deepEqual(changes(), ['ce_created: Hub 10, USB 11 (counter reset or baseline mismatch)']);
  chunk('I (10) platform_boot: node_id=EAR-001 firmware_version=0.8.39 uptime_s=0\n');
  assert.match(summary(), /invalidated/);
  assert.deepEqual(changes(), []);
  assert.equal(requests.length, 1);
  await controls['usb-console-close:click']();
  assert.match(status(), /Last connected · EAR-001/);
  assert.match(comparisonTitle(), /Last captured \/ Disconnected/);
  assert.match(summary(), /invalidated/);

  context.testPort = port();
  await run('openUsbConsole(testPort)');
  assert.equal(comparison().hidden, true);
  assert.deepEqual(changes(), []);
  chunk('I (60000) platform_boot: node_id=EAR-001 firmware_version=0.8.39 uptime_s=60\n');
  assert.equal(requests[1].path, '/api/v1/nodes/EAR-001');
  requests[1].resolve(snapshot('EAR-001', { ce_created: 20 }));
  await new Promise(setImmediate);
  chunk('I (60001) ear_obs: ce diag created=23\n');
  const capturedSummary = summary();
  assert.match(capturedSummary, /Hub receipt:.*Boot times consistent.*Last USB counter sample:/);
  assert.deepEqual(changes(), ['ce_created: 20 → 23 (+3)']);
  // Close via the button, leaving the captured comparison visible and expandable.
  comparison().open = true;
  await controls['usb-console-close:click']();
  assert.match(status(), /Last connected · EAR-001 · Firmware 0.8.39 · Uptime 60s/);
  assert.match(comparisonTitle(), /Last captured \/ Disconnected/);
  assert.equal(comparison().hidden, false);
  assert.equal(comparison().open, true);
  assert.equal(summary(), capturedSummary);
  assert.deepEqual(changes(), ['ce_created: 20 → 23 (+3)']);

  context.testPort = port();
  await run('openUsbConsole(testPort)');
  assert.equal(comparison().hidden, true);
  assert.equal(comparison().open, false);
  assert.deepEqual(changes(), []);
  chunk('I (60000) platform_boot: node_id=EAR-001 firmware_version=0.8.39 uptime_s=60\n');
  requests[2].resolve(snapshot('EAR-001', { ce_created: 25 }));
  await new Promise(setImmediate);
  chunk('I (60001) ear_obs: ce diag created=26\n');
  events.disconnect({ target: context.testPort });
  assert.match(status(), /Last connected · EAR-001.*USB disconnected/);
  assert.match(comparisonTitle(), /Last captured \/ Disconnected/);
  assert.equal(comparison().hidden, false);
  assert.deepEqual(changes(), ['ce_created: 25 → 26 (+1)']);
  await new Promise(setImmediate);

  context.testPort = port();
  await run('openUsbConsole(testPort)');
  assert.match(status(), /Identifying/);
  chunk('node_id=EAR-002\n');
  await run('stopUsbConsole()');
  assert.match(status(), /Last connected · EAR-002/);
  assert.match(summary(), /not captured before disconnect/);
  const closedSummary = summary();
  requests[3].resolve(snapshot('EAR-002', { ce_created: 10 }));
  await new Promise(setImmediate);
  assert.equal(summary(), closedSummary);
  context.testPort = port();
  await run('openUsbConsole(testPort)');
  chunk('I (60100) platform_boot: node_id=EAR-003 firmware_version=0.8.39 uptime_s=60\n');
  await new Promise(setImmediate);
  assert.match(status(), /EAR-003/);
  assert.match(summary(), /Loading/);
  requests[4].resolve(snapshot('EAR-003', null));
  await new Promise(setImmediate);
  assert.match(summary(), /No usable/);
  await run('stopUsbConsole()');

  context.testPort = port();
  await run('openUsbConsole(testPort)');
  chunk('I (60000) esp-output: node_id=OUT-001 firmware_version=0.8.39 uptime_s=60\n');
  assert.match(status(), /OUT-001 · Firmware 0.8.39 · Uptime 60s/);
  assert.equal(requests.length, 5);
  assert.equal(elements.get('usb-console-comparison').hidden, true);
  await run('stopUsbConsole()');

  context.testPort = port();
  await run('openUsbConsole(testPort)');
  chunk('DBMAP_DEVICE_ID=EAR-004\n');
  requests[5].resolve(snapshot('EAR-004', { ce_created: 3 }));
  await new Promise(setImmediate);
  chunk('I (60001) ear_obs: ce diag created=8\n');
  assert.match(summary(), /unverified/);
  assert.deepEqual(changes(), []);
  await run('stopUsbConsole()');

  context.testPort = port();
  await run('openUsbConsole(testPort)');
  chunk('node_id=EAR-005\n');
  await run('stopUsbConsole()');
  context.testPort = port();
  await run('openUsbConsole(testPort)');
  chunk('node_id=EAR-006\n');
  requests[6].resolve(snapshot('EAR-005', { ce_created: 10 }));
  await new Promise(setImmediate);
  assert.match(status(), /EAR-006/);
  assert.match(summary(), /Loading/);
  requests[7].reject(new Error('Forbidden'));
  await new Promise(setImmediate);
  assert.match(summary(), /unavailable/);
  assert.match(status(), /Console open · EAR-006/);
  await run('stopUsbConsole()');
})().catch((error) => { console.error(error); process.exitCode = 1; });
