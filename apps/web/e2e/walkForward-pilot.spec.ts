// Opt-in acceptance of real-recording pilot results against a real, isolated
// research service, by explicit IDs, so it never depends on how many
// collections or studies the served root holds:
//   CHRONOS_WEB_PILOT_STUDIES=<study-id>,<study-id>   studies to read and reload
//   CHRONOS_WEB_OBSERVED_COLLECTION=<collection-id>   a one-recording observed collection
//   CHRONOS_WEB_CORRUPT_STUDY=<study-id>              a study with one run corrupted
//   CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:18190 npx playwright test walkForward-pilot.spec.ts --workers=1
// The trading API is answered inside the browser, so nothing reaches a
// trading server, and nothing here can acquire data.
import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { mockChronosApi } from './mockApi';

const studies = (process.env.CHRONOS_WEB_PILOT_STUDIES ?? '').split(',').filter((id) => id !== '');
const observedCollection = process.env.CHRONOS_WEB_OBSERVED_COLLECTION ?? '';
const corruptStudy = process.env.CHRONOS_WEB_CORRUPT_STUDY ?? '';

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

test('reads each pilot study with its unavailable values and reasons, reloads it, and returns to the same account', async ({
  page,
}, testInfo) => {
  test.skip(studies.length === 0, 'set CHRONOS_WEB_PILOT_STUDIES to the pilot study IDs');
  test.setTimeout(180_000);
  await mockChronosApi(page);
  await openAccount(page, 'e2e-pilot-1');
  for (const studyId of studies) {
    await page.goto(`/#/research/walk-forward/studies/${studyId}`);
    await expect(page.getByText('This study is partial')).toBeVisible({ timeout: 60_000 });
    await expect(page.getByText('Every recording here was generated for tests.', { exact: false })).toBeVisible();
    // Five folds, two models, three scenarios: every cell is a row, run or not.
    const trading = page.getByRole('table', { name: /one row per engine run/ });
    await expect(trading.getByRole('row')).toHaveCount(31);
    // Fold 1's logistic cells were not run, and say why: the model's own reason.
    await expect(trading.getByText('Not run').first()).toBeVisible();
    await expect(trading.getByText('Too few training rows of some class to fit')).toHaveCount(3);
    const totals = page.getByRole('table', { name: /summed over its folds' independent accounts/ });
    await expect(totals.getByText('Complete total unavailable')).toHaveCount(3);
    for (const row of await page.getByRole('table', { name: /whether its artifacts verify/ }).getByRole('row').all()) {
      await expect(row.getByText('Does not verify')).toHaveCount(0);
      await expect(row.getByText('Not checked')).toHaveCount(0);
    }
    await expect(page.getByText('Some of this study’s artifacts do not verify')).toHaveCount(0);
    await expectAccessibleAndContained(page);
    await page.screenshot({ path: testInfo.outputPath(`pilot-study-${studyId.slice(0, 8)}.png`), fullPage: true });
    await page.reload();
    await expect(page.getByRole('table', { name: /one row per engine run/ }).getByRole('row')).toHaveCount(31);
  }
  await page.getByRole('link', { name: '← All walk-forward studies' }).click();
  await page.getByRole('link', { name: 'Back to trading' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
  await expect(page.getByRole('banner').getByText('e2e-pilot-1')).toBeVisible();
});

test('a one-recording observed collection is labelled and cannot be planned', async ({ page }, testInfo) => {
  test.skip(observedCollection === '', 'set CHRONOS_WEB_OBSERVED_COLLECTION to an observed collection ID');
  await mockChronosApi(page);
  await page.goto('/#/research/walk-forward');
  const form = page.getByRole('region', { name: 'Plan a walk-forward study' });
  await form.getByLabel('Collection').selectOption(observedCollection);
  await expect(form.getByLabel('Collection').locator('option:checked')).toContainText('Observed provider data');
  await expect(form.getByText('Not enough independent recordings for a walk-forward study')).toBeVisible();
  await expect(form.getByRole('button', { name: 'Preview plan' })).toBeDisabled();
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('observed-collection.png'), fullPage: true });
});

test('a pilot study whose run no longer verifies says so on a fresh load', async ({ page }, testInfo) => {
  test.skip(corruptStudy === '', 'set CHRONOS_WEB_CORRUPT_STUDY to a study with a corrupted run');
  await mockChronosApi(page);
  await page.goto(`/#/research/walk-forward/studies/${corruptStudy}`);
  await expect(page.getByText('Some of this study’s artifacts do not verify')).toBeVisible({ timeout: 60_000 });
  await expect(page.getByText('Completed, unverified').first()).toBeVisible();
  await expect(page.getByText("journal.ndjson differs from its manifest's checksum").first()).toBeVisible();
  await expectAccessibleAndContained(page);
  await page.screenshot({ path: testInfo.outputPath('pilot-corrupt-run.png'), fullPage: true });
});
