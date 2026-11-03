// Runtime decoders for every API resource (docs/paper-trading-api.md). A
// response that does not match is reported as an unexpected response rather
// than displayed. Unknown keys are ignored so additive server changes within
// /api/v1 do not break the client; every documented key is required.
import { z } from 'zod';

import { FRACTION_DIGITS, isCanonicalDecimal } from './decimal';
import { IDENTIFIER_PATTERN } from './identifiers';

function canonicalDecimal(scale: number) {
  return z.string().refine((text) => isCanonicalDecimal(text, scale), {
    message: `expected a canonical decimal string with at most ${String(scale)} fractional digits`,
  });
}

// Decimals stay strings; the brands keep a price from being used as money.
export const MoneySchema = canonicalDecimal(FRACTION_DIGITS.money).brand<'Money'>();
export const PriceSchema = canonicalDecimal(FRACTION_DIGITS.price).brand<'Price'>();
export const QuantitySchema = canonicalDecimal(FRACTION_DIGITS.quantity).brand<'Quantity'>();
export const PercentageSchema = canonicalDecimal(FRACTION_DIGITS.percentage).brand<'Percentage'>();

export type Money = z.infer<typeof MoneySchema>;
export type Price = z.infer<typeof PriceSchema>;
export type Quantity = z.infer<typeof QuantitySchema>;
export type Percentage = z.infer<typeof PercentageSchema>;
export type ExactDecimal = Money | Price | Quantity | Percentage;

// Versions, sequences, and durations are base-10 strings because JSON
// numbers above 2^53 lose precision.
export const UnsignedIntegerSchema = z
  .string()
  .regex(/^(0|[1-9][0-9]*)$/, 'expected a base-10 unsigned integer string')
  .brand<'UnsignedInteger'>();
export type UnsignedInteger = z.infer<typeof UnsignedIntegerSchema>;

// RFC 3339 UTC with exactly nine fractional digits, exact to the nanosecond.
export const TimestampSchema = z
  .string()
  .regex(
    /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z$/,
    'expected an RFC 3339 UTC timestamp with nine fractional digits',
  )
  .brand<'Timestamp'>();
export type Timestamp = z.infer<typeof TimestampSchema>;

export const IdentifierSchema = z.string().regex(IDENTIFIER_PATTERN, 'expected an identifier');

const PaperTradingMode = z.literal('paper');

// One held position the server could not mark, and why.
export const PositionMarkFailureSchema = z.object({
  instrument: IdentifierSchema,
  reason: z.string().min(1),
  message: z.string(),
});
export type PositionMarkFailure = z.infer<typeof PositionMarkFailureSchema>;

export const ErrorBodySchema = z.object({
  code: z.string().min(1),
  message: z.string(),
  field: z.string().nullable(),
  reason: z.string().nullable(),
  // Present only when the reason is position_mark_unavailable: every held
  // position that could not be marked.
  instruments: z.array(PositionMarkFailureSchema).optional(),
});
export type ErrorBody = z.infer<typeof ErrorBodySchema>;

export const ErrorEnvelopeSchema = z.object({ error: ErrorBodySchema });

export const SideSchema = z.enum(['buy', 'sell']);
export type Side = z.infer<typeof SideSchema>;

export const OrderTypeSchema = z.enum(['market', 'limit']);
export type OrderType = z.infer<typeof OrderTypeSchema>;

export const SourceKindSchema = z.enum(['manual', 'model']);
export type SourceKind = z.infer<typeof SourceKindSchema>;

export const OrderSourceSchema = z.object({
  kind: SourceKindSchema,
  id: IdentifierSchema,
});
export type OrderSource = z.infer<typeof OrderSourceSchema>;

export const QualityFlagSchema = z.enum([
  'snapshot',
  'replay',
  'questionable_receive_timestamp',
  'potentially_invalid_book',
  'source_specific',
]);
export type QualityFlag = z.infer<typeof QualityFlagSchema>;

