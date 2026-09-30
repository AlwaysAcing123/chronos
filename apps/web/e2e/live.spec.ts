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

  // The real server keeps the change stream open.
  await expect(page.getByText(/^Streaming changes/)).toBeVisible();
  // A paced replay waits in Ready until someone starts it.
  const start = page.getByRole('region', { name: 'Market-data source' }).getByRole('button', { name: 'Start replay' });
  if (await start.isVisible()) {
    await start.click();
  }
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

  const orders = page.getByRole('region', { name: 'Orders', exact: true });
  await expect(orders.getByText('model-limit-1')).toBeVisible();
  await expect(orders.getByText('Model', { exact: true })).toBeVisible();
  await expect(orders.getByText('Manual', { exact: true })).toBeVisible();

  await page.reload();
  await expect(page.getByRole('banner').getByText(accountId)).toBeVisible();
  await expect(page.getByRole('region', { name: 'Orders', exact: true }).getByText('model-limit-1')).toBeVisible();
});

// A good-til-cancelled buy far below the market rests on the real server,
// reserves cash there, and is cancelled through the working-orders view.
test('rests and cancels a GTC order on the real server', async ({ page }, testInfo) => {
  const accountId = `web-gtc-${testInfo.project.name}-${Date.now().toString(36)}`;
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill(accountId);
  await create.getByRole('button', { name: 'Create paper account' }).click();
  const start = page.getByRole('region', { name: 'Market-data source' }).getByRole('button', { name: 'Start replay' });
  if (await start.isVisible()) {
    await start.click();
  }
  const ticket = page.getByRole('region', { name: 'Order ticket' });
  await expect(ticket.getByText('Best bid')).toBeVisible();
  await ticket.getByRole('radio', { name: 'Limit' }).check();
  await ticket.getByRole('radio', { name: 'Good til cancelled (GTC)' }).check();
  await ticket.getByLabel('Quantity (AAPL)').fill('2');
  await ticket.getByLabel('Limit price').fill('1');
  await ticket.getByRole('button', { name: 'Review order' }).click();
  await ticket.getByRole('button', { name: 'Submit paper order' }).click();
  await expect(ticket.getByText('Working: 2 rests at limit 1')).toBeVisible();

  const working = page.getByRole('region', { name: 'Working orders' });
  // 1 × 2 plus the account's fee until the first fill, as the server says.
  await expect(working.getByRole('row', { name: /Working/ })).toBeVisible();
  await working.getByRole('button', { name: /^Cancel remainder of / }).click();
  await expect(working.getByText(/: remainder cancelled$/)).toBeVisible();
  await expect(working.getByText(/No working orders/)).toBeVisible();

  await page.reload();
  await expect(page.getByRole('region', { name: 'Orders', exact: true }).getByRole('cell', { name: 'Cancelled', exact: true })).toBeVisible();
  await expect(page.getByRole('region', { name: 'Working orders' }).getByText(/No working orders/)).toBeVisible();
});
