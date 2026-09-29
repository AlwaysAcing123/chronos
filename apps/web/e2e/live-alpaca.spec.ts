// Opt-in acceptance run against the real C++ API in Alpaca IEX mode:
//   CHRONOS_WEB_LIVE_ALPACA=1 npx playwright test live-alpaca.spec.ts --project=desktop
// It needs the server on 127.0.0.1:8080 started with --alpaca-iex for AAPL,
// and creates uniquely named accounts in that database. The browser never
// sees Alpaca credentials; the server holds them.
import { expect, test } from '@playwright/test';

test.skip(process.env.CHRONOS_WEB_LIVE_ALPACA !== '1', 'set CHRONOS_WEB_LIVE_ALPACA=1 to run against a live Alpaca server');

test('labels live IEX data and lets only Chronos process orders', async ({ page, request }, testInfo) => {
  const accountId = `web-alpaca-${testInfo.project.name}-${Date.now().toString(36)}`;
  await page.goto('/');
  const bar = page.getByRole('region', { name: 'Market-data source' });
  await expect(bar.getByText('Live data')).toBeVisible();
  await expect(bar.getByText('Alpaca IEX')).toBeVisible();
  await expect(bar.getByText('IEX only, not SIP or NBBO')).toBeVisible();
  await expect(bar.getByRole('button')).toHaveCount(0);
  await expect(page.getByText('Alpaca IEX is one exchange, not the whole market')).toBeVisible();
  await expect(page.getByText(/^Streaming changes/)).toBeVisible();

  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill(accountId);
  await create.getByRole('button', { name: 'Create paper account' }).click();
  const ticket = page.getByRole('region', { name: 'Order ticket' });
  await ticket.getByLabel('Quantity (AAPL)').fill('1');
  await ticket.getByRole('button', { name: 'Review order' }).click();
  await ticket.getByRole('button', { name: 'Submit paper order' }).click();

  const market = (await (await request.get('/api/v1/market-data')).json()) as {
    market_data: { instruments: { instrument: string; execution_quote_available: boolean }[] };
  };
  const aapl = market.market_data.instruments.find((item) => item.instrument === 'AAPL');
  if (aapl?.execution_quote_available === true) {
    // A current IEX quote: Chronos simulates and records the order itself.
    await expect(ticket.getByText('Recorded in the paper journal.', { exact: false })).toBeVisible();
  } else {
    // No current quote (market closed or reconnecting): nothing is recorded.
    await expect(ticket.getByText(/^Not recorded/)).toBeVisible();
    const orders = await request.get(`/api/v1/accounts/${accountId}/orders`);
    expect(((await orders.json()) as { orders: unknown[] }).orders).toHaveLength(0);
  }
});
