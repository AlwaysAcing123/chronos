import AxeBuilder from '@axe-core/playwright';
import type { Page, TestInfo } from '@playwright/test';
import { expect, test } from '@playwright/test';

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

// Screenshots for review, only when a directory is given.
async function capture(page: Page, testInfo: TestInfo, name: string) {
  const directory = process.env.CHRONOS_SCREENSHOT_DIR;
  if (directory !== undefined) {
    await page.screenshot({ path: `${directory}/${testInfo.project.name}-${name}.png`, fullPage: true });
  }
}

test('deploys a model to its own paper account, starts it only when confirmed, stops it, and opens its account', async ({
  page,
}, testInfo) => {
  await mockChronosApi(page, { deployments: true });
  await mockResearchService(page);
  await page.goto('/#/research');

  const form = page.getByRole('region', { name: 'Deploy to paper' });
  await expect(form).toBeVisible();
  await form.getByLabel('Deployment ID').fill('spy-logistic');
  await form.getByLabel('New paper account ID').fill('model-spy');
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await capture(page, testInfo, '1-deploy-form');

  // The review freezes the request; keyboard users reach every control.
  await form.getByRole('button', { name: 'Review deployment' }).focus();
  await page.keyboard.press('Enter');
  const review = form.getByLabel('Review the deployment');
  await expect(review.getByText('spy-logistic')).toBeVisible();
  await expect(review.getByText(/Nothing trades until you press Start/)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await capture(page, testInfo, '2-review');
  await form.getByRole('button', { name: 'Create deployment' }).click();
  await expect(form.getByText('Deployment created, stopped')).toBeVisible();

  const card = page.getByRole('article', { name: 'Deployment spy-logistic' });
  await expect(card.getByText('Created; not started yet')).toBeVisible();
  const start = card.getByRole('button', { name: 'Start' });
  await expect(start).toBeDisabled();
  await card.getByLabel('Let this model place simulated orders in model-spy').check();
  await start.click();
  await expect(card.getByText('Running', { exact: true })).toBeVisible();
  await expect(card.getByRole('cell', { name: /^Buy 1/ })).toBeVisible();
  await expect(card.getByText('Its latest prediction led to an order; it waits for the next quote.')).toBeVisible();
  await expect(card.getByRole('cell', { name: /Filled 1 @ 768\.94/ })).toBeVisible();
  // The counts open from the keyboard and say they are cumulative.
  await card.getByText('Counts by reason (run 1, this server process)').focus();
  await page.keyboard.press('Enter');
  await expect(card.getByText(/not what is happening now/)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await capture(page, testInfo, '3-running');

  // The kill switch is armed before it acts.
  const panel = page.getByRole('region', { name: 'Paper deployments' });
  await panel.getByRole('button', { name: 'Stop all deployments…' }).click();
  await expect(panel.getByRole('button', { name: 'Stop every deployment now' })).toBeVisible();
  await panel.getByRole('button', { name: 'Cancel' }).click();

  await card.getByRole('button', { name: 'Stop' }).click();
  await expect(card.getByText('Stopped on request')).toBeVisible();
  await expect(card.getByText('Not trading. It trades again only if started.')).toBeVisible();

  // A reload finds it as it was.
  await page.reload();
  await expect(page.getByRole('article', { name: 'Deployment spy-logistic' }).getByText('Stopped on request')).toBeVisible();

  // Its account opens in trading, with no manual order ticket.
  await page
    .getByRole('article', { name: 'Deployment spy-logistic' })
    .getByRole('button', { name: 'Open model-spy in trading' })
    .click();
  await expect(page.getByText('Traded by model deployment spy-logistic')).toBeVisible();
  await expect(page.getByRole('button', { name: /Review order/ })).toHaveCount(0);
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
  await capture(page, testInfo, '4-owned-account');

  // Back to manual trading: another account has its ticket again.
  await page.getByRole('button', { name: 'Switch account' }).click();
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill('manual-after-model');
  await create.getByRole('button', { name: 'Create paper account' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
  await expect(page.getByText(/Traded by model deployment/)).toHaveCount(0);
});

test('says when the server has deployments disabled', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await page.goto('/#/research');
  await expect(page.getByText('Model deployments are disabled on this server')).toBeVisible();
  await expectNoAccessibilityViolations(page);
});
