// Opt-in acceptance of strategy diagnostics against a real, isolated research
// service, by explicit IDs, through the preview proxy:
//   CHRONOS_WEB_LIVE_DIAGNOSTICS=<diagnostic-id>        diagnostics to read, reload, and deep-link (reads only)
//   CHRONOS_WEB_LIVE_DIAGNOSTICS_STUDY=<study-id>       a synthetic study to explain from its page (one POST)
//   CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:18190 npx playwright test diagnostics-live.spec.ts --workers=1
// The trading API is answered inside the browser, so nothing reaches a
// trading server, and nothing here fits, schedules, runs, or acquires.
import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { mockChronosApi } from './mockApi';

const diagnosticId = process.env.CHRONOS_WEB_LIVE_DIAGNOSTICS ?? '';
const syntheticStudy = process.env.CHRONOS_WEB_LIVE_DIAGNOSTICS_STUDY ?? '';

async function expectAccessibleAndContained(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  expect(overflow).toBeLessThanOrEqual(0);
}

async function openAccount(page: Page, accountId: string) {
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill(accountId);
  await create.getByRole('button', { name: 'Create paper account' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
}

test('reads real diagnostics level by level, explains a run, reloads its deep link, and keeps the paper account', async ({
  page,
}, testInfo) => {
  test.skip(diagnosticId === '', 'set CHRONOS_WEB_LIVE_DIAGNOSTICS to a diagnostics ID');
  test.setTimeout(240_000);
  await mockChronosApi(page);
  await openAccount(page, 'e2e-live-diagnostics-1');
  await page.goto(`/#/research/walk-forward/diagnostics/${diagnosticId}`);
  await expect(page.getByRole('heading', { level: 1, name: 'Strategy diagnostics' })).toBeVisible();
  const levels = page.getByRole('table', { name: 'Each level of verification and its state' });
  await expect(levels.getByText('Verified on this load')).toBeVisible({ timeout: 120_000 });
  await expect(levels.getByText('Match what was read')).toBeVisible();
  await expect(levels.getByText('Not run by this page')).toHaveCount(2);
  await expect(page.getByText('Some sources no longer match what the diagnostics read')).toHaveCount(0);
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-diagnostics-overview.png'), fullPage: true });

  const runs = page.getByRole('table', { name: /run, explained/ });
  const logistic = runs.getByRole('row', { name: /Logistic regression · Base/ }).first();
  await logistic.getByRole('link', { name: /^Explain/ }).click();
  const selected = page.getByRole('region', { name: /Logistic regression · Base · Strategy/ });
  const bridge = selected.getByRole('table', { name: 'How the booked net PnL arises, term by term' });
  await expect(bridge.getByRole('row', { name: /= Booked net PnL/ })).toBeVisible();
  await expect(selected.getByText('Reconciles exactly').first()).toBeVisible();
  await expect(selected.getByRole('table', { name: /Every fill of this account/ })).toBeVisible({ timeout: 60_000 });
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-diagnostics-run.png'), fullPage: true });

  const link = page.url();
  await page.reload();
  await expect(page.getByRole('region', { name: /Logistic regression · Base · Strategy/ })).toBeVisible({ timeout: 120_000 });
  expect(page.url()).toBe(link);
  await page.getByRole('navigation', { name: 'Simulated account' }).getByRole('link', { name: 'Buy and hold' }).click();
  await expect(page.getByRole('region', { name: /Logistic regression · Base · Buy and hold/ })).toBeVisible();
  await page.getByRole('link', { name: 'Back to trading' }).click();
  await expect(page.getByRole('banner').getByText('e2e-live-diagnostics-1')).toBeVisible();
});

test('explains a synthetic study from its page through a real job', async ({ page }, testInfo) => {
  test.skip(syntheticStudy === '', 'set CHRONOS_WEB_LIVE_DIAGNOSTICS_STUDY to a synthetic study ID');
  test.setTimeout(300_000);
  await mockChronosApi(page);
  await page.goto(`/#/research/walk-forward/studies/${syntheticStudy}`);
  const panel = page.getByRole('region', { name: 'Strategy diagnostics' });
  await expect(panel.getByRole('button', { name: 'Explain this study’s runs' })).toBeVisible({ timeout: 120_000 });
  await panel.getByRole('button', { name: 'Explain this study’s runs' }).click();
  await expect(panel.getByText('Diagnostics published')).toBeVisible({ timeout: 240_000 });
  await panel.getByRole('link', { name: 'Open the diagnostics' }).click();
  await expect(page.getByText('Every recording here was generated for tests.', { exact: false })).toBeVisible({ timeout: 60_000 });
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-diagnostics-synthetic.png'), fullPage: true });
});
