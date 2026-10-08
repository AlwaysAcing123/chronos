import { createHash } from 'node:crypto';

import AxeBuilder from '@axe-core/playwright';
import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

import { IMPORTED_VERSION_ID, IMPORT_MANIFEST_TEXT } from '../src/test/importFixtures';
import { MODEL_VERSION_ID } from '../src/test/modelFixtures';
import { mockChronosApi } from './mockApi';
import { mockResearchService } from './researchMock';

const MODEL = Buffer.from([8, 8, 18, 4, 116, 101, 115, 116]);

async function expectNoAccessibilityViolations(page: Page) {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze();
  expect(results.violations.map((violation) => `${violation.id}: ${violation.help}`)).toEqual([]);
}

async function expectNoHorizontalOverflow(page: Page) {
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  expect(overflow).toBeLessThanOrEqual(0);
}

test('registers a model version, opens and archives it, and keeps the trading account', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await page.goto('/');
  const create = page.getByRole('region', { name: 'Create a paper account' });
  await create.getByLabel('Account ID').fill('e2e-models-1');
  await create.getByRole('button', { name: 'Create paper account' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();

  await page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Models' }).click();
  await expect(page.getByRole('heading', { level: 1, name: 'Model library' })).toBeVisible();
  await expect(page.getByText('No model versions are registered yet.')).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);

  // Keyboard only: choose the instrument, review, and register.
  const form = page.getByRole('region', { name: 'Register a model version' });
  await form.getByLabel('Instrument').focus();
  await form.getByLabel('Instrument').selectOption('SPY');
  await form.getByRole('button', { name: 'Review registration' }).focus();
  await page.keyboard.press('Enter');
  await expect(form.getByText(/cannot later point to another model/)).toBeVisible();
  await form.getByRole('button', { name: 'Register' }).focus();
  await page.keyboard.press('Enter');
  await expect(form.getByText('Model version registered')).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await form.getByRole('link', { name: 'Open the model version' }).click();

  const identity = page.getByRole('region', { name: 'aapl-logistic-20260925 1' });
  await expect(identity.getByText(MODEL_VERSION_ID)).toBeVisible();
  await expect(page.getByRole('region', { name: 'Training provenance' }).getByText('Verified by Chronos', { exact: true })).toBeVisible();
  await expect(page.getByRole('region', { name: 'Inference compatibility' }).getByText('Compatible', { exact: true })).toHaveCount(2);
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);

  await identity.getByLabel('Reason, for the record').fill('superseded in review');
  await identity.getByRole('button', { name: 'Archive this version' }).click();
  await expect(page.getByText('This version is archived')).toBeVisible();

  // A reload reopens the same version.
  await page.reload();
  await expect(page.getByRole('region', { name: 'aapl-logistic-20260925 1' })).toBeVisible();
  expect(page.url()).toContain(`#/models/${MODEL_VERSION_ID}`);

  await page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Trading' }).click();
  await expect(page.getByRole('region', { name: 'Order ticket' })).toBeVisible();
  await expect(page.getByText('e2e-models-1').first()).toBeVisible();
});

test('imports an ONNX model after review and shows what was declared apart from what was verified', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page);
  await page.goto('/#/models');
  const form = page.getByRole('region', { name: 'Import an ONNX model' });
  await form.getByLabel('Import manifest (JSON)').setInputFiles({
    name: 'import-manifest.json',
    mimeType: 'application/json',
    buffer: Buffer.from(IMPORT_MANIFEST_TEXT),
  });
  await form.getByLabel('Model (.onnx)').setInputFiles({
    name: 'model.onnx',
    mimeType: 'application/octet-stream',
    buffer: MODEL,
  });
  await form.getByRole('button', { name: 'Review import' }).click();
  await expect(form.getByText(createHash('sha256').update(MODEL).digest('hex'))).toBeVisible();
  await expect(form.getByText('Declared by uploader, not verified')).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);

  await form.getByRole('button', { name: 'Validate and import' }).click();
  await expect(form.getByText('Model version imported')).toBeVisible();
  await form.getByRole('link', { name: 'Open the model version' }).click();
  const provenance = page.getByRole('region', { name: 'Training provenance' });
  await expect(provenance.getByText('Declared by the uploader, not verified')).toBeVisible();
  await expect(provenance.getByText('Sandbox validation')).toBeVisible();
  await expect(page.getByRole('region', { name: 'aapl-logistic-onnx 1' }).getByText(IMPORTED_VERSION_ID)).toBeVisible();
  await expectNoAccessibilityViolations(page);
  await expectNoHorizontalOverflow(page);
});

test('shows how to start the research service when it is unreachable', async ({ page }) => {
  await mockChronosApi(page);
  await mockResearchService(page, { unreachable: true });
  await page.goto('/#/models');
  await expect(page.getByText('The research service is not reachable')).toBeVisible();
  await expectNoAccessibilityViolations(page);
});
