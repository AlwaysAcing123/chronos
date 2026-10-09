// Walk-forward evaluation in the browser, answered by the mocked research
// service with responses captured from the real one over synthetic
// recordings. walkForward-live.spec.ts runs the same flow against a real,
// isolated service.
import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { PARTIAL_STUDY_ID, PLAN_ID } from '../src/test/walkForwardFixtures';
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

test('plans, reviews, reloads, and starts a study inside Research, following its job', async ({ page }, testInfo) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await page.goto('/#/research');
  await page.getByRole('navigation', { name: 'Research navigation' }).getByRole('link', { name: 'Walk-forward evaluation' }).click();
  await expect(page.getByRole('heading', { level: 1, name: 'Walk-forward evaluation' })).toBeVisible();
  // It is part of Research, not another dashboard.
  await expect(page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Research' })).toHaveAttribute(
    'aria-current',
    'page',
  );
  const collections = page.getByRole('region', { name: 'Recording collections' });
  await expect(collections.getByRole('cell', { name: /^synthetic-2026-09-23/ })).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('walk-forward-home.png'), fullPage: true });

  // Keyboard only: Preview the plan of exactly this request.
  const form = page.getByRole('region', { name: 'Plan a walk-forward study' });
  await form.getByRole('button', { name: 'Preview plan' }).focus();
  await page.keyboard.press('Enter');
  await expect(form.getByText('The plan is ready to review')).toBeVisible();
  await form.getByRole('link', { name: 'Review the plan' }).click();
  expect(page.url()).toContain(`#/research/walk-forward/plans/${PLAN_ID}`);
  await expect(page.getByRole('heading', { level: 1, name: 'Review the plan' })).toBeVisible();
  const folds = page.getByRole('table', { name: /training recordings, cutoff, and evaluation recording/ });
  await expect(folds.getByRole('row')).toHaveCount(3);
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('walk-forward-plan.png'), fullPage: true });

  // The reviewed plan survives a reload unchanged.
  await page.reload();
  await expect(page.getByRole('table', { name: 'The frozen fee and latency scenarios' })).toBeVisible();

  const start = page.getByRole('region', { name: 'Start the study' });
  await start.getByRole('button', { name: 'Start study' }).click();
  await expect(start.getByText(/Fold 1 of 2: fitting fresh models|Fold 2 of 2: running the engine/)).toBeVisible();
  await expect(start.getByText('Study published, partial')).toBeVisible();
  await expect(start.getByRole('link', { name: 'Open the study' })).toHaveAttribute(
    'href',
    `#/research/walk-forward/studies/${PARTIAL_STUDY_ID}`,
  );
  await page.getByRole('link', { name: '← All walk-forward studies' }).click();
  await expect(page.getByRole('region', { name: 'Study jobs' }).getByText('Succeeded', { exact: true })).toBeVisible();
  await expect(page.getByRole('region', { name: 'Published studies' }).getByRole('link').first()).toBeVisible();
});

test('opens a partial study by its link, reloads it, and returns to the same account', async ({ page }, testInfo) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await openAccount(page, 'e2e-walk-forward-1');
  await page.goto(`/#/research/walk-forward/studies/${PARTIAL_STUDY_ID}`);

  await expect(page.getByText('This study is partial')).toBeVisible();
  await expect(page.getByText('Every recording here was generated for tests.', { exact: false })).toBeVisible();
  const trading = page.getByRole('table', { name: /one row per engine run/ });
  await expect(trading.getByRole('row')).toHaveCount(9);
  await expect(page.getByRole('table', { name: /summed over its folds' independent accounts/ }).getByText('Complete total unavailable')).toHaveCount(2);
  await expect(page.getByRole('region', { name: 'Prediction quality' })).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('walk-forward-study.png'), fullPage: true });

  await page.reload();
  await expect(page.getByText('This study is partial')).toBeVisible();
  await page.getByRole('link', { name: '← All walk-forward studies' }).click();
  await expect(page.getByRole('region', { name: 'Recording collections' })).toBeVisible();
  await page.getByRole('link', { name: 'Back to trading' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
  await expect(page.getByRole('banner').getByText('e2e-walk-forward-1')).toBeVisible();
});

test('shows a run that no longer verifies on a freshly loaded study, on every width', async ({ page }, testInfo) => {
  await mockChronosApi(page);
  await mockResearchService(page, { corruptStudyRun: true });
  await page.goto(`/#/research/walk-forward/studies/${PARTIAL_STUDY_ID}`);
  const warning = page.getByText('Some of this study’s artifacts do not verify');
  await expect(warning).toBeVisible();
  await expect(page.getByText(/Fold 2, Logistic regression, Base scenario run: journal\.ndjson differs/)).toBeVisible();
  const trading = page.getByRole('table', { name: /one row per engine run/ });
  await expect(trading.getByText('Completed, unverified')).toBeVisible();
  await expect(trading.getByText('Does not verify')).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('walk-forward-unverified-run.png'), fullPage: true });
});

test('explains why one recording cannot start a study and what is needed', async ({ page }, testInfo) => {
  await mockChronosApi(page);
  await mockResearchService(page, { oneRecording: true });
  await page.goto('/#/research/walk-forward');
  const form = page.getByRole('region', { name: 'Plan a walk-forward study' });
  await expect(form.getByText('Not enough independent recordings for a walk-forward study')).toBeVisible();
  await expect(form.getByText(/needs at least 2 independent recordings of AAPL/)).toBeVisible();
  await expect(form.getByRole('button', { name: 'Preview plan' })).toBeDisabled();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await page.screenshot({ path: testInfo.outputPath('walk-forward-insufficient.png'), fullPage: true });
});

test('explains an unreachable research service on the walk-forward page', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { unreachable: true });
  await page.goto('/#/research/walk-forward');
  await expect(page.getByText('The research service is not reachable')).toBeVisible();
  await expectNoAccessibilityViolations(page);
});
