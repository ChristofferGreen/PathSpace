const { expect } = require('@playwright/test');
const AxeBuilder = require('@axe-core/playwright').default;

function resolveBaseUrl(testInfo) {
  const explicit = process.env.INSPECTOR_TEST_BASE_URL;
  const projectUrl = testInfo?.project?.use?.baseURL;
  return projectUrl || explicit || 'http://127.0.0.1:8765';
}

async function mutateInspector(testInfo, payload) {
  const baseUrl = resolveBaseUrl(testInfo);
  if (!baseUrl) {
    throw new Error('INSPECTOR_TEST_BASE_URL is not set');
  }
  const target = new URL('/inspector/test/mutate', baseUrl);
  const response = await fetch(target, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!response.ok) {
    const text = await response.text();
    throw new Error(`Inspector mutate failed: ${response.status} ${text}`);
  }
  return response.json();
}

async function disableEventSource(page) {
  await page.addInitScript(() => {
    try {
      Object.defineProperty(window, 'EventSource', {
        configurable: true,
        writable: true,
        value: undefined,
      });
    } catch (error) {
      window.EventSource = undefined;
    }
  });
}

async function addWatchFromSearch(page, query, matchText) {
  await page.fill('#search-input', query);
  await page.click('#run-search');
  const results = page.locator('#search-results li');
  const target = matchText ? results.filter({ hasText: matchText }).first() : results.first();
  await expect(target).toBeVisible({ timeout: 10_000 });
  await target.getByRole('button', { name: 'Watch' }).click();
}

async function expectNoAxeViolations(page, options = {}) {
  const builder = new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa']);
  const includes = options.include || [];
  const excludes = options.exclude || [];
  includes.forEach((selector) => builder.include(selector));
  excludes.forEach((selector) => builder.exclude(selector));
  const results = await builder.analyze();
  const violations = results.violations || [];
  if (violations.length) {
    const summary = violations
      .map((violation) => {
        const targets = violation.nodes?.map((node) => node.target.join(' ')).join(', ') || 'unknown target';
        return `${violation.id}: ${violation.help} (${targets})`;
      })
      .join('\n');
    throw new Error(`Accessibility violations:\n${summary}`);
  }
}

module.exports = {
  mutateInspector,
  disableEventSource,
  addWatchFromSearch,
  expectNoAxeViolations,
};
