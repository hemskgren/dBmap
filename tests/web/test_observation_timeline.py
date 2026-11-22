"""Exercise the Hub timeline and refresh controls with a real DOM and stub API."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

INDEX = Path(__file__).resolve().parents[2] / "web" / "index.html"


@pytest.mark.parametrize("width", [1280, 500])
def test_observation_timeline_in_browser(tmp_path, width):
    chrome = next(
        (path for name in ("google-chrome", "chromium") if (path := shutil.which(name))),
        None,
    )
    if not chrome:
        pytest.skip("Chrome or Chromium is required for the timeline browser test")

    html = INDEX.read_text()
    script = html.split("    <script>")[-1].split("</script>", 1)[0]

    def block(start, end):
        return script.split(start, 1)[1].split(end, 1)[0]

    production = (
        "function el(" + block("function el(", "function renderObservationStatistics(")
        + "async function refreshObservations("
        + block("async function refreshObservations(", "function formatCounts(")
        + "function showStatus(" + block("function showStatus(", "let createOwnersReady")
        + "document.getElementById('observation-filter-node').addEventListener"
        + block(
            "document.getElementById('observation-filter-node').addEventListener",
            "document.getElementById('refresh-users')",
        )
    )

    checks = r"""
      const get = (id) => document.getElementById(id);
      const expect = (condition, message) => { if (!condition) throw Error(message); };
      const requests = [];
      const copied = [];
      let fail = false;
      const a = '11111111-1111-1111-1111-111111111111';
      const b = '22222222-2222-2222-2222-222222222222';
      const c = '33333333-3333-3333-3333-333333333333';
      const absent = 'AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA';
      const report = (node_id, observation_id, sequence_number, emission_kind, assessment, origin) => ({
        identity: { node_id, observation_id, sequence_number },
        context: { emission_kind, origin, event_time_utc: '2026-11-22T17:24:36Z' },
        acoustic: { assessment, classification: { source_family: 'OBSOLETE' } },
        received_time_utc: '2026-11-22T17:24:36Z',
      });
      const assessment = { schema: 'ear_hyp_v2', path: [
        { id: 'ALARM', confidence: 0.78 }, { id: 'SE_PUBLIC_ALERT', confidence: 0.74 },
        { id: 'ZERO', confidence: 0 }, { id: 'MISSING' },
      ] };
      let reports = [
        report('EAR-C', c, 4, 'complete', {schema: 'ear_hyp_v1', primary: {family: 'alarm', tier: 'possible'}}),
        report('EAR-A', a, 3, 'end', assessment),
        report('EAR-B', b, 2, 'periodic', null, 'critical_path'),
        report('EAR-A', a, 1, 'start', null),
      ];
      const api = async (path) => {
        requests.push(path);
        if (fail) throw Error('Offline');
        if (path === '/api/v1/devices') return [];
        if (path.endsWith('/count')) return { total: reports.length, unique_observations: 3 };
        return { reports };
      };
      const renderDevices = () => {};
      const formatDisplayDateTime = () => '2026-11-22 17:24:36 CET';
      Object.defineProperty(navigator, 'clipboard', {
        value: { writeText: async (value) => copied.push(value) },
      });
      const node = get('observation-filter-node');
      const ids = get('observation-filter-observation');
      const draft = get('observation-id-input');
      const selected = (select) => [...select.selectedOptions].map((option) => option.value);
      const change = (control) => control.dispatchEvent(new Event('change', { bubbles: true }));
      const choose = (select, values) => {
        for (const option of select.options) option.selected = values.includes(option.value);
        change(select);
      };
      const rows = () => [...document.querySelectorAll('.observation-table tbody tr')];
      const keys = () => rows().map((row) => row.querySelector('.timeline-entry').dataset.reportKey);
      const state = () => get('observation-filter-status').textContent;
      const submit = () => get('observation-id-form').dispatchEvent(
        new Event('submit', { cancelable: true })
      );
      const reset = () => get('observation-filter-clear').click();

      (async () => {
        get('dashboard').hidden = false;
        get('tab-observations').hidden = false;
        for (const key of ['hub', 'devices', 'observation']) {
          const time = get(key + '-updated');
          expect(time.hidden, 'timestamp must start hidden');
          expect(time.previousElementSibling.tagName === 'H2', 'timestamp beside H2');
        }
        markUpdated('hub');
        await refreshDevices();
        const refresh = refreshObservations();
        expect(get('observation-refresh-status').textContent.includes('Loading'), 'loading state');
        await refresh;
        expect(requests.includes('/api/v1/observations?page=1&page_size=200&order=newest'), '200 report request');
        for (const key of ['hub', 'devices', 'observation']) {
          expect(get(key + '-updated').textContent.includes('17:24:36 CET'), 'local timestamp');
          expect(Boolean(get(key + '-updated').dateTime), 'machine-readable timestamp');
        }
        expect(!get('observation-refresh-status').textContent.includes('Updated'), 'duplicate timestamp');
        const columns = [...document.querySelectorAll('.observation-table th')];
        expect(columns.map((th) => th.textContent).join('|') ===
          'Time|Node|Observation ID|Emission|Assessment', 'column order');
        expect(getComputedStyle(columns[0].closest('table')).tableLayout === 'fixed', 'stable layout');
        expect(new Set(rows().map((row) => row.cells[2].getBoundingClientRect().width)).size === 1,
          'consistent ID widths');
        const idCell = rows()[1].cells[2];
        expect(getComputedStyle(idCell).display ===
          (matchMedia('(max-width: 900px)').matches ? 'grid' : 'table-cell'), 'responsive layout');
        expect(idCell.scrollWidth <= idCell.clientWidth + 1, 'UUID and actions fit in cell');
        expect(keys().join() === 'EAR-C:4,EAR-A:3,EAR-B:2,EAR-A:1', 'newest order and all kinds');
        expect(rows()[1].cells[2].querySelector('code').textContent === a, 'full UUID');
        expect(rows()[1].cells[4].textContent ===
          'ALARM 0.78 / SE_PUBLIC_ALERT 0.74 / ZERO 0.00 / MISSING', 'ordered assessment');
        expect(rows()[0].cells[4].textContent === 'alarm (possible)', 'legacy assessment');
        expect(rows()[2].cells[4].textContent === 'Critical path · No assessment', 'critical report');
        expect(rows()[3].cells[4].textContent === 'No assessment', 'absent assessment');

        choose(node, ['EAR-A', 'EAR-B']);
        expect([...ids.options].find((option) => option.value === c).parentElement.label === 'Other nodes',
          'cross-node UUID must remain available');
        choose(ids, [a, b]);
        expect(keys().join() === 'EAR-A:3,EAR-B:2,EAR-A:1', 'OR within each group and AND between them');
        draft.value = '  ' + absent + '  ';
        submit();
        expect(selected(ids).includes(absent.toLowerCase()) && selected(ids).length === 3, 'UUID paste adds');
        draft.value = '11111111';
        submit();
        expect(draft.getAttribute('aria-invalid') === 'true' && selected(ids).length === 3,
          'prefix rejected without removing selection');
        expect(state().includes('Outside the latest 200'), 'out-of-window warning');

        choose(node, ['EAR-C']);
        expect(!rows().length && state().includes('Outside the selected nodes'), 'explicit conflict');
        expect(selected(ids).length === 3 && selected(node)[0] === 'EAR-C', 'conflict preserves filters');
        choose(node, ['EAR-A', 'EAR-B']);
        const checks = [...document.querySelectorAll('.observation-emissions input')];
        for (const box of checks.filter((box) => ['periodic', 'complete'].includes(box.value))) {
          box.checked = false;
          change(box);
        }
        expect(keys().join() === 'EAR-A:3,EAR-A:1', 'Start + End only');
        expect(document.querySelector('.obs-slot-bar').textContent === '2', 'chart shares list filter');
        const details = get('latest-observation-details');
        details.open = false;
        rows()[1].querySelector('[aria-label^="Copy"]').click();
        await Promise.resolve();
        expect(copied.length === 1 && copied[0] === a && !details.open, 'Copy uses full UUID only');
        rows()[1].querySelector('[aria-label^="Filter"]').click();
        expect(selected(ids).join() === a && selected(node).length === 2 && !details.open,
          'Filter replaces UUIDs while preserving nodes and independent JSON navigation');
        expect(keys().join() === 'EAR-A:3,EAR-A:1', 'Filter preserves emissions and order');
        rows()[1].querySelector('.timeline-entry').click();
        expect(details.open && JSON.parse(get('latest-observation-json').textContent).identity.sequence_number === 1,
          'same UUID reports open distinct JSON');
        document.querySelector('.obs-slot-bar').click();
        expect(rows().every((row) => row.classList.contains('in-slot-focus')), 'chart focuses exact visible reports');

        reports = reports.filter((entry) => entry.identity.observation_id !== a);
        await refreshObservations({ force: true });
        expect(selected(ids).join() === a && state().includes('Outside the latest 200'),
          'refresh retains aged-out selection');
        const lastUpdated = get('observation-updated').dateTime;
        const requestCount = requests.length;
        await refreshObservations();
        expect(requests.slice(requestCount).join() === '/api/v1/observations/count', 'paused totals-only request');
        expect(get('observation-updated').dateTime === lastUpdated, 'paused timestamp unchanged');
        reset();
        expect(!selected(ids).length && !selected(node).length &&
          checks.every((box) => box.checked) && !draft.value && draft.getAttribute('aria-invalid') === 'false',
          'Clear restores defaults');
        for (const box of checks) {
          box.checked = false;
          change(box);
        }
        expect(!rows().length && !document.querySelector('.obs-slot-bar'), 'zero emissions match nothing');
        fail = true;
        await refreshObservations({ force: true });
        await refreshDevices();
        expect(get('observation-refresh-status').textContent.includes('Could not load'), 'observation error retained');
        expect(get('refresh-status').textContent === 'Offline', 'device error retained');
        document.body.dataset.testResult = 'passed';
      })().catch((error) => { document.body.dataset.testResult = error.message; });
    """
    markup = re.sub(r"<script\b[^>]*>.*?</script>", "", html, flags=re.DOTALL)
    fixture = tmp_path / "timeline.html"
    fixture.write_text(markup.replace("</body>", f"<script>{production}\n{checks}</script></body>"))
    result = subprocess.run(
        [
            chrome, "--headless", "--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
            "--disable-background-networking", f"--user-data-dir={tmp_path / 'profile'}",
            f"--window-size={width},900", "--dump-dom", fixture.as_uri(),
        ],
        capture_output=True, text=True, timeout=30, check=True,
    )
    outcome = re.search(r'data-test-result="([^"]*)"', result.stdout)
    assert outcome and outcome[1] == "passed", outcome[1] if outcome else result.stderr[-2000:]