export const BookLevelSchema = z.object({
  price: PriceSchema,
  quantity: QuantitySchema,
});
export type BookLevel = z.infer<typeof BookLevelSchema>;

export const MarketObservationSchema = z.object({
  stream_id: z.string().min(1),
  ingest_sequence: UnsignedIntegerSchema,
  venue: z.string().nullable(),
  event_timestamp: TimestampSchema,
  receive_timestamp: TimestampSchema,
  quality_flags: z.array(QualityFlagSchema),
  bid: BookLevelSchema.nullable(),
  ask: BookLevelSchema.nullable(),
});
export type MarketObservation = z.infer<typeof MarketObservationSchema>;

// The validated stream behind the server's market state.
export const MarketStreamSchema = z
  .object({
    stream_id: z.string().min(1),
    state: z.enum(['accepting', 'interrupted']),
    last_ingest_sequence: UnsignedIntegerSchema.nullable(),
    market_time: TimestampSchema.nullable(),
    interruption: z.string().nullable(),
  })
  .refine((stream) => (stream.state === 'interrupted') === (stream.interruption !== null), {
    message: 'an interrupted stream must describe its interruption',
  });
export type MarketStream = z.infer<typeof MarketStreamSchema>;

export const OrderMatchingStateSchema = z.enum(['running', 'retrying', 'stopped']);
export type OrderMatchingState = z.infer<typeof OrderMatchingStateSchema>;

// The working-order matcher, reported by servers that run one.
export const OrderMatchingSchema = z.object({
  state: OrderMatchingStateSchema,
  observations: UnsignedIntegerSchema,
  account_matches: UnsignedIntegerSchema,
  fills: UnsignedIntegerSchema,
  retries: UnsignedIntegerSchema,
  failed_account_matches: UnsignedIntegerSchema,
  total_matching_ns: UnsignedIntegerSchema,
  longest_matching_ns: UnsignedIntegerSchema,
});
export type OrderMatching = z.infer<typeof OrderMatchingSchema>;

export const HealthSchema = z.object({
  status: z.enum(['ok', 'unavailable']),
  trading_mode: PaperTradingMode,
  persistence: z.enum(['available', 'unavailable']),
  tradeable_instruments: z.array(IdentifierSchema).min(1),
  market_data: MarketStreamSchema,
  order_matching: OrderMatchingSchema.optional(),
});
export type Health = z.infer<typeof HealthSchema>;

export const MarketDataModeSchema = z.enum(['none', 'historical_replay', 'live']);
export type MarketDataMode = z.infer<typeof MarketDataModeSchema>;

export const MarketDataSourceStateSchema = z.enum([
  'none',
  'ready',
  'running',
  'paused',
  'completed',
  'connecting',
  'authenticating',
  'subscribing',
  'live',
  'reconnecting',
  'failed',
  'stopped',
]);
export type MarketDataSourceState = z.infer<typeof MarketDataSourceStateSchema>;

export const ReplaySpeedSchema = z.enum(['1x', '10x', '100x', 'max']);
export type ReplaySpeed = z.infer<typeof ReplaySpeedSchema>;

export const MarketDataSourceSchema = z
  .object({
    mode: MarketDataModeSchema,
    // The events were invented by a generator, not observed on a market: a
    // replay of a synthetic dataset (ADR 0025).
    synthetic: z.boolean(),
    provider: z.string().min(1).nullable(),
    feed: z.string().min(1).nullable(),
    label: z.string().min(1),
    // A limitation of the feed that every view must show.
    coverage_notice: z.string().min(1).nullable(),
    // The same limitation in a few words, for the source bar.
    coverage_label: z.string().min(1).nullable(),
    state: MarketDataSourceStateSchema,
    replay_speed: ReplaySpeedSchema.nullable(),
    events_applied: UnsignedIntegerSchema,
    diagnostic: z.string().nullable(),
    recording_sha256: z.string().regex(/^[0-9a-f]{64}$/, 'expected a SHA-256 digest').nullable(),
    instruments: z.array(IdentifierSchema),
  })
  .refine((source) => (source.mode === 'historical_replay') === (source.replay_speed !== null), {
    message: 'only a replay has a playback speed',
    path: ['replay_speed'],
  })
  .refine((source) => !source.synthetic || source.mode === 'historical_replay', {
    message: 'only a replay can be synthetic',
    path: ['synthetic'],
  });
