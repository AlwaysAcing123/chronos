import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import { createFakeResearch } from '../../test/fakeResearch';
import { renderWithServices } from '../../test/renderWithServices';
import {
  experimentJson,
  experimentPageJson,
  jobJson,
  researchStatusJson,
  succeededJobJson,
} from '../../test/researchFixtures';
import { Research } from './Research';

const hex = (number: number, width: number) => number.toString(16).padStart(width, '0');

// Sixty jobs, newest first, with their sequence in the job ID.
function jobs(from: number, to: number) {
  const list = [];
  for (let number = from; number > to; number -= 1) {
    list.push(succeededJobJson({ job_id: hex(number, 32), created_at: '2026-10-04T05:40:00.000000000Z' }));
  }
  return list;
}

describe('Research lists longer than one page', () => {
  it('loads older runs and more experiments only when asked, without repeats, keeping the selection', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
    const experiments = Array.from({ length: 51 }, (_, index) => experimentJson({ experiment_id: hex(index + 1, 64) }));
    research.on('GET', `${RESEARCH_API}/experiments`, (request) =>
      request.query.get('cursor') === null
        ? { status: 200, body: { ...experimentPageJson(experiments.slice(0, 50)), next_cursor: 'experiments-after-50' } }
        : { status: 200, body: experimentPageJson(experiments.slice(50)) },
    );
    // A job arrives after the first page; the keyset cursor still continues
    // with exactly the older ten.
    research.on('GET', `${RESEARCH_API}/runs`, (request) =>
      request.query.get('cursor') === null
        ? { status: 200, body: { runs: jobs(60, 10), next_cursor: 'runs-before-11' } }
        : { status: 200, body: { runs: jobs(10, 0), next_cursor: null } },
    );
    research.on('GET', `${RESEARCH_API}/results`, { status: 200, body: { results: [], invalid_entries: [], next_cursor: null } });
    const { user } = renderWithServices(<Research resultId={null} />, { research });

    const runs = await screen.findByRole('region', { name: 'Runs' });
    expect(await within(runs).findByText('50 runs shown')).toBeInTheDocument();
    expect(research.requestsTo('GET', `${RESEARCH_API}/runs`)).toHaveLength(1);
    await user.click(within(runs).getByRole('button', { name: 'Load more runs' }));
    expect(await within(runs).findAllByRole('link', { name: 'Open result' })).toHaveLength(60);
    expect(research.requestsTo('GET', `${RESEARCH_API}/runs`).at(-1)?.query.get('cursor')).toBe('runs-before-11');
    // The last page is loaded, so there is nothing more to offer.
    expect(within(runs).queryByRole('button', { name: /Load more/ })).not.toBeInTheDocument();

    const model = screen.getByRole('region', { name: 'Model' });
    const second = within(model).getAllByRole('radio', { name: /^[0-9a-f]{12}…/ })[1];
    if (second === undefined) {
      throw new Error('expected a second experiment');
    }
    await user.click(second);
    await user.click(within(model).getByRole('button', { name: 'Load more experiments' }));
    expect(await within(model).findAllByRole('radio', { name: /^[0-9a-f]{12}…/ })).toHaveLength(51);
    expect(within(model).getAllByRole('radio', { name: /^[0-9a-f]{12}…/ })[1]).toBeChecked();
    expect(research.requestsTo('GET', `${RESEARCH_API}/experiments`).at(-1)?.query.get('cursor')).toBe('experiments-after-50');
  });

  it('keeps what is shown when an older page fails, and offers to try again', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
    research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
    research.on('GET', `${RESEARCH_API}/results`, { status: 200, body: { results: [], invalid_entries: [], next_cursor: null } });
    research.on('GET', `${RESEARCH_API}/runs`, (request) =>
      request.query.get('cursor') === null
        ? { status: 200, body: { runs: [jobJson({ state: 'succeeded' })], next_cursor: 'older' } }
        : { status: 400, body: { error: { code: 'malformed_request', message: 'bad cursor', field: 'cursor', reason: 'invalid_cursor' } } },
    );
    const { user } = renderWithServices(<Research resultId={null} />, { research });
    const runs = await screen.findByRole('region', { name: 'Runs' });
    await user.click(await within(runs).findByRole('button', { name: 'Load more runs' }));
    expect(await within(runs).findByText('The next page could not be loaded.')).toBeInTheDocument();
    expect(within(runs).getByText('1 runs shown')).toBeInTheDocument();
    expect(within(runs).getByRole('button', { name: 'Try again' })).toBeEnabled();
  });
});
