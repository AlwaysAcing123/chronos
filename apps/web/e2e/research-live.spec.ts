// Opt-in acceptance run against the real research service through the
// preview proxy:
//   CHRONOS_WEB_LIVE_RESEARCH=1 CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:8090 \
//     npx playwright test research-live.spec.ts
// It needs `python -m chronos_research serve` over artifact roots holding a
// verified experiment with a fitted AAPL logistic model, such as the saved
// demonstration's. It never calls the trading API.
import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';

test.skip(process.env.CHRONOS_WEB_LIVE_RESEARCH !== '1', 'set CHRONOS_WEB_LIVE_RESEARCH=1 to run against the real service');

test('runs a real evaluation and shows its verified result', async ({ page }, testInfo) => {
  test.setTimeout(300_000);
  await page.goto('/#/research');
  const model = page.getByRole('region', { name: 'Model' });
  await model.getByRole('radio', { name: /AAPL/ }).check();
  const form = page.getByRole('region', { name: 'Evaluation' });
  await form.getByRole('radio', { name: /^Test/ }).check();
  await form.getByRole('button', { name: 'Review evaluation' }).click();
  await page.screenshot({ path: testInfo.outputPath('research-review.png'), fullPage: true });
  await form.getByRole('button', { name: 'Start evaluation' }).click();
  await expect(form.getByText(/Evaluation queued|This evaluation already exists/)).toBeVisible();

  const runs = page.getByRole('region', { name: 'Runs' });
  const row = runs.getByRole('row', { name: /AAPL · Test/ }).first();
  await expect(row.getByText('Succeeded', { exact: true })).toBeVisible({ timeout: 240_000 });
  await page.screenshot({ path: testInfo.outputPath('research-home.png'), fullPage: true });
  await row.getByRole('link', { name: 'Open result' }).click();

  const outcome = page.getByRole('region', { name: 'Outcome' });
  await expect(outcome.getByRole('row', { name: /^Final equity/ }).getByRole('cell').nth(2)).toHaveText('10000');
  await expect(page.getByRole('slider', { name: /Equity over the window/ })).toBeVisible();
  await expect(page.getByRole('region', { name: 'Trades' }).getByRole('row').nth(1)).toBeVisible();
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => violation.id)).toEqual([]);
  await page.screenshot({ path: testInfo.outputPath('research-result.png'), fullPage: true });

  await page.reload();
  await expect(page.getByRole('region', { name: 'Outcome' })).toBeVisible();
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  expect(overflow).toBeLessThanOrEqual(0);
});