export type MarketDataSource = z.infer<typeof MarketDataSourceSchema>;

export const InstrumentAvailabilitySchema = z
  .object({
    instrument: IdentifierSchema,
    execution_quote_available: z.boolean(),
    unavailable_reason: z.string().min(1).nullable(),
    // The quote an order would execute against now, for reference.
    quote: MarketObservationSchema.nullable(),
  })
  .refine((item) => item.execution_quote_available === (item.unavailable_reason === null), {
    message: 'an unavailable quote must say why',
  })
  .refine((item) => item.execution_quote_available === (item.quote !== null), {
    message: 'exactly an available instrument carries its quote',
    path: ['quote'],
  });
export type InstrumentAvailability = z.infer<typeof InstrumentAvailabilitySchema>;

export const MarketDataSchema = z.object({
  trading_mode: PaperTradingMode,
  revision: UnsignedIntegerSchema,
  source: MarketDataSourceSchema,
  stream: MarketStreamSchema,
  instruments: z.array(InstrumentAvailabilitySchema),
});
export type MarketData = z.infer<typeof MarketDataSchema>;

export const MarketDataEnvelopeSchema = z.object({ market_data: MarketDataSchema });

export const AccountStatusSchema = z.enum(['active', 'halted']);
export type AccountStatus = z.infer<typeof AccountStatusSchema>;

export const RiskLimitsSchema = z.object({
  maximum_order_quantity: QuantitySchema,
  maximum_order_notional: MoneySchema,
  maximum_position_quantity: QuantitySchema,
  maximum_quote_age_ns: UnsignedIntegerSchema,
  fee_per_fill: MoneySchema,
});
export type RiskLimits = z.infer<typeof RiskLimitsSchema>;

// An open position's ledger. Quantity and cost basis are positive, so the
// average entry price is always defined. Shares held back for working sells
// are reserved; the server reports what remains available.
export const OpenPositionSchema = z.object({
  instrument: IdentifierSchema,
  quantity: QuantitySchema,
  reserved_quantity: QuantitySchema,
  available_quantity: QuantitySchema,
  open_cost_basis: MoneySchema,
  average_entry_price: PriceSchema,
  gross_realized_pnl: MoneySchema,
  fees_paid: MoneySchema,
  last_fill_at: TimestampSchema,
});
export type OpenPosition = z.infer<typeof OpenPositionSchema>;

// A position that was closed keeps what it realized and paid.
export const ClosedPositionSchema = z.object({
  instrument: IdentifierSchema,
  gross_realized_pnl: MoneySchema,
  fees_paid: MoneySchema,
  last_fill_at: TimestampSchema,
});
export type ClosedPosition = z.infer<typeof ClosedPositionSchema>;

// Account totals are the server's sums over every position, open or closed.
export const AccountSchema = z.object({
  trading_mode: PaperTradingMode,
  account_id: IdentifierSchema,
  version: UnsignedIntegerSchema,
  status: AccountStatusSchema,
  market_stream_id: z.string().min(1).nullable(),
  starting_cash: MoneySchema,
  cash: MoneySchema,
  // Cash working orders hold back, and the rest, as the server computed them.
  reserved_cash: MoneySchema,
  available_cash: MoneySchema,
  working_order_count: UnsignedIntegerSchema,
  open_cost_basis: MoneySchema,
  gross_realized_pnl: MoneySchema,
  fees_paid: MoneySchema,
  last_fill_at: TimestampSchema.nullable(),
  positions: z.array(OpenPositionSchema),
  closed_positions: z.array(ClosedPositionSchema),
  risk_limits: RiskLimitsSchema,
  // The model deployment that owns the account, if any; only it trades the
  // account. Older servers omit it, and their accounts are all manual.
  owner: z
    .object({ kind: z.literal('model_deployment'), deployment_id: IdentifierSchema })
    .nullable()
    .default(null),
});
export type Account = z.infer<typeof AccountSchema>;

