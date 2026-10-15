// The demo workspace's three guided flows, on the real stack (ADR 0025):
//   python3.12 tools/demo/acceptance.py
// starts the actual supervisor and runs this with playwright.demo.config.ts.
// Every request reaches the real trading server, research service, inference
// worker, and PostgreSQL; nothing is mocked. Exact accounting is checked by
// the harness in decimal arithmetic; this spec drives the screens and reads
// the server's own strings.
import AxeBuilder from '@axe-core/playwright';
import type { APIRequestContext, Page, TestInfo } from '@playwright/test';
import { expect, test } from '@playwright/test';

test.skip(process.env.CHRONOS_DEMO_URL === undefined, 'run by tools/demo/acceptance.py against a demo workspace');

// Which session the harness opened: a fresh playback, or a retained one.
const PHASE = process.env.CHRONOS_DEMO_EXPECT ?? 'playback';

const STAMP = (process.env.CHRONOS_DEMO_STAMP ?? Date.now().toString(36)).slice(-8);
const MANUAL = `demo-manual-${STAMP}`;
const DEPLOYMENT = `demo-model-${STAMP}`;
const DEPLOYMENT_ACCOUNT = `demo-model-${STAMP}-acct`;
const WCAG = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa'];

type Json = Record<string, unknown>;

async function getJson(request: APIRequestContext, path: string): Promise<Json> {
  const response = await request.get(path);
  expect(response.status(), path).toBe(200);
  return (await response.json()) as Json;
}

// Accessibility violations, every one of which must be listed as allowed.
async function audit(page: Page, testInfo: TestInfo, name: string, allowed: readonly string[] = []) {
  const results = await new AxeBuilder({ page }).withTags(WCAG).analyze();
  const ids = results.violations.map((violation) => violation.id);
  await testInfo.attach(`axe-${name}.json`, { body: JSON.stringify(ids), contentType: 'application/json' });
  expect(ids.filter((id) => !allowed.includes(id)), `${name}: accessibility violations`).toEqual([]);
}

async function noHorizontalScroll(page: Page) {
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  expect(overflow).toBeLessThanOrEqual(0);
}

async function shot(page: Page, testInfo: TestInfo, name: string) {
  await page.screenshot({ path: testInfo.outputPath(`${testInfo.project.name}-${name}.png`), fullPage: true });
}

async function guideRoutes(request: APIRequestContext): Promise<Json> {
  const guide = await getJson(request, '/api/v1/research/demo');
  return (guide.seed as Json).routes as Json;
}

async function position(request: APIRequestContext, accountId: string, instrument: string): Promise<string> {
  const account = (await getJson(request, `/api/v1/accounts/${accountId}`)).account as Json;
  const positions = account.positions as Json[];
  return (positions.find((entry) => entry.instrument === instrument)?.quantity as string | undefined) ?? '0';
}

// The trading workspace's own pre-existing contrast findings (they fail the
// ordinary mocked suite on main too); nothing else is allowed anywhere.
const TRADING_ALLOWED = ['color-contrast'];

