import type { KeyboardEvent, PointerEvent } from 'react';
import { useId, useMemo, useState } from 'react';

import type { AccountName, EquityPoint, Valuation } from '../../api/researchSchemas';
import { isComplete } from '../../api/researchSchemas';
import { formatMarketClock } from '../../presentation/format';
import { ACCOUNT_LABEL } from '../../presentation/research';
import historyStyles from '../history/history.module.css';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import { MarketTime } from '../ui/MarketTime';
import styles from './research.module.css';

const ACCOUNTS: readonly AccountName[] = ['strategy', 'buy_and_hold', 'cash'];
const LINE: Readonly<Record<AccountName, string | undefined>> = {
  strategy: styles.lineStrategy,
  buy_and_hold: styles.lineBuyAndHold,
  cash: styles.lineCash,
};
const SWATCH: Readonly<Record<AccountName, string | undefined>> = {
  strategy: styles.swatch,
  buy_and_hold: `${styles.swatch ?? ''} ${styles.swatchBuyAndHold ?? ''}`,
  cash: `${styles.swatch ?? ''} ${styles.swatchCash ?? ''}`,
};
const WIDTH = 1000;
const HEIGHT = 260;
const PADDING = 12;

/** An RFC 3339 nanosecond timestamp as integer nanoseconds since the epoch. */
export function timestampNanoseconds(timestamp: string): bigint {
  const milliseconds = BigInt(Date.parse(`${timestamp.slice(0, 19)}Z`));
  return milliseconds * 1_000_000n + BigInt(timestamp.slice(20, 29));
}

function valuationText(valuation: Valuation): string {
  return isComplete(valuation) ? valuation.equity : `unavailable (${valuation.unavailable.code})`;
}

export function pointDescription(point: EquityPoint): string {
  const values = ACCOUNTS.map((account) => `${ACCOUNT_LABEL[account]} ${valuationText(point.accounts[account])}`);
  return `${formatMarketClock(point.at)} UTC, after ${point.trigger.replaceAll('_', ' ')}: ${values.join('; ')}`;
}

interface Geometry {
  readonly x: readonly number[];
  readonly paths: Readonly<Record<AccountName, string>>;
  readonly lowest: string | null;
  readonly highest: string | null;
}

// Approximate geometry for drawing only. Financial values on screen always
// come from the server's exact strings, never from these numbers.
function geometry(points: readonly EquityPoint[]): Geometry {
  const times = points.map((point) => timestampNanoseconds(point.at));
  const first = times[0] ?? 0n;
  const span = Number((times.at(-1) ?? 0n) - first) || 1;
  const x = times.map((time) => PADDING + (Number(time - first) / span) * (WIDTH - 2 * PADDING));
  let lowest: { value: number; text: string } | null = null;
  let highest: { value: number; text: string } | null = null;
  for (const point of points) {
    for (const account of ACCOUNTS) {
      const valuation = point.accounts[account];
      if (isComplete(valuation)) {
        const value = Number(valuation.equity);
        if (lowest === null || value < lowest.value) {
          lowest = { value, text: valuation.equity };
        }
        if (highest === null || value > highest.value) {
          highest = { value, text: valuation.equity };
        }
      }
    }
  }
  const low = lowest?.value ?? 0;
  const range = (highest?.value ?? 1) - low || 1;
  const y = (value: number) => HEIGHT - PADDING - ((value - low) / range) * (HEIGHT - 2 * PADDING);
  const paths = Object.fromEntries(
    ACCOUNTS.map((account) => {
      let path = '';
      let drawing = false;
      points.forEach((point, index) => {
        const valuation = point.accounts[account];
        if (!isComplete(valuation)) {
          // A gap, never an invented value.
          drawing = false;
          return;
        }
        path += `${drawing ? 'L' : 'M'}${(x[index] ?? 0).toFixed(2)},${y(Number(valuation.equity)).toFixed(2)}`;
        drawing = true;
      });
      return [account, path];
    }),
  ) as Record<AccountName, string>;
  return { x, paths, lowest: lowest?.text ?? null, highest: highest?.text ?? null };
}