// An open position marked at its best bid.
export const ValuedPositionSchema = OpenPositionSchema.extend({
  mark_price: PriceSchema,
  market: MarketObservationSchema,
  market_value: MoneySchema,
  unrealized_pnl: MoneySchema,
});
export type ValuedPosition = z.infer<typeof ValuedPositionSchema>;

export const PortfolioSchema = z.object({
  trading_mode: PaperTradingMode,
  account_id: IdentifierSchema,
  account_version: UnsignedIntegerSchema,
  status: AccountStatusSchema,
  market_stream_id: z.string().min(1).nullable(),
  // Null only before the market's first event, when nothing is held.
  valued_at: TimestampSchema.nullable(),
  mark_policy: z.literal('best_bid'),
  starting_cash: MoneySchema,
  cash: MoneySchema,
  reserved_cash: MoneySchema,
  available_cash: MoneySchema,
  open_cost_basis: MoneySchema,
  market_value: MoneySchema,
  unrealized_pnl: MoneySchema,
  gross_realized_pnl: MoneySchema,
  fees_paid: MoneySchema,
  total_pnl: MoneySchema,
  equity: MoneySchema,
  return_percent: PercentageSchema,
  positions: z.array(ValuedPositionSchema),
  closed_positions: z.array(ClosedPositionSchema),
});
export type Portfolio = z.infer<typeof PortfolioSchema>;

export const TimeInForceSchema = z.enum(['ioc', 'gtc']);
export type TimeInForce = z.infer<typeof TimeInForceSchema>;

// Working and partially_filled are the working states: the remainder rests
// until later quotes fill it or it is cancelled.
export const OrderStatusSchema = z.enum([
  'working',
  'partially_filled',
  'filled',
  'partially_filled_and_cancelled',
  'cancelled',
  'rejected',
  'faulted',
]);
export type OrderStatus = z.infer<typeof OrderStatusSchema>;

export const WORKING_STATUSES: readonly OrderStatus[] = ['working', 'partially_filled'];

export function isWorking(status: OrderStatus): boolean {
  return WORKING_STATUSES.includes(status);
}

export const CancellationReasonSchema = z.enum([
  'limit_not_marketable',
  'displayed_liquidity_exhausted',
  'user_requested',
]);
export type CancellationReason = z.infer<typeof CancellationReasonSchema>;

// Fill IDs are server-assigned (such as "buy-1:fill:0"), not client identifiers.
const FillIdSchema = z.string().min(1);

export const OrderFillSchema = z.object({
  fill_id: FillIdSchema,
  side: SideSchema,
  price: PriceSchema,
  quantity: QuantitySchema,
  notional: MoneySchema,
  fee: MoneySchema,
  filled_at: TimestampSchema,
});
export type OrderFill = z.infer<typeof OrderFillSchema>;

const UNEXECUTED_STATUSES: readonly OrderStatus[] = ['rejected', 'faulted'];

// Canonical decimals have one spelling for zero.
const isZero = (value: string) => value === '0';