test.describe.serial('the three guided flows', () => {
  test.beforeEach(({ browserName }, testInfo) => {
    test.skip(
      PHASE !== 'playback' || testInfo.project.name !== 'desktop' || browserName !== 'chromium',
      'once, on desktop Chromium, in a fresh session',
    );
  });

  test('1. manual paper trading', async ({ page, request }, testInfo) => {
    await page.goto('/#/demo');
    await expect(page.getByRole('heading', { name: 'Three guided flows' })).toBeVisible();
    await expect(page.getByText(/^Session \d+: fresh playback$/)).toBeVisible();
    await audit(page, testInfo, 'guide');
    await shot(page, testInfo, 'guide');
    // Visiting the guide started nothing.
    const before = ((await getJson(request, '/api/v1/market-data')).market_data as Json).source as Json;
    expect([before.state, before.events_applied, before.synthetic]).toEqual(['ready', '0', true]);

    await page.getByRole('main').getByRole('link', { name: 'Trading' }).first().click();
    const bar = page.getByRole('region', { name: 'Market-data source' });
    await expect(bar.getByText('Synthetic replay', { exact: true })).toBeVisible();
    await expect(page.getByText('Synthetic data: invented prices, not market data', { exact: true })).toBeVisible();
    await bar.getByRole('button', { name: 'Start replay' }).click();
    await expect(bar.getByText('Playing', { exact: true })).toBeVisible();

    const create = page.getByRole('region', { name: 'Create a paper account' });
    await create.getByLabel('Account ID').fill(MANUAL);
    await create.getByRole('button', { name: 'Create paper account' }).click();
    const ticket = page.getByRole('region', { name: 'Order ticket' });
    await expect(ticket.getByText('Best bid')).toBeVisible();
    await ticket.getByLabel('Quantity (AAPL)').fill('1');
    await ticket.getByRole('button', { name: 'Review order' }).click();
    await shot(page, testInfo, 'manual-review');
    await ticket.getByRole('button', { name: 'Submit paper order' }).click();
    await expect(ticket.getByText('Recorded in the paper journal.', { exact: false })).toBeVisible();
    await expect.poll(() => position(request, MANUAL, 'AAPL')).toBe('1');
    const orders = page.getByRole('region', { name: 'Orders', exact: true });
    await expect(orders.getByRole('cell', { name: 'Filled', exact: true }).first()).toBeVisible();
    await shot(page, testInfo, 'manual-filled');
    await audit(page, testInfo, 'trading', TRADING_ALLOWED);

    await ticket.getByRole('button', { name: 'New order' }).click();
    await ticket.getByRole('radio', { name: 'Limit' }).check();
    await ticket.getByRole('radio', { name: 'Good til cancelled (GTC)' }).check();
    await ticket.getByLabel('Quantity (AAPL)').fill('2');
    await ticket.getByLabel('Limit price').fill('90');
    await ticket.getByRole('button', { name: 'Review order' }).click();
    await ticket.getByRole('button', { name: 'Submit paper order' }).click();
    await expect(ticket.getByText('Working: 2 rests at limit 90')).toBeVisible();
    const working = page.getByRole('region', { name: 'Working orders' });
    await expect(working.getByRole('row', { name: /Working/ })).toBeVisible();
    const reserved = (await getJson(request, `/api/v1/accounts/${MANUAL}`)).account as Json;
    // 2 × 90 plus the 0.01 fee until the first fill, as the server says.
    expect(reserved.reserved_cash).toBe('180.01');
    await shot(page, testInfo, 'manual-gtc-reserved');
    await working.getByRole('button', { name: /^Cancel remainder of / }).click();
    await expect(working.getByText(/No working orders/)).toBeVisible();
    const released = (await getJson(request, `/api/v1/accounts/${MANUAL}`)).account as Json;
    expect([released.reserved_cash, released.available_cash]).toEqual(['0', released.cash]);
    await shot(page, testInfo, 'manual-gtc-cancelled');
  });

  test('2. evaluate, then paper-deploy a model', async ({ page, request }, testInfo) => {
    const routes = await guideRoutes(request);
    const models = routes.models as Json;
    await page.goto('/#/demo');
    await page.getByRole('link', { name: 'comparison', exact: true }).click();
    await expect(page).toHaveURL(new RegExp(`${routes.comparison as string}$`));
    await expect(page.getByText('demo-aapl-logistic').first()).toBeVisible();
    await audit(page, testInfo, 'comparison');
    await shot(page, testInfo, 'comparison');

    await page.goto('/#/demo');
    await page.getByRole('link', { name: 'logistic model' }).click();
    await expect(page).toHaveURL(new RegExp(`${models.logistic as string}$`));
    const form = page.getByRole('region', { name: 'Deploy this version to paper' });
    await expect(form).toBeVisible();
    await audit(page, testInfo, 'model');
    await shot(page, testInfo, 'model');
    await form.getByLabel('Deployment ID').fill(DEPLOYMENT);
    await form.getByLabel('New paper account ID').fill(DEPLOYMENT_ACCOUNT);
    await form.getByRole('button', { name: 'Review deployment' }).click();
    await shot(page, testInfo, 'deploy-review');
    await form.getByRole('button', { name: 'Create deployment' }).click();
    await form.getByRole('link', { name: 'Open Paper deployments' }).click();

    const card = page.getByRole('article', { name: `Deployment ${DEPLOYMENT}` });
    await expect(card.getByText('Stopped', { exact: true }).first()).toBeVisible();
    // Created stopped: nothing decided before Start.
    const created = await getJson(request, `/api/v1/deployments/${DEPLOYMENT}/decisions`);
    expect(created.decisions).toEqual([]);
    await card.getByLabel(`Let this model place simulated orders in ${DEPLOYMENT_ACCOUNT}`).check();
    await card.getByRole('button', { name: 'Start' }).click();
    // Start answers once the worker has loaded the model: up to the server's
    // 30 s model-load deadline.
    await expect(card.getByText('Running', { exact: true }).first()).toBeVisible({ timeout: 45_000 });
    // A real saved prediction, decision, and model order, at the model's pace.
    await expect
      .poll(() => position(request, DEPLOYMENT_ACCOUNT, 'AAPL'), { timeout: 420_000, intervals: [1_000] })
      .toBe('1');
    await expect(card.getByRole('table', { name: `Latest saved decisions of ${DEPLOYMENT}` })).toBeVisible();
    await shot(page, testInfo, 'deployment-running');
    await audit(page, testInfo, 'research');
    await card.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(card.getByText('Stopped', { exact: true }).first()).toBeVisible();
    await shot(page, testInfo, 'deployment-stopped');

    await card.getByRole('button', { name: `Open ${DEPLOYMENT_ACCOUNT} in trading` }).click();
    const orders = page.getByRole('region', { name: 'Orders', exact: true });
    await expect(orders.getByText('Model', { exact: true }).first()).toBeVisible();
    // A model-owned account's ticket names its deployment and takes no manual order.
    const ownedTicket = page.getByRole('region', { name: 'Order ticket' });
    await expect(ownedTicket.getByText(`Traded by model deployment ${DEPLOYMENT}`)).toBeVisible();
    await expect(ownedTicket.getByRole('button', { name: 'Review order' })).toHaveCount(0);
    await shot(page, testInfo, 'deployment-account');
  });

  test('3. explain a strategy result, then return to the same account', async ({ page, request }, testInfo) => {
    const routes = await guideRoutes(request);
    // Open the manual account again, as a person would before reading research.
    // (Each test starts with fresh browser storage, so none is selected yet.)
    await page.goto('/#/');
    const open = page.getByRole('region', { name: 'Open an existing account' });
    await open.getByLabel('Account ID').fill(MANUAL);
    await open.getByRole('button', { name: 'Open account' }).click();
    await expect(page.getByRole('banner').getByText(MANUAL)).toBeVisible();

    await page.goto('/#/demo');
    await expect(page.getByText(new RegExp(`The account you had open, ${MANUAL}, is still selected`))).toBeVisible();
    await page.getByRole('link', { name: 'AAPL walk-forward study' }).click();
    await expect(page).toHaveURL(new RegExp(`${(routes.studies as Json).AAPL as string}$`));
    await expect(page.getByText('Trading results by fold').first()).toBeVisible();
    await audit(page, testInfo, 'study');
    await shot(page, testInfo, 'study');

    await page.goto('/#/demo');
    await page.getByRole('link', { name: 'the first fold’s logistic base run' }).click();
    await expect(page).toHaveURL(new RegExp(`${routes.diagnostics_run as string}$`));
    await expect(page.getByText('PnL bridge').first()).toBeVisible();
    await expect(page.getByRole('heading', { name: 'Verification, level by level' })).toBeVisible();
    await audit(page, testInfo, 'diagnostics');
    await shot(page, testInfo, 'diagnostics-run');

    await page.goto('/#/demo');
    await page.getByRole('main').getByRole('link', { name: 'Trading' }).last().click();
    await expect(page.getByRole('banner').getByText(MANUAL)).toBeVisible();
    await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
    await shot(page, testInfo, 'returned-to-trading');
  });
});