export function EquityChart({ points }: { readonly points: readonly EquityPoint[] }) {
  const readoutId = useId();
  const [cursor, setCursor] = useState(Math.max(0, points.length - 1));
  const shape = useMemo(() => geometry(points), [points]);
  const current = points[Math.min(cursor, points.length - 1)];

  if (points.length === 0 || current === undefined) {
    return <p className={historyStyles.empty}>The run has no valuation points.</p>;
  }

  const move = (index: number) => {
    setCursor(Math.max(0, Math.min(points.length - 1, index)));
  };
  const onKey = (event: KeyboardEvent<HTMLDivElement>) => {
    const step = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 }[event.key];
    if (step !== undefined) {
      event.preventDefault();
      move(cursor + step);
    } else if (event.key === 'Home') {
      event.preventDefault();
      move(0);
    } else if (event.key === 'End') {
      event.preventDefault();
      move(points.length - 1);
    } else if (event.key === 'PageUp' || event.key === 'PageDown') {
      event.preventDefault();
      move(cursor + (event.key === 'PageUp' ? 10 : -10));
    }
  };
  const onPointer = (event: PointerEvent<HTMLDivElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const target = ((event.clientX - bounds.left) / bounds.width) * WIDTH;
    let nearest = 0;
    shape.x.forEach((value, index) => {
      if (Math.abs(value - target) < Math.abs((shape.x[nearest] ?? 0) - target)) {
        nearest = index;
      }
    });
    move(nearest);
  };

  return (
    <div className={styles.chart}>
      <ul className={styles.legend} aria-label="Lines">
        {ACCOUNTS.map((account) => (
          <li key={account}>
            <span className={SWATCH[account]} aria-hidden="true" />
            {ACCOUNT_LABEL[account]}
          </li>
        ))}
      </ul>
      <div
        className={styles.chartFrame}
        tabIndex={0}
        role="slider"
        aria-label="Equity over the window; arrow keys move between valuation points"
        aria-valuemin={1}
        aria-valuemax={points.length}
        aria-valuenow={cursor + 1}
        aria-valuetext={pointDescription(current)}
        aria-describedby={readoutId}
        onKeyDown={onKey}
        onPointerMove={onPointer}
        onPointerDown={onPointer}
      >
        <svg className={styles.chartSvg} viewBox={`0 0 ${String(WIDTH)} ${String(HEIGHT)}`} preserveAspectRatio="none" aria-hidden="true">
          <line className={styles.gridLine} x1={0} x2={WIDTH} y1={PADDING} y2={PADDING} vectorEffect="non-scaling-stroke" />
          <line
            className={styles.gridLine}
            x1={0}
            x2={WIDTH}
            y1={HEIGHT - PADDING}
            y2={HEIGHT - PADDING}
            vectorEffect="non-scaling-stroke"
          />
          {ACCOUNTS.map((account) => (
            <path key={account} className={LINE[account]} d={shape.paths[account]} vectorEffect="non-scaling-stroke" />
          ))}
          <line
            className={styles.cursor}
            x1={shape.x[cursor] ?? 0}
            x2={shape.x[cursor] ?? 0}
            y1={0}
            y2={HEIGHT}
            vectorEffect="non-scaling-stroke"
          />
        </svg>
      </div>
      <div className={styles.axis} aria-hidden="true">
        <span>
          {shape.lowest === null ? 'No valued point' : `Low ${shape.lowest}`} · {shape.highest === null ? '' : `High ${shape.highest}`}
        </span>
        <span className="mono">
          {formatMarketClock(points[0]?.at ?? current.at)} – {formatMarketClock(points.at(-1)?.at ?? current.at)} UTC
        </span>
      </div>
      <div id={readoutId} className={styles.readout}>
        <p>
          Point {cursor + 1} of {points.length}, <MarketTime value={current.at} /> UTC, after{' '}
          {current.trigger.replaceAll('_', ' ')}
        </p>
        <p>
          {ACCOUNTS.map((account, index) => {
            const valuation = current.accounts[account];
            return (
              <span key={account}>
                {index > 0 && ' · '}
                {ACCOUNT_LABEL[account]}{' '}
                {isComplete(valuation) ? (
                  <DecimalText value={valuation.equity} />
                ) : (
                  <Unavailable>{`Unavailable: ${valuation.unavailable.code.replaceAll('_', ' ')}`}</Unavailable>
                )}
              </span>
            );
          })}
        </p>
      </div>
      <details className={styles.details}>
        <summary>Equity at every valuation point (table)</summary>
        <div className={historyStyles.scroll}>
          <table className={historyStyles.table}>
            <caption className="visually-hidden">Equity at every valuation point</caption>
            <thead>
              <tr>
                <th scope="col">Time (UTC)</th>
                <th scope="col">After</th>
                {ACCOUNTS.map((account) => (
                  <th key={account} scope="col" className={historyStyles.number}>
                    {ACCOUNT_LABEL[account]}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {points.map((point, index) => (
                <tr key={`${point.at}-${String(index)}`}>
                  <td data-label="Time (UTC)">
                    <MarketTime value={point.at} />
                  </td>
                  <td data-label="After">{point.trigger.replaceAll('_', ' ')}</td>
                  {ACCOUNTS.map((account) => {
                    const valuation = point.accounts[account];
                    return (
                      <td key={account} className={historyStyles.number} data-label={ACCOUNT_LABEL[account]}>
                        {isComplete(valuation) ? (
                          <DecimalText value={valuation.equity} />
                        ) : (
                          <Unavailable>{valuation.unavailable.code.replaceAll('_', ' ')}</Unavailable>
                        )}
                      </td>
                    );
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </details>
    </div>
  );
}