export const OrderSchema = z
  .object({
    trading_mode: PaperTradingMode,
    account_id: IdentifierSchema,
    client_order_id: IdentifierSchema,
    source: OrderSourceSchema,
    instrument: IdentifierSchema,
    side: SideSchema,
    type: OrderTypeSchema,
    // The original quantity: filled plus remaining.
    quantity: QuantitySchema,
    limit_price: PriceSchema.nullable(),
    time_in_force: TimeInForceSchema,
    submitted_at: TimestampSchema,
    status: OrderStatusSchema,
    filled_quantity: QuantitySchema,
    remaining_quantity: QuantitySchema,
    // The same value under its original name.
    unfilled_quantity: QuantitySchema,
    filled_notional: MoneySchema,
    fill_count: UnsignedIntegerSchema,
    fee_charged: z.boolean(),
    cumulative_fee: MoneySchema,
    reserved_cash: MoneySchema,
    reserved_quantity: QuantitySchema,
    // A working order's durable acceptance priority; opaque to clients.
    priority: UnsignedIntegerSchema.nullable(),
    updated_at: TimestampSchema,
    cancellation_reason: CancellationReasonSchema.nullable(),
    cancel_request_id: IdentifierSchema.nullable(),
    // The fill booked while the submission was processed.
    fill: OrderFillSchema.nullable(),
    rejection: ErrorBodySchema.nullable(),
    market: MarketObservationSchema,
  })
  .superRefine((order, context) => {
    // The presentation relies on these contract invariants, so a record
    // that breaks them is refused rather than shown half-explained. They
    // compare values; the client computes none.
    const issue = (message: string, path: string) => {
      context.addIssue({ code: 'custom', message, path: [path] });
    };
    const executed = !UNEXECUTED_STATUSES.includes(order.status);
    const rests = order.time_in_force === 'gtc';
    if (executed === (order.rejection !== null)) {
      issue('only rejected and faulted orders carry a rejection', 'rejection');
    }
    if (order.unfilled_quantity !== order.remaining_quantity) {
      issue('unfilled and remaining quantities are one value', 'unfilled_quantity');
    }
    if (rests) {
      if (order.fill !== null && isZero(order.filled_quantity)) {
        issue('a fill needs a filled quantity', 'fill');
      }
    } else {
      const booksFill = order.status === 'filled' || order.status === 'partially_filled_and_cancelled';
      if (booksFill !== (order.fill !== null)) {
        issue('only filled and partially filled orders carry a fill', 'fill');
      }
    }
    if (order.type === 'market' && rests) {
      issue('a market order is immediate-or-cancel', 'time_in_force');
    }
    if (isWorking(order.status) && !rests) {
      issue('only a good-til-cancelled order works', 'status');
    }
    const hasCancellation =
      order.status === 'cancelled' || order.status === 'partially_filled_and_cancelled';
    if (hasCancellation !== (order.cancellation_reason !== null)) {
      issue('only cancelled orders carry a cancellation reason', 'cancellation_reason');
    }
    if ((order.cancellation_reason === 'user_requested') !== (order.cancel_request_id !== null)) {
      issue('a client cancellation names its request', 'cancel_request_id');
    }
    // Clients cancel only good-til-cancelled orders, and nothing else
    // cancels them.
    if (order.cancellation_reason !== null && (order.cancellation_reason === 'user_requested') !== rests) {
      issue('only a good-til-cancelled order is cancelled by a client', 'cancellation_reason');
    }
    const filledAny = !isZero(order.filled_quantity);
    if (order.fee_charged !== filledAny || isZero(order.fill_count) === filledAny) {
      issue('the first fill charges the fee', 'fee_charged');
    }
    if (isZero(order.filled_notional) === filledAny) {
      issue('a filled quantity has a notional', 'filled_notional');
    }
    const remainingAny = !isZero(order.remaining_quantity);
    if (
      (order.status === 'working' && filledAny) ||
      (order.status === 'partially_filled' && (!filledAny || !remainingAny)) ||
      (order.status === 'filled' && remainingAny) ||
      (order.status === 'partially_filled_and_cancelled' && (!filledAny || !remainingAny)) ||
      (order.status === 'cancelled' && filledAny)
    ) {
      issue('the quantities do not fit the status', 'status');
    }
    if (!isWorking(order.status) && (!isZero(order.reserved_cash) || !isZero(order.reserved_quantity))) {
      issue('only a working order reserves anything', 'reserved_cash');
    }
    if ((order.priority !== null) !== (rests && executed)) {
      issue('exactly an executed good-til-cancelled order has a priority', 'priority');
    }
  });