test('the guide and trading read on a phone', async ({ page }, testInfo) => {
  test.skip(PHASE !== 'playback' || testInfo.project.name !== 'mobile', 'the phone layout, in a fresh session');
  await page.goto('/#/demo');
  await expect(page.getByRole('heading', { name: 'Three guided flows' })).toBeVisible();
  await expect(page.getByText(/^Session \d+: (fresh playback|history, with no replay)$/)).toBeVisible();
  await noHorizontalScroll(page);
  await audit(page, testInfo, 'guide-phone');
  await shot(page, testInfo, 'guide');
  await page.getByRole('main').getByRole('link', { name: 'Trading' }).first().click();
  await expect(
    page.getByRole('region', { name: 'Market-data source' }).getByText('Synthetic replay', { exact: true }),
  ).toBeVisible();
  await noHorizontalScroll(page);
  await shot(page, testInfo, 'trading');
});

test('the guide works from the keyboard', async ({ page }, testInfo) => {
  test.skip(PHASE !== 'playback' || testInfo.project.name !== 'desktop', 'once, on desktop, in a fresh session');
  await page.goto('/#/demo');
  await expect(page.getByRole('heading', { name: 'Three guided flows' })).toBeVisible();
  await page.keyboard.press('Tab');
  await expect(page.getByRole('link', { name: 'Skip to content' })).toBeFocused();
  // Tab on to the guide's own link to Trading, with a visible focus ring, and follow it.
  const trading = page.getByRole('main').getByRole('link', { name: 'Trading' }).first();
  const focused = () => trading.evaluate((element) => element === document.activeElement);
  for (let presses = 0; presses < 40 && !(await focused()); presses += 1) {
    await page.keyboard.press('Tab');
  }
  await expect(trading).toBeFocused();
  // The app's focus ring is a box shadow (--focus-ring), not an outline.
  expect(await trading.evaluate((element) => getComputedStyle(element).boxShadow)).not.toBe('none');
  await page.keyboard.press('Enter');
  await expect(page).toHaveURL(/#\/$/);
});

// After a hard-killed trading server, the harness reopens the same session.
test('a retained session reopens as history, truthfully', async ({ page, request }, testInfo) => {
  test.skip(PHASE !== 'history' || testInfo.project.name !== 'desktop', 'a retained session');
  const guide = await getJson(request, '/api/v1/research/demo');
  expect((guide.session as Json).mode).toBe('history');
  await page.goto('/#/demo');
  await expect(page.getByText(/^Session \d+: history, with no replay$/)).toBeVisible();
  await audit(page, testInfo, 'guide-history');
  await shot(page, testInfo, 'guide-history');
  await page.getByRole('main').getByRole('link', { name: 'Trading' }).first().click();
  await expect(
    page.getByRole('region', { name: 'Market-data source' }).getByText('No market data', { exact: true }),
  ).toBeVisible();
  await shot(page, testInfo, 'trading-history');
});
