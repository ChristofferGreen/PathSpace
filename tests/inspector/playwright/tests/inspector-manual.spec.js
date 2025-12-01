const { test, expect } = require('@playwright/test');
const { mutateInspector, disableEventSource, addWatchFromSearch } = require('./utils');

async function addWatch(page, path) {
  await page.fill('#watch-input', path);
  await page.click('#watch-form button[type="submit"]');
}

test.describe('Inspector manual refresh fallback', () => {
  test('maintains search/watchlist coverage without SSE', async ({ page }, testInfo) => {
    await disableEventSource(page);
    await page.goto('/');

    await expect(page.locator('#status-badge')).toHaveText(/Polling|Idle/i);

    await page.fill('#root-input', '/demo/widgets');
    await page.fill('#depth-input', '4');
    await page.fill('#children-input', '8');
    await page.click('#refresh-tree');
    const widgetRow = page.locator('#tree [data-path="/demo/widgets"]');
    await expect(widgetRow).toBeVisible();

    await addWatch(page, '/demo/widgets/button');
    await addWatch(page, '/demo/widgets/missing/path');
    await addWatch(page, '/system/status/current');

    await expect(page.locator('#watch-live-badge')).toHaveText(/Live\s+1/);
    await expect(page.locator('#watch-missing-badge')).toHaveText(/Missing\s+1/);
    await expect(page.locator('#watch-scope-badge')).toHaveText(/Out of scope\s+1/);

    await page.fill('#search-input', 'button');
    await page.click('#run-search');
    await expect(page.locator('#search-results li').first()).toContainText('/demo/widgets/button');

    await mutateInspector(testInfo, {
      set: [
        { path: '/demo/widgets/manual/only', value: 'manual-refresh' },
      ],
    });

    const manualRow = page.locator('#tree [data-path="/demo/widgets/manual/only"]');
    await expect(manualRow).toHaveCount(0, { timeout: 1_000 });

    await page.click('#refresh-tree');
    await expect(manualRow).toBeVisible();

    await addWatchFromSearch(page, 'manual/only', '/demo/widgets/manual/only');
    await expect(page.locator('#watch-live-badge')).toHaveText(/Live\s+2/);
    await expect(page.locator('#search-query-badge')).toHaveText(/[1-9]\d*\s+queries/);

    const manualWatch = page
      .locator('#watchlist .watch-entry')
      .filter({ hasText: '/demo/widgets/manual/only' })
      .first();
    await manualWatch.getByRole('button', { name: 'Open' }).click();
    await expect(page.locator('#node-json')).toContainText('manual-refresh');
  });
});