export type Order = z.infer<typeof OrderSchema>;

export const FillRecordSchema = z.object({
  trading_mode: PaperTradingMode,
  account_id: IdentifierSchema,
  client_order_id: IdentifierSchema,
  source: OrderSourceSchema,
  instrument: IdentifierSchema,
  fill_id: FillIdSchema,
  side: SideSchema,
  price: PriceSchema,
  quantity: QuantitySchema,
  notional: MoneySchema,
  fee: MoneySchema,
  filled_at: TimestampSchema,
  // Its position among its order's fills; only the first pays the fee.
  fill_index: UnsignedIntegerSchema,
});
export type FillRecord = z.infer<typeof FillRecordSchema>;

export const OrderPageSchema = z.object({
  orders: z.array(OrderSchema),
  next_cursor: z.string().min(1).nullable(),
});
export type OrderPage = z.infer<typeof OrderPageSchema>;

export const FillPageSchema = z.object({
  fills: z.array(FillRecordSchema),
  next_cursor: z.string().min(1).nullable(),
});
export type FillPage = z.infer<typeof FillPageSchema>;

// Working orders in priority order; every one is still working.
export const WorkingOrderPageSchema = z.object({
  orders: z.array(OrderSchema.refine((order) => isWorking(order.status), { message: 'expected a working order' })),
  next_cursor: z.string().min(1).nullable(),
});
export type WorkingOrderPage = z.infer<typeof WorkingOrderPageSchema>;

export const OrderEventTypeSchema = z.enum([
  'order_submitted',
  'risk_rejected',
  'execution_rejected',
  'order_completed',
  'fill_applied',
  'account_state_updated',
  'processor_faulted',
  'order_accepted',
  'order_filled',
  'order_cancelled',
]);
export type OrderEventType = z.infer<typeof OrderEventTypeSchema>;

export const OrderEventSchema = z
  .object({
    sequence: UnsignedIntegerSchema,
    type: OrderEventTypeSchema,
    occurred_at: TimestampSchema,
    fill: OrderFillSchema.nullable(),
  })
  .refine((event) => (event.type === 'fill_applied') === (event.fill !== null), {
    message: 'exactly a fill event carries its fill',
    path: ['fill'],
  });
export type OrderEvent = z.infer<typeof OrderEventSchema>;

// One order's lifecycle, oldest first.
export const OrderEventPageSchema = z.object({
  events: z.array(OrderEventSchema),
  next_cursor: z.string().min(1).nullable(),
});
export type OrderEventPage = z.infer<typeof OrderEventPageSchema>;

export const AccountEnvelopeSchema = z.object({ account: AccountSchema });

// A cancellation, or a retry of one that already happened.
export const CancellationSchema = z.object({
  order: OrderSchema,
  account: AccountSchema,
  already_cancelled: z.boolean(),
});
export type Cancellation = z.infer<typeof CancellationSchema>;

// An order that has nothing left to cancel, as it is now.
export const NotCancellableSchema = z.object({
  error: ErrorBodySchema,
  order: OrderSchema,
});
export const PortfolioEnvelopeSchema = z.object({ portfolio: PortfolioSchema });
export const OrderEnvelopeSchema = z.object({ order: OrderSchema });

// A recorded submission always carries the order and the account after
// processing; rejected and faulted ones also repeat the rejection as `error`.
export const RecordedSubmissionSchema = z.object({
  order: OrderSchema,
  account: AccountSchema,
  error: ErrorBodySchema.optional(),
});
