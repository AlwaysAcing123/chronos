import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { mockChronosApi } from './mockApi';

async function expectNoAccessibilityViolations(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
}

// A good-til-cancelled buy rests, a later quote partially fills it, and its
// remainder is cancelled: every view shows the server's state at each step,
// and a reload restores it.
test('works a GTC order through a partial fill and a cancellation', async ({ page }) => {
  const api = await mockChronosApi(page);
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill('e2e-gtc-1');
  await create.getByRole('button', { name: 'Create paper account' }).click();

  const ticket = page.getByRole('region', { name: 'Order ticket' });
  const working = page.getByRole('region', { name: 'Working orders' });
  const portfolio = page.getByRole('region', { name: 'Portfolio' });
  await expect(working.getByText(/No working orders/)).toBeVisible();

  // A limit buy at 336, below the 337 ask, as good til cancelled.
  await ticket.getByRole('radio', { name: 'Limit' }).check();
  const timeInForce = ticket.getByRole('group', { name: 'Time in force' });
  await timeInForce.getByRole('radio', { name: 'Good til cancelled (GTC)' }).check();
  await ticket.getByLabel('Quantity (AAPL)').fill('10');
  await ticket.getByLabel('Limit price').fill('336');
  await ticket.getByRole('button', { name: 'Review order' }).click();
  const review = ticket.getByRole('region', { name: 'Review paper order' });
  await expect(review.getByText('Good til cancelled (GTC)')).toBeVisible();
  await expect(review.getByText(/rests as a working order/)).toBeVisible();
  await review.getByRole('button', { name: 'Submit paper order' }).click();

  await expect(ticket.getByText('Working: 10 rests at limit 336')).toBeVisible();
  const row = working.getByRole('row', { name: /336/ });
  await expect(row).toContainText('Working');
  await expect(row).toContainText('3360.35');
  await expect(portfolio.getByText('96639.65')).toBeVisible();
  await expectNoAccessibilityViolations(page);

  // A later ask of 336 × 4 fills part of it; the change stream or the poll
  // refreshes every view.
  api.fillWorkingOrder('e2e-gtc-1');
  await expect(row).toContainText('Working, partially filled', { timeout: 10_000 });
  await expect(row).toContainText('2016');
  await expect(page.getByRole('region', { name: 'Fills' }).getByText('1344')).toBeVisible();
  await expect(portfolio.getByText('99998.93')).toBeVisible();
  const positions = portfolio.getByRole('region', { name: 'Open positions' });
  await expect(positions.getByRole('row', { name: /^AAPL/ })).toContainText('1343.28');

  // Cancel the remainder: the fill and fee stand, the reservation is released.
  await working.getByRole('button', { name: /^Cancel remainder of / }).click();
  await expect(working.getByText(/: remainder cancelled$/)).toBeVisible();
  await expect(working.getByText('4 of 10 filled; 6 cancelled and released.')).toBeVisible();
  await expect(working.getByText(/No working orders/)).toBeVisible();
  await expect(portfolio.getByText('98655.65').first()).toBeVisible();
  const ledger = portfolio.getByText('Reserved by working orders', { exact: true });
  await expect(ledger.locator('xpath=following-sibling::dd[1]')).toHaveText('0');

  const orders = page.getByRole('region', { name: 'Orders', exact: true });
  await expect(orders.getByRole('cell', { name: 'Partially filled, rest cancelled' })).toBeVisible();
  await orders.getByRole('button', { name: /^Lifecycle of / }).click();
  const events = orders.getByRole('list', { name: /^Events of / });
  await expect(events.getByText('Accepted as a working order')).toBeVisible();
  await expect(events.getByText(/4 at 336 · notional 1344 · fee 0.35/)).toBeVisible();
  await expect(events.getByText('Remainder cancelled')).toBeVisible();
  await expectNoAccessibilityViolations(page);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  expect(overflow).toBeLessThanOrEqual(0);

  // Everything is the server's, so a reload shows the same state.
  await page.reload();
  const reloaded = page.getByRole('region', { name: 'Portfolio' });
  await expect(reloaded.getByText('99998.93')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Working orders' }).getByText(/No working orders/)).toBeVisible();
  await expect(page.getByRole('region', { name: 'Orders', exact: true }).getByRole('cell', { name: 'Partially filled, rest cancelled' })).toBeVisible();
  expect(api.submissionCount()).toBe(1);
});

test('forces a market order to immediate-or-cancel', async ({ page }) => {
  await mockChronosApi(page);
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill('e2e-gtc-market');
  await create.getByRole('button', { name: 'Create paper account' }).click();
  const ticket = page.getByRole('region', { name: 'Order ticket' });
  await expect(ticket.getByText('Immediate or cancel (IOC). A market order cannot rest.')).toBeVisible();
  await ticket.getByRole('radio', { name: 'Limit' }).check();
  await ticket.getByRole('radio', { name: 'Good til cancelled (GTC)' }).check();
  await ticket.getByRole('radio', { name: 'Market', exact: true }).check();
  await expect(ticket.getByRole('group', { name: 'Time in force' })).toHaveCount(0);
  await ticket.getByLabel('Quantity (AAPL)').fill('10');
  await ticket.getByRole('button', { name: 'Review order' }).click();
  await expect(ticket.getByRole('region', { name: 'Review paper order' }).getByText('Immediate or cancel (IOC)')).toBeVisible();
});
