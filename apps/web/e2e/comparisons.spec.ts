import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { COMPARISON_ID } from '../src/test/comparisonFixtures';
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

test('compares two versions after review, follows the job, and opens the comparison', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { seededLibrary: true });
  await page.goto('/');
  await page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Comparisons' }).click();
  await expect(page.getByRole('heading', { level: 1, name: 'Model comparisons' })).toBeVisible();
  await expect(page.getByText('No comparisons are published yet.')).toBeVisible();

  const form = page.getByRole('region', { name: 'Compare model versions' });
  // Keyboard only: choose both members, review, and start.
  await form.getByRole('checkbox', { name: /aapl-logistic-20260925 1/ }).focus();
  await page.keyboard.press('Space');
  await form.getByRole('checkbox', { name: /aapl-logistic-onnx 1/ }).focus();
  await page.keyboard.press('Space');
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await form.getByRole('button', { name: 'Review comparison' }).focus();
  await page.keyboard.press('Enter');
  await expect(form.getByText(/No member is retrained/)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await form.getByRole('button', { name: 'Start comparison' }).focus();
  await page.keyboard.press('Enter');
  await expect(form.getByText('Comparison published')).toBeVisible();
  await form.getByRole('link', { name: 'Open the comparison' }).click();

  expect(page.url()).toContain(`#/comparisons/${COMPARISON_ID}`);
  const table = page.getByRole('table', { name: /strategy results/ });
  await expect(table.getByRole('columnheader', { name: /spy-logistic 1/ })).toBeVisible();
  const quality = page.getByRole('table', { name: /classification quality against realized labels/ });
  await expect(quality.getByRole('row', { name: /^Rows scored/ }).getByText('79 of 80')).toHaveCount(2);
  await expect(page.getByText('Some results rest on declared cutoffs')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Benchmarks' }).getByText('Identical for every member')).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);

  // A reload opens the same comparison.
  await page.reload();
  await expect(page.getByRole('table', { name: /strategy results/ })).toBeVisible();
});
