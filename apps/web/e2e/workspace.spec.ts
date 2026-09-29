import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { mockChronosApi } from './mockApi';

async function expectNoAccessibilityViolations(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
}

async function createAccount(page: Page, accountId: string) {
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await expect(create.getByRole('combobox', { name: 'Instrument' })).toHaveValue('AAPL');
  await create.getByLabel('Account ID').fill(accountId);
  await create.getByRole('button', { name: 'Create paper account' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
}

test('is unmistakably paper trading and accessible before an account is chosen', async ({ page }) => {
  await mockChronosApi(page);
  await page.goto('/');
  await expect(page.getByRole('note', { name: 'Paper trading notice' })).toContainText('No real money');
  await expect(page.getByText('API online')).toBeVisible();
  await expectNoAccessibilityViolations(page);
});

test('creates an account, trades with the keyboard, and restores the account after reload', async ({ page }) => {
  const api = await mockChronosApi(page);
  await createAccount(page, 'e2e-desk-1');
  const ticket = page.getByRole('region', { name: 'Order ticket' });
  await expect(page.getByRole('region', { name: 'Portfolio' }).getByText('100000').first()).toBeVisible();

  await ticket.getByLabel('Quantity (AAPL)').fill('10');
  await ticket.getByLabel('Quantity (AAPL)').press('Enter');
  await expect(ticket.getByRole('button', { name: 'Submit paper order' })).toBeFocused();
  await page.keyboard.press('Enter');

  await expect(ticket.getByText('Partially filled 4 of 10 at 337; 6 cancelled')).toBeVisible();
  const orders = page.getByRole('region', { name: 'Orders' });
  await expect(orders.getByRole('cell', { name: 'Partially filled' })).toBeVisible();
  await expect(page.getByRole('region', { name: 'Fills' }).getByText('1348')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Portfolio' }).getByText('99994.93')).toBeVisible();
  expect(api.submissionCount()).toBe(1);
  await expectNoAccessibilityViolations(page);

  await page.reload();
  await expect(page.getByRole('banner').getByText('e2e-desk-1')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Portfolio' }).getByText('99994.93')).toBeVisible();
});

test('recovers the outcome of a submission whose response was lost', async ({ page }) => {
  const api = await mockChronosApi(page, { loseFirstSubmissionResponse: true });
  await createAccount(page, 'e2e-lost-1');
  const ticket = page.getByRole('region', { name: 'Order ticket' });
  await ticket.getByLabel('Quantity (AAPL)').fill('10');
  await ticket.getByRole('button', { name: 'Review order' }).click();
  await ticket.getByRole('button', { name: 'Submit paper order' }).click();

  await expect(ticket.getByText('Partially filled 4 of 10 at 337; 6 cancelled')).toBeVisible();
  await expect(ticket.getByText(/confirmed by looking up the client order ID/)).toBeVisible();
  expect(api.submissionCount()).toBe(1);
});

test('lays out without horizontal scrolling', async ({ page }) => {
  await mockChronosApi(page);
  await createAccount(page, 'e2e-layout-1');
  await expect(page.getByRole('region', { name: 'Portfolio' }).getByText('Equity')).toBeVisible();
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  expect(overflow).toBeLessThanOrEqual(0);
});

test('offers section navigation only where panels stack', async ({ page, isMobile }) => {
  await mockChronosApi(page);
  await createAccount(page, 'e2e-nav-1');
  const sections = page.getByRole('navigation', { name: 'Workspace sections' });
  if (isMobile) {
    await expect(sections).toBeVisible();
    await sections.getByRole('link', { name: 'Orders' }).click();
    await expect(page.getByRole('region', { name: 'Orders' })).toBeInViewport();
  } else {
    await expect(sections).toBeHidden();
  }
});

test('removes motion when the user prefers reduced motion', async ({ page }) => {
  await mockChronosApi(page);
  const pulse = async () =>
    page.locator('[data-refreshing]').evaluate((element) => {
      element.setAttribute('data-refreshing', 'true');
      return getComputedStyle(element).animationName;
    });
  await page.goto('/');
  expect(await pulse()).not.toBe('none');

  await page.emulateMedia({ reducedMotion: 'reduce' });
  expect(await pulse()).toBe('none');
  await page.keyboard.press('Tab');
  // The skip link appears at once instead of sliding in.
  await expect(page.getByRole('link', { name: 'Skip to content' })).toBeInViewport();
});
