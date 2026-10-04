import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { RUN_ID } from '../src/test/researchFixtures';
import { mockChronosApi } from './mockApi';
import { mockResearchService } from './researchMock';

async function expectNoAccessibilityViolations(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
}

// Stricter than comparing with innerWidth: a phone's layout viewport grows to
// fit content that is too wide, but the client width does not.
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

test('runs an evaluation, opens and reloads its result, and returns to the same account', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await openAccount(page, 'e2e-research-1');

  await page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Research' }).click();
  await expect(page.getByRole('heading', { name: 'Historical evaluation' })).toBeVisible();
  await expect(page.getByRole('note', { name: 'Paper trading notice' })).toBeVisible();
  const model = page.getByRole('region', { name: 'Model' });
  await expect(model.getByRole('radio', { name: /SPY/ })).toBeChecked();
  await expect(model.getByText('Logistic unevaluable: insufficient class support')).toBeVisible();
  await expect(model.getByText(/Accuracy is not profit/)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);

  const form = page.getByRole('region', { name: 'Evaluation' });
  await form.getByRole('button', { name: 'Review evaluation' }).click();
  await expect(form.getByText(/never short/)).toBeVisible();
  await form.getByRole('button', { name: 'Start evaluation' }).click();
  await expect(form.getByText('Evaluation queued')).toBeVisible();

  const runs = page.getByRole('region', { name: 'Runs' });
  await expect(runs.getByText('Succeeded', { exact: true })).toBeVisible();
  await expect(page.getByRole('region', { name: 'Saved results' }).getByText('0.13')).toBeVisible();
  await runs.getByRole('link', { name: 'Open result' }).click();

  const outcome = page.getByRole('region', { name: 'Outcome' });
  const equity = outcome.getByRole('row', { name: /^Final equity/ });
  await expect(equity.getByRole('cell').nth(0)).toHaveText('10000.13');
  await expect(equity.getByRole('cell').nth(1)).toHaveText('Unavailable: stale market state');
  await expect(equity.getByRole('cell').nth(2)).toHaveText('10000');

  const chart = page.getByRole('slider', { name: /Equity over the window/ });
  await chart.focus();
  await page.keyboard.press('ArrowLeft');
  await expect(chart).toHaveAttribute('aria-valuenow', '3');
  await expect(page.getByText(/Point 3 of 4/)).toBeVisible();
  await page.getByText('Equity at every valuation point (table)').click();
  await expect(page.getByRole('table', { name: 'Equity at every valuation point' }).getByRole('row')).toHaveCount(5);

  const trades = page.getByRole('region', { name: 'Trades' });
  await trades.getByRole('button', { name: 'Load more' }).click();
  await expect(trades.getByText('Expired at window end')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Assumptions and provenance' }).getByText(RUN_ID)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);

  // A reload rediscovers the saved result.
  await page.reload();
  await expect(page.getByRole('region', { name: 'Outcome' })).toBeVisible();
  expect(page.url()).toContain(`#/research/results/${RUN_ID}`);

  await page.getByRole('link', { name: 'Back to trading' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
  await expect(page.getByRole('banner').getByText('e2e-research-1')).toBeVisible();
});

test('starts an evaluation with the keyboard alone', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await page.goto('/#/research');
  const form = page.getByRole('region', { name: 'Evaluation' });
  const fee = form.getByLabel('Fee per fill (USD)');
  await fee.focus();
  await page.keyboard.press('Enter');
  await expect(form.getByText(/never short/)).toBeVisible();
  const start = form.getByRole('button', { name: 'Start evaluation' });
  await start.focus();
  await expect(start).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(form.getByText('Evaluation queued')).toBeVisible();
});

test('explains an unreachable research service and leaves trading usable', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { unreachable: true });
  await page.goto('/#/research');
  await expect(page.getByText('The research service is not reachable')).toBeVisible();
  await expect(page.getByText(/python -m chronos_research serve/)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Trading' }).click();
  await expect(page.getByRole('region', { name: 'Create a paper account' })).toBeVisible();
});

test('reports a failed evaluation, a corrupt result, and an empty catalog as they are', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { failJob: true, corruptResult: true });
  await page.goto('/#/research');
  const form = page.getByRole('region', { name: 'Evaluation' });
  await form.getByRole('button', { name: 'Review evaluation' }).click();
  await form.getByRole('button', { name: 'Start evaluation' }).click();
  const runs = page.getByRole('region', { name: 'Runs' });
  await expect(runs.getByText('Failed', { exact: true })).toBeVisible();
  await expect(runs.getByText('the split artifact was refused')).toBeVisible();

  await page.goto(`/#/research/results/${RUN_ID}`);
  await expect(page.getByText('This result cannot be shown')).toBeVisible();
  await expect(page.getByText(/differs from its manifest/)).toBeVisible();
});

test('says so when no experiment has been published', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { emptyCatalog: true });
  await page.goto('/#/research');
  await expect(page.getByText('No baseline experiments yet.')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Evaluation' }).getByText(/Choose a model/)).toBeVisible();
  await expectNoHorizontalOverflow(page);
});
