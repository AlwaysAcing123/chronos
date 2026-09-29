// Opt-in acceptance run against the real C++ API through the preview proxy:
//   CHRONOS_WEB_LIVE_API=1 npx playwright test live.spec.ts --project=desktop
// It needs the server on 127.0.0.1:8080 replaying a recording that has an
// AAPL quote, and it creates uniquely named accounts in that database.
import { expect, test } from '@playwright/test';

test.skip(process.env.CHRONOS_WEB_LIVE_API !== '1', 'set CHRONOS_WEB_LIVE_API=1 to run against the real API');

test('trades against the real server and restores the account after reload', async ({ page, request }, testInfo) => {
  // Projects run in parallel, so the ID names the project as well as the time.
  const accountId = `web-live-${testInfo.project.name}-${Date.now().toString(36)}`;
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill(accountId);
  await create.getByRole('button', { name: 'Create paper account' }).click();

  const ticket = page.getByRole('region', { name: 'Order ticket' });
  await expect(ticket.getByText('Best bid')).toBeVisible();
  await ticket.getByLabel('Quantity (AAPL)').fill('1');
  await ticket.getByRole('button', { name: 'Review order' }).click();
  await ticket.getByRole('button', { name: 'Submit paper order' }).click();
  await expect(ticket.getByText('Recorded in the paper journal.', { exact: false })).toBeVisible();

  // A model-attributed order through the same API path.
  const model = await request.post(`/api/v1/accounts/${accountId}/orders`, {
    data: {
      client_order_id: 'model-limit-1',
      source: { kind: 'model', id: 'e2e-model' },
      instrument: 'AAPL',
      side: 'buy',
      type: 'limit',
      quantity: '1',
      limit_price: '1',
      time_in_force: 'ioc',
    },
  });
  expect(model.status()).toBe(201);

  const orders = page.getByRole('region', { name: 'Orders' });
  await expect(orders.getByText('model-limit-1')).toBeVisible();
  await expect(orders.getByText('Model', { exact: true })).toBeVisible();
  await expect(orders.getByText('Manual', { exact: true })).toBeVisible();

  await page.reload();
  await expect(page.getByRole('banner').getByText(accountId)).toBeVisible();
  await expect(page.getByRole('region', { name: 'Orders' }).getByText('model-limit-1')).toBeVisible();
});
