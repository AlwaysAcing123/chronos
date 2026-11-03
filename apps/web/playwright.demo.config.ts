import { defineConfig, devices } from '@playwright/test';

// The real-stack acceptance suite of the demo workspace (ADR 0025). The
// harness, tools/demo/acceptance.py, starts the actual supervisor, which
// serves this build with its PostgreSQL, trading server, research service,
// and inference workers; this config only points the browser at it. There
// is no webServer and no route mock.
const baseURL = process.env.CHRONOS_DEMO_URL;
const evidence = process.env.CHRONOS_DEMO_EVIDENCE ?? 'test-results/demo';

export default defineConfig({
  testDir: 'e2e',
  testMatch: 'demo-acceptance.spec.ts',
  fullyParallel: false,
  workers: 1,
  retries: 0,
  forbidOnly: true,
  timeout: 600_000,
  reporter: [['list'], ['json', { outputFile: `${evidence}/playwright-report.json` }]],
  outputDir: `${evidence}/playwright-output`,
  use: {
    ...(baseURL === undefined ? {} : { baseURL }),
    // A control that never appears fails in seconds, not at the test timeout.
    actionTimeout: 30_000,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    { name: 'desktop', use: { ...devices['Desktop Chrome'], viewport: { width: 1440, height: 900 } } },
    { name: 'mobile', use: { ...devices['Pixel 7'] } },
  ],
});
