// Strategy diagnostics in the browser, answered by the mocked research
// service with responses captured from the real one over synthetic
// recordings. diagnostics-live.spec.ts reads a real, isolated service.
import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { DIAGNOSED_RUN_ID, DIAGNOSTIC_ID } from '../src/test/diagnosticsFixtures';
import { PARTIAL_STUDY_ID } from '../src/test/walkForwardFixtures';
import { mockChronosApi } from './mockApi';
import { mockResearchService } from './researchMock';

async function expectNoAccessibilityViolations(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
}

async function expectNoHorizontalOverflow(page: Page) {
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

const RUN_HASH = `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${DIAGNOSED_RUN_ID}`;

// Each full-page accessibility scan of the diagnostics page takes seconds on
// a phone-sized page with every table, so these scans get room.
const SCANNED_MS = 120_000;

test('explains a study’s runs from its page, reloads the deep link, and keeps the paper account', async ({ page }, testInfo) => {
  test.setTimeout(SCANNED_MS);
  await mockChronosApi(page);
  await mockResearchService(page);
  await openAccount(page, 'e2e-diagnostics-1');
  await page.goto(`/#/research/walk-forward/studies/${PARTIAL_STUDY_ID}`);

  const panel = page.getByRole('region', { name: 'Strategy diagnostics' });
  await expect(panel.getByText('No diagnostics of this study exist yet.')).toBeVisible();
  // Keyboard only: request the diagnostics.
  await panel.getByRole('button', { name: 'Explain this study’s runs' }).focus();
  await page.keyboard.press('Enter');
  await expect(panel.getByText('Diagnostics published')).toBeVisible();
  await panel.getByRole('link', { name: 'Open the diagnostics' }).click();
  expect(page.url()).toContain(`#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}`);
  await expect(page.getByRole('heading', { level: 1, name: 'Strategy diagnostics' })).toBeVisible();
  await expect(page.getByRole('table', { name: 'Each level of verification and its state' }).getByText('Not run by this page')).toHaveCount(2);
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('diagnostics-overview.png'), fullPage: true });

  const runs = page.getByRole('table', { name: /run, explained/ });
  await runs.getByRole('row', { name: /Logistic regression · Base/ }).getByRole('link', { name: /^Explain/ }).click();
  expect(page.url()).toContain(`${RUN_HASH}/strategy`);
  const selected = page.getByRole('region', { name: /Logistic regression · Base · Strategy/ });
  const bridge = selected.getByRole('table', { name: 'How the booked net PnL arises, term by term' });
  await expect(bridge.getByRole('row', { name: /= Booked net PnL/ })).toContainText('-0.81');
  await expect(selected.getByRole('table', { name: /Every fill of this account/ })).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('diagnostics-run.png'), fullPage: true });

  // A reload opens the same run.
  await page.reload();
  await expect(page.getByRole('region', { name: /Logistic regression · Base · Strategy/ })).toBeVisible();

  // Keyboard only: the next scenario, by its selector.
  const scenario = page.getByRole('region', { name: /Logistic regression · Base · Strategy/ }).getByLabel('Scenario');
  await scenario.focus();
  await scenario.selectOption('stress');
  await expect(page.getByRole('region', { name: /Logistic regression · Stress · Strategy/ })).toBeVisible();
  expect(page.url()).not.toContain(DIAGNOSED_RUN_ID);

  // The simulated benchmark account, by its link.
  await page.goto(`/${RUN_HASH}/strategy`);
  await page.getByRole('navigation', { name: 'Simulated account' }).getByRole('link', { name: 'Buy and hold' }).click();
  expect(page.url()).toContain(`${RUN_HASH}/buy_and_hold`);
  await expect(page.getByRole('region', { name: /Logistic regression · Base · Buy and hold/ })).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await page.screenshot({ path: testInfo.outputPath('diagnostics-buy-and-hold.png'), fullPage: true });

  // Back to trading, in the same paper account.
  await page.getByRole('link', { name: 'Back to trading' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
  await expect(page.getByRole('banner').getByText('e2e-diagnostics-1')).toBeVisible();
});

test('labels a run whose journal no longer matches, and figures that do not exist, on every width', async ({ page }, testInfo) => {
  test.setTimeout(SCANNED_MS);
  await mockChronosApi(page);
  await mockResearchService(page, { seededDiagnostics: true, corruptDiagnosticsRun: true });
  await page.goto(`/${RUN_HASH}/strategy`);
  await expect(page.getByText('Some sources no longer match what the diagnostics read')).toBeVisible();
  const selected = page.getByRole('region', { name: /Logistic regression · Base · Strategy/ });
  await expect(selected.getByText('Unverified run')).toBeVisible();
  const bridge = selected.getByRole('table', { name: 'How the booked net PnL arises, term by term' });
  await expect(bridge.getByRole('row', { name: /= Booked net PnL/ })).toContainText('Unavailable: The final valuation is unavailable');
  await expect(selected.getByText('Not checkable')).toHaveCount(2);
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('diagnostics-unverified.png'), fullPage: true });
});

for (const source of ['schedule', 'study'] as const) {
  test(`labels the run, cohorts, and pools resting on an invalid ${source}, on every width`, async ({ page }, testInfo) => {
    test.setTimeout(SCANNED_MS);
    await mockChronosApi(page);
    await mockResearchService(page, { seededDiagnostics: true, unverifiedDiagnostics: source });
    await page.goto(`/${RUN_HASH}/strategy`);
    await expect(page.getByText('Some sources no longer match what the diagnostics read')).toBeVisible();
    const selected = page.getByRole('region', { name: /Logistic regression · Base · Strategy/ });
    await expect(selected.getByText('Unverified run', { exact: true })).toBeVisible();
    await expect(selected.getByText('Reconciles exactly', { exact: true })).toHaveCount(0);
    await expect(selected.getByText('Reconciled when generated', { exact: true })).toHaveCount(2);
    const pools = page.getByRole('region', { name: 'AAPL: pooled over folds' });
    await expect(pools.getByText('Unverified pool', { exact: true }).first()).toBeVisible();
    await expect(pools.getByText('Unverified cohort', { exact: true }).first()).toBeVisible();
    await expectNoAccessibilityViolations(page);
    await expectNoHorizontalOverflow(page);
    await page.screenshot({ path: testInfo.outputPath(`diagnostics-unverified-${source}.png`), fullPage: true });
  });
}

test('says when diagnostics do not exist or the service is unreachable', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await page.goto(`/#/research/walk-forward/diagnostics/${'f'.repeat(64)}`);
  await expect(page.getByText('The diagnostics could not be read')).toBeVisible();
  await expectNoAccessibilityViolations(page);
});

test('explains an unreachable research service on the diagnostics page', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { unreachable: true });
  await page.goto(`/#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}`);
  await expect(page.getByText('The research service is not reachable')).toBeVisible();
  await expectNoAccessibilityViolations(page);
});
