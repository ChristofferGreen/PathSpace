const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: './tests',
  timeout: 30_000,
  expect: {
    timeout: 5_000,
  },
  workers: 1,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: process.env.INSPECTOR_TEST_BASE_URL || 'http://127.0.0.1:8765',
    headless: true,
    actionTimeout: 5_000,
    navigationTimeout: 20_000,
    trace: 'retain-on-failure',
  },
});
