import { defineConfig, devices } from '@playwright/test';

const ci = process.env.CI !== undefined;

// Runs against the production build. The mocked specs answer every API
// request inside the browser, so no C++ server, database, or market data is
// needed; live.spec.ts opts into the real API through the preview proxy.
export default defineConfig({
  testDir: 'e2e',
  fullyParallel: true,
  forbidOnly: ci,
  retries: 0,
  reporter: ci ? [['list'], ['html', { open: 'never' }]] : 'list',
  use: {
    baseURL: 'http://127.0.0.1:4173',
    trace: 'retain-on-failure',
  },
  projects: [
    { name: 'desktop', use: { ...devices['Desktop Chrome'], viewport: { width: 1440, height: 900 } } },
    { name: 'mobile', use: { ...devices['Pixel 7'] } },
  ],
  webServer: {
    // CI builds in its own step; locally the suite builds what it tests.
    command: ci ? 'npm run preview' : 'npm run build && npm run preview',
    url: 'http://127.0.0.1:4173',
    reuseExistingServer: !ci,
    timeout: 120_000,
  },
});
