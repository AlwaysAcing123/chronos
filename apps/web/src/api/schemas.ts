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

export const ErrorBodySchema = z.object({
  code: z.string().min(1),
  message: z.string(),
  field: z.string().nullable(),
  reason: z.string().nullable(),
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

export const HealthSchema = z
  .object({
    status: z.enum(['ok', 'unavailable']),
    trading_mode: PaperTradingMode,
    persistence: z.enum(['available', 'unavailable']),
    tradeable_instruments: z.array(IdentifierSchema).min(1),
    market_data: z.object({
      stream_id: z.string().min(1),
      state: z.enum(['accepting', 'interrupted']),
      last_ingest_sequence: UnsignedIntegerSchema.nullable(),
      market_time: TimestampSchema.nullable(),
      interruption: z.string().nullable(),
    }),
  })
  .refine(
    (health) =>
      (health.market_data.state === 'interrupted') === (health.market_data.interruption !== null),
    { message: 'an interrupted stream must describe its interruption' },
  );
export type Health = z.infer<typeof HealthSchema>;

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

export const AccountSchema = z.object({
  trading_mode: PaperTradingMode,
  account_id: IdentifierSchema,
  instrument: IdentifierSchema,
  version: UnsignedIntegerSchema,
  status: AccountStatusSchema,
  starting_cash: MoneySchema,
  cash: MoneySchema,
  position: QuantitySchema,
  open_cost_basis: MoneySchema,
  average_entry_price: PriceSchema.nullable(),
  gross_realized_pnl: MoneySchema,
  fees_paid: MoneySchema,
  last_fill_at: TimestampSchema.nullable(),
  risk_limits: RiskLimitsSchema,
});
export type Account = z.infer<typeof AccountSchema>;

export const PortfolioSchema = z.object({
  trading_mode: PaperTradingMode,
  account_id: IdentifierSchema,
  instrument: IdentifierSchema,
  account_version: UnsignedIntegerSchema,
  status: AccountStatusSchema,
  valued_at: TimestampSchema,
  mark_policy: z.literal('best_bid'),
  mark_price: PriceSchema,
  market: MarketObservationSchema,
  starting_cash: MoneySchema,
  cash: MoneySchema,
  position: QuantitySchema,
  open_cost_basis: MoneySchema,
  average_entry_price: PriceSchema.nullable(),
  position_market_value: MoneySchema,
  unrealized_pnl: MoneySchema,
  gross_realized_pnl: MoneySchema,
  fees_paid: MoneySchema,
  total_pnl: MoneySchema,
  equity: MoneySchema,
  return_percent: PercentageSchema,
});
export type Portfolio = z.infer<typeof PortfolioSchema>;

export const OrderStatusSchema = z.enum([
  'filled',
  'partially_filled_and_cancelled',
  'cancelled',
  'rejected',
  'faulted',
]);
export type OrderStatus = z.infer<typeof OrderStatusSchema>;

export const CancellationReasonSchema = z.enum([
  'limit_not_marketable',
  'displayed_liquidity_exhausted',
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

const EXECUTED_STATUSES: readonly OrderStatus[] = [
  'filled',
  'partially_filled_and_cancelled',
  'cancelled',
];

export const OrderSchema = z
  .object({
    trading_mode: PaperTradingMode,
    account_id: IdentifierSchema,
    client_order_id: IdentifierSchema,
    source: OrderSourceSchema,
    instrument: IdentifierSchema,
    side: SideSchema,
    type: OrderTypeSchema,
    quantity: QuantitySchema,
    limit_price: PriceSchema.nullable(),
    time_in_force: z.literal('ioc'),
    submitted_at: TimestampSchema,
    status: OrderStatusSchema,
    filled_quantity: QuantitySchema,
    unfilled_quantity: QuantitySchema,
    cancellation_reason: CancellationReasonSchema.nullable(),
    fill: OrderFillSchema.nullable(),
    rejection: ErrorBodySchema.nullable(),
    market: MarketObservationSchema,
  })
  .superRefine((order, context) => {
    // The presentation relies on these contract invariants, so a record
    // that breaks them is refused rather than shown half-explained.
    const executed = EXECUTED_STATUSES.includes(order.status);
    if (executed === (order.rejection !== null)) {
      context.addIssue({
        code: 'custom',
        message: 'only rejected and faulted orders carry a rejection',
        path: ['rejection'],
      });
    }
    const booksFill = order.status === 'filled' || order.status === 'partially_filled_and_cancelled';
    if (booksFill !== (order.fill !== null)) {
      context.addIssue({
        code: 'custom',
        message: 'only filled and partially filled orders carry a fill',
        path: ['fill'],
      });
    }
    const hasCancellation =
      order.status === 'cancelled' || order.status === 'partially_filled_and_cancelled';
    if (hasCancellation !== (order.cancellation_reason !== null)) {
      context.addIssue({
        code: 'custom',
        message: 'only cancelled orders carry a cancellation reason',
        path: ['cancellation_reason'],
      });
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

export const AccountEnvelopeSchema = z.object({ account: AccountSchema });
export const PortfolioEnvelopeSchema = z.object({ portfolio: PortfolioSchema });
export const OrderEnvelopeSchema = z.object({ order: OrderSchema });

// A recorded submission always carries the order and the account after
// processing; rejected and faulted ones also repeat the rejection as `error`.
export const RecordedSubmissionSchema = z.object({
  order: OrderSchema,
  account: AccountSchema,
  error: ErrorBodySchema.optional(),
});
