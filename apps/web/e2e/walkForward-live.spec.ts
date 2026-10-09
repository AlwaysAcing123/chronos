// Opt-in acceptance run against a real, isolated research service through
// the preview proxy:
//   CHRONOS_WEB_LIVE_RESEARCH=1 CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:18190 \
//     npx playwright test walkForward-live.spec.ts --workers=1
// It needs `python -m chronos_research serve` over artifact roots holding a
// verified collection of three synthetic recordings and one of a single
// recording (the walk-forward walkthrough builds both). The trading API is
// answered inside the browser, so nothing reaches a trading server.
import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { mockChronosApi } from './mockApi';

test.skip(process.env.CHRONOS_WEB_LIVE_RESEARCH !== '1', 'set CHRONOS_WEB_LIVE_RESEARCH=1 to run against the real service');

async function expectAccessibleAndContained(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  expect(overflow).toBeLessThanOrEqual(0);
}

test('previews, starts, and reads a real study, then opens a fold run’s journal', async ({ page }, testInfo) => {
  test.setTimeout(300_000);
  await mockChronosApi(page);
  await page.goto('/#/research/walk-forward');
  const form = page.getByRole('region', { name: 'Plan a walk-forward study' });
  const collection = form.getByLabel('Collection');
  await expect(collection.locator('option')).toHaveCount(2);
  const options = await collection.locator('option').allTextContents();
  const single = options.find((text) => text.includes('· 1 recording ·'));
  const week = options.find((text) => text.includes('· 3 recordings ·'));
  expect(single).toBeDefined();
  expect(week).toBeDefined();

  // One recording: the page says why Start is impossible and what is needed.
  await collection.selectOption({ label: single ?? '' });
  await expect(form.getByText('Not enough independent recordings for a walk-forward study')).toBeVisible();
  await expect(form.getByRole('button', { name: 'Preview plan' })).toBeDisabled();
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-insufficient.png'), fullPage: true });

  await collection.selectOption({ label: week ?? '' });
  await form.getByRole('button', { name: 'Preview plan' }).click();
  await expect(form.getByText('The plan is ready to review')).toBeVisible({ timeout: 120_000 });
  await form.getByRole('link', { name: 'Review the plan' }).click();
  await expect(page.getByRole('table', { name: /training recordings, cutoff, and evaluation recording/ }).getByRole('row')).toHaveCount(3);
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-plan.png'), fullPage: true });
  await page.reload();
  await expect(page.getByRole('table', { name: 'The frozen fee and latency scenarios' })).toBeVisible();

  const start = page.getByRole('region', { name: 'Start the study' });
  await start.getByRole('button', { name: 'Start study' }).click();
  await expect(start.getByText(/^Study published/)).toBeVisible({ timeout: 240_000 });
  await start.getByRole('link', { name: 'Open the study' }).click();
  const trading = page.getByRole('table', { name: /one row per engine run/ });
  await expect(trading.getByRole('row')).toHaveCount(9);
  await expect(page.getByText('Every recording here was generated for tests.', { exact: false })).toBeVisible();
  await expect(page.getByRole('table', { name: /independent accounts/ }).getByText('Complete: all 2 folds')).toHaveCount(4);
  for (const row of await page.getByRole('table', { name: /whether its artifacts verify/ }).getByRole('row').all()) {
    await expect(row.getByText('Does not verify')).toHaveCount(0);
  }
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-study.png'), fullPage: true });
  await page.reload();
  await expect(page.getByRole('table', { name: /one row per engine run/ })).toBeVisible();

  // A fold run opens like any result, with its journal.
  await trading.getByRole('link', { name: /^Open run/ }).last().click();
  await expect(page.getByText(/Fold 2 window/)).toBeVisible();
  await expect(page.getByRole('region', { name: 'Assumptions and provenance' }).getByText('Information boundary (exclusive)')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Decisions' }).getByRole('row').nth(1)).toBeVisible();
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('live-fold-run.png'), fullPage: true });

  await page.goto('/#/research/walk-forward');
  await expect(page.getByRole('region', { name: 'Study jobs' }).getByText('Succeeded', { exact: true }).first()).toBeVisible();
});
