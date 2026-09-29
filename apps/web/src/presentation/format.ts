// Display helpers. Server values are reformatted lexically only; nothing
// financial is parsed into a JavaScript number.
import type { Timestamp, UnsignedInteger } from '../api/schemas';

// Market timestamps are UTC; labels say so once instead of every value.

/** "2026-09-25T13:39:59.969574414Z" as "2026-09-25 13:39:59.969574414", exact to the nanosecond. */
export function formatMarketTime(timestamp: Timestamp): string {
  return `${formatMarketDate(timestamp)} ${formatMarketClock(timestamp)}`;
}

/** The date part of a market timestamp, "2026-09-25". */
export function formatMarketDate(timestamp: Timestamp): string {
  return timestamp.slice(0, 10);
}

/** The time-of-day part of a market timestamp, "13:39:59.969574414". */
export function formatMarketClock(timestamp: Timestamp): string {
  return timestamp.slice(11, -1);
}

/** A browser wall-clock instant, such as when a poll succeeded, in local time. */
export function formatLocalClock(epochMilliseconds: number): string {
  return new Date(epochMilliseconds).toLocaleTimeString(undefined, { hour12: false });
}

/** An integer nanosecond count as exact seconds: "5000000000" is "5", "1500" is "0.0000015". */
export function nanosecondsAsSeconds(nanoseconds: UnsignedInteger): string {
  const digits = nanoseconds.padStart(10, '0');
  const whole = digits.slice(0, -9).replace(/^0+(?=\d)/, '');
  const fraction = digits.slice(-9).replace(/0+$/, '');
  return fraction === '' ? whole : `${whole}.${fraction}`;
}
