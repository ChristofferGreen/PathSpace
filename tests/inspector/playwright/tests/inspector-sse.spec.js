const { test, expect } = require('@playwright/test');
const { mutateInspector, addWatchFromSearch, expectNoAxeViolations } = require('./utils');

const STREAM_STATUS_REGEX = /(Streaming|Live)/i;
const LIVE_PATH = '/inspector/watch/live/state';
const TRUNCATED_PATH = '/inspector/watch/live/deep/node/value';
const REMOTE_PATH = '/system/status/outside';
const BASE_FIXTURE = [
  { path: LIVE_PATH, value: 'stream-initial' },
  { path: TRUNCATED_PATH, value: 'deep-initial' },
  { path: REMOTE_PATH, value: 'system-live' },
  { path: '/inspector/search/buttons/buttonA', value: 'alpha' },
  { path: '/inspector/search/buttons/buttonB', value: 'beta' },
];

async function waitForStreamingReady(page) {
  const badge = page.locator('#status-badge');
  await expect(badge).toHaveText(STREAM_STATUS_REGEX, { timeout: 15_000 });
}

async function submitSnapshotForm(page, overrides = {}) {
  const { root, depth, children } = overrides;
  if (root !== undefined) {
    await page.fill('#root-input', root);
  }
  if (depth !== undefined) {
    await page.fill('#depth-input', String(depth));
  }
  if (children !== undefined) {
    await page.fill('#children-input', String(children));
  }
  const response = page.waitForResponse((res) =>
    res.url().includes('/inspector/tree') && res.request().method() === 'GET');
  await page.click('#refresh-tree');
  await response;
}

test.describe('Inspector SSE flows', () => {
  test('search + watchlist scenarios stay in sync with streaming deltas', async ({ page }, testInfo) => {
    await mutateInspector(testInfo, { clear: true, set: BASE_FIXTURE });

    await page.goto('/');
    await waitForStreamingReady(page);

    await submitSnapshotForm(page, { depth: 6 });

    await addWatchFromSearch(page, 'live/state', LIVE_PATH);
    await addWatchFromSearch(page, 'deep/node/value', TRUNCATED_PATH);
    await addWatchFromSearch(page, 'system/status', REMOTE_PATH);

    await expect(page.locator('#watch-live-badge')).toHaveText(/Live\s+3/);
    await expect(page.locator('#search-query-badge')).toHaveText(/[1-9]\d*\s+queries/);

    await mutateInspector(testInfo, {
      clear: true,
      set: [{ path: REMOTE_PATH, value: 'system-live' }],
    });
    await expect(page.locator('#watch-missing-badge')).toHaveText(/Missing\s+2/);
    await expect(page.locator('#watch-live-badge')).toHaveText(/Live\s+1/);

    await mutateInspector(testInfo, { clear: true, set: BASE_FIXTURE });
    await expect(page.locator('#watch-live-badge')).toHaveText(/Live\s+3/);
    await expect(page.locator('#watch-truncate-badge')).toHaveText(/Truncated\s+0/);

    await submitSnapshotForm(page, { root: '/inspector/watch/live' });
    await expect(page.locator('#watch-scope-badge')).toHaveText(/Out of scope\s+1/);
    await expect(page.locator('#watch-live-badge')).toHaveText(/Live\s+2/);
  });

  test('core panels satisfy axe-core WCAG AA audit', async ({ page }, testInfo) => {
    await mutateInspector(testInfo, { clear: true, set: BASE_FIXTURE });
    await page.goto('/');
    await waitForStreamingReady(page);
    await submitSnapshotForm(page, { depth: 4 });
    await page.locator('[data-panel-id="watchlist"]').scrollIntoViewIfNeeded();
    await page.locator('[data-panel-id="snapshots"]').scrollIntoViewIfNeeded();
    await page.locator('[data-panel-id="paint"]').scrollIntoViewIfNeeded();
    await expectNoAxeViolations(page);
  });
});
