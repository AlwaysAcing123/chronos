// Strategy diagnostics exactly as the research service returned them over
// synthetic recordings: the generated week, planned with two training days so
// its one fold evaluates the third, threshold 400,000,000 ppb. The majority
// model never trades (genuine zeros); the logistic model fills 37 times, with
// 18 completed holding episodes and one censored at the window's end. Generated
// from a scratch run of the real service (strategy_diagnostics, ADR 0024);
// shared by the component tests and the mocked browser suite.
import type { Json } from './fixtures';

export const DIAGNOSED_STUDY_ID = '48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388';
export const DIAGNOSTIC_ID = 'ea7c2b639c877f0f26bc53150d97c6576c43309c887b79f2685cd4a4eab46404';
// The logistic model's base-scenario run.
export const DIAGNOSED_RUN_ID = '91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588';
export const DIAGNOSTICS_JOB_ID = 'c4f0a8e0d1b2436f9a5e7d3c2b1a0f9e';

const DETAIL: Json = {
  "diagnostics": {
    "confidence_bins": [
      {"lower_ppb": "0", "name": "0.0-0.1", "upper": "exclusive", "upper_ppb": "100000000"},
      {"lower_ppb": "100000000", "name": "0.1-0.2", "upper": "exclusive", "upper_ppb": "200000000"},
      {"lower_ppb": "200000000", "name": "0.2-0.3", "upper": "exclusive", "upper_ppb": "300000000"},
      {"lower_ppb": "300000000", "name": "0.3-0.4", "upper": "exclusive", "upper_ppb": "400000000"},
      {"lower_ppb": "400000000", "name": "0.4-0.5", "upper": "exclusive", "upper_ppb": "500000000"},
      {"lower_ppb": "500000000", "name": "0.5-0.6", "upper": "exclusive", "upper_ppb": "600000000"},
      {"lower_ppb": "600000000", "name": "0.6-0.7", "upper": "exclusive", "upper_ppb": "700000000"},
      {"lower_ppb": "700000000", "name": "0.7-0.8", "upper": "exclusive", "upper_ppb": "800000000"},
      {"lower_ppb": "800000000", "name": "0.8-0.9", "upper": "exclusive", "upper_ppb": "900000000"},
      {"lower_ppb": "900000000", "name": "0.9-1.0", "upper": "inclusive", "upper_ppb": "1000000000"}
    ],
    "counts": {"episodes": "42", "fills": "78", "runs": "4", "runs_attributed": "4"},
    "definitions": {
      "attribution": {
        "booked_net_pnl": "the engine's final net_total_pnl",
        "execution_spread_cost": "sum q_i*(p_i - m_i)",
        "fees": "sum f_i",
        "inventory_pnl_at_bid": "Q*B - sum q_i*m_i = midpoint_inventory_pnl - terminal_bid_markdown",
        "market_value_rounding": "V - Q*B",
        "midpoint_inventory_pnl": "Q*M - sum q_i*m_i",
        "notional_rounding": "sum (n_i - q_i*p_i)",
        "observed_midpoint_movement": "sum q_i*(m_i - s_i); positive is adverse",
        "rounding_adjustment": "(V - Q*B) - sum (n_i - q_i*p_i)",
        "signal_reference_pnl": "Q*M - sum q_i*s_i",
        "terminal_bid_markdown": "Q*(M - B)"
      },
      "cohorts": {
        "filled_orders": "order-scheduling decisions whose order filled, at least in part",
        "order_scheduling_decisions": "sampled decisions whose reason is order_scheduled",
        "sampled_decisions": "the strategy's journaled decisions; rows skipped by the decision interval are counted",
        "scorable_predictions": "schedule rows with a prediction; its scored rows equal the study's"
      },
      "episodes": "from a fill that leaves a flat position to the fill that makes it flat again (completed, booked PnL = -sum n_i - sum f_i) or to the window's end (censored, marked at the final valuation)",
      "future_returns": "each cohort's scored items' label return_raw, the label artifact's unit: (future midpoint - current midpoint) / current midpoint in parts per billion, rounded half away from zero by the label generator; rows, exact sum, minimum, maximum, and exact mean, for the cohort, each predicted class, and each fixed confidence bin; items without a scored label are counted as not_scored. Retrospective only: no threshold, ranking, or choice of what to run uses them",
      "identities": [
        "booked_net_pnl = midpoint_inventory_pnl - execution_spread_cost - fees - terminal_bid_markdown + rounding_adjustment, checked exactly; when only M is missing, booked_net_pnl = inventory_pnl_at_bid - execution_spread_cost - fees + rounding_adjustment",
        "midpoint_inventory_pnl = signal_reference_pnl - observed_midpoint_movement, checked exactly"
      ],
      "joins": "events by ingest sequence within the run's own dataset and stream, and only top-of-book events of the run's instrument as quotes; a decision's signal by its schedule row's identity and that row's quote; an order by (account, decision); its one resolution by (account, client order ID)",
      "labels": "the evaluation recording's verified labels under the plan's scoring rule, read only to describe outcomes; they never reach a model, a policy, an order, or a choice of what to run",
      "not_computed": "no spliced equity curve, annualization, Sharpe ratio, significance test, threshold search, ranking, or winner",
      "pooling": "exact sums over independent reset accounts of the included folds, and with several studies over studies for scenarios with identical names, fees, and latencies; decision cohorts pool counts and then compute their ratios; a fold whose cell did not complete, or a study with no completed fold of the cell, is listed as missing and the pool is partial; a pool with no completed run has no financial figures (unavailable: no_completed_runs), and its counts are zero",
      "ratios": "accuracy = correct / scored; balanced accuracy = mean recall over the realized classes that occur; exact reduced fractions, with a 6-place decimal rounded half away from zero; undefined when nothing is scored",
      "scales": "prices 10^-9, quantities and money 10^-8; a midpoint can be half a price unit, so every attribution figure is an exact decimal of at most 18 places, with no exponent, trailing zeros, or negative zero",
      "symbols": "for fill i of an account: q_i its quantity, positive for a buy and negative for a sell; p_i its price; n_i its booked notional, signed as q_i; f_i its fee; m_i the (bid + ask) / 2 of its execution observation; s_i the (bid + ask) / 2 of its decision's signal quote. Q is the final position, B the final mark's bid, M that quote's (bid + ask) / 2, and V the booked final market value",
      "timing": "the configured latency, and per execution the observed durations from signal to arrival, arrival to execution, and signal to execution; expired orders have no execution time",
      "unavailable": "a figure that needs a value that does not exist is unavailable with its reason; nothing is substituted, not the ask, a trade, the cost basis, an earlier quote, or zero. M and B are needed only for an open final position"
    },
    "diagnostic_id": "ea7c2b639c877f0f26bc53150d97c6576c43309c887b79f2685cd4a4eab46404",
    "files": {
      "episodes": {
        "name": "episodes.ndjson",
        "rows": "42",
        "sha256": "545cfcdf3df4e9b5730cec8198fe49433caa701a964f06d77edf73bcec8faba1",
        "size_bytes": "24158"
      },
      "fills": {
        "name": "fills.ndjson",
        "rows": "78",
        "sha256": "cf87f58ddbb0c04991ee783723b750ca30a5f88e2f97f14667340798643e1066",
        "size_bytes": "95470"
      }
    },
    "generator": {"name": "chronos_research.strategy_diagnostics", "version": "2"},
    "request": {
      "schema": {"name": "chronos.strategy_diagnostics_request", "version": "1"},
      "study_ids": ["48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
    },
    "schema": {"name": "chronos.strategy_diagnostics", "version": "2"},
    "scope": "retrospective explanation only: how each recorded run's PnL and decisions arose in these recordings; nothing is fitted, scheduled, run, ranked, promoted, or deployed",
    "status": "complete",
    "studies": [
      {
        "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
        "evidence": "synthetic",
        "folds": [
          {
            "dataset_id": "d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a",
            "events_sha256": "42b87ee28997a472bdab0152a01741e54d219cc25aaa2fa1939b9d1c3d1de4d9",
            "feature_artifact_id": "f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5",
            "features_sha256": "192d9fee1a4eacfff9c1643143e1ad762c67a9537f93377e6df81417d16eb35d",
            "index": "1",
            "label_artifact_id": "78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6",
            "labels_sha256": "d24bea510b6a9f2d1c7de6cee350294e18ad1d6f6154bd1a7a39b90d983bf4ea",
            "models": {
              "logistic": {
                "predictions": {
                  "cohort": {
                    "accuracy": {"denominator": "494", "numerator": "213", "rounded": "0.431174"},
                    "balanced_accuracy": {"denominator": "199", "numerator": "71", "rounded": "0.356784"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "419", "numerator": "174", "rounded": "0.415274"},
                        "correct": "174",
                        "items": "425",
                        "name": "0.4-0.5",
                        "scored": "419"
                      },
                      {
                        "accuracy": {"denominator": "25", "numerator": "13", "rounded": "0.52"},
                        "correct": "39",
                        "items": "75",
                        "name": "0.5-0.6",
                        "scored": "75"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "160", "flat": "0", "up": "39"},
                      "flat": {"down": "72", "flat": "0", "up": "24"},
                      "up": {"down": "146", "flat": "0", "up": "53"}
                    },
                    "correct": "213",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "600601"},
                        "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                        "minimum_raw": {"value": "-500350"},
                        "rows": "494",
                        "sum_raw": "-2090231"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "419", "numerator": "3207480", "rounded": "7655.083532"},
                          "minimum_raw": {"value": "-500350"},
                          "name": "0.4-0.5",
                          "rows": "419",
                          "sum_raw": "3207480"
                        },
                        {
                          "maximum_raw": {"value": "400400"},
                          "mean_raw": {"denominator": "75", "numerator": "-5297711", "rounded": "-70636.146667"},
                          "minimum_raw": {"value": "-500150"},
                          "name": "0.5-0.6",
                          "rows": "75",
                          "sum_raw": "-5297711"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "126", "numerator": "-1963877", "rounded": "-15586.325397"},
                          "minimum_raw": {"value": "-500150"},
                          "rows": "378",
                          "sum_raw": "-5891631"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "500450"},
                          "mean_raw": {"denominator": "29", "numerator": "950350", "rounded": "32770.689655"},
                          "minimum_raw": {"value": "-500350"},
                          "rows": "116",
                          "sum_raw": "3801400"
                        }
                      },
                      "not_scored": "6"
                    },
                    "items": "500",
                    "labels": {"label_unavailable:no_future_observation": "6", "scored": "494"},
                    "predicted": {"down": "380", "flat": "0", "up": "120"},
                    "realized": {"down": "199", "flat": "96", "up": "199"},
                    "reasons": {},
                    "scored": "494"
                  },
                  "feature_unavailable": {},
                  "scheduled": "500"
                },
                "schedule_id": "106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9",
                "schedule_sha256": "5333428e0b9732d507c4f23c1d0ae8d6ed6bf1e5adbe58abc9ff9ad3bfeb813b",
                "state": "completed",
                "study_scores": "agree"
              },
              "majority": {
                "predictions": {
                  "cohort": {
                    "accuracy": {"denominator": "494", "numerator": "199", "rounded": "0.402834"},
                    "balanced_accuracy": {"denominator": "3", "numerator": "1", "rounded": "0.333333"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "494", "numerator": "199", "rounded": "0.402834"},
                        "correct": "199",
                        "items": "500",
                        "name": "0.4-0.5",
                        "scored": "494"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "199", "flat": "0", "up": "0"},
                      "flat": {"down": "96", "flat": "0", "up": "0"},
                      "up": {"down": "199", "flat": "0", "up": "0"}
                    },
                    "correct": "199",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "600601"},
                        "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                        "minimum_raw": {"value": "-500350"},
                        "rows": "494",
                        "sum_raw": "-2090231"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                          "minimum_raw": {"value": "-500350"},
                          "name": "0.4-0.5",
                          "rows": "494",
                          "sum_raw": "-2090231"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                          "minimum_raw": {"value": "-500350"},
                          "rows": "494",
                          "sum_raw": "-2090231"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "6"
                    },
                    "items": "500",
                    "labels": {"label_unavailable:no_future_observation": "6", "scored": "494"},
                    "predicted": {"down": "500", "flat": "0", "up": "0"},
                    "realized": {"down": "199", "flat": "96", "up": "199"},
                    "reasons": {},
                    "scored": "494"
                  },
                  "feature_unavailable": {},
                  "scheduled": "500"
                },
                "schedule_id": "79c7c2f9c60ddbe467a36235194ae84daeb9337111803c678b9650d6e610b184",
                "schedule_sha256": "6fa31341ad7f0174360679acd44c8e58acfe7c345231a4e5de053f23a9719355",
                "state": "completed",
                "study_scores": "agree"
              }
            },
            "runs": [
              {
                "accounts": {
                  "buy_and_hold": {
                    "booked": {
                      "cash": "9899.98",
                      "fees_paid": "0.01",
                      "final_quantity": "1",
                      "starting_cash": "10000",
                      "valuation": {
                        "equity": "9999.91",
                        "market_value": "99.93",
                        "net_total_pnl": "-0.09",
                        "return_percent": "-0.0009"
                      }
                    },
                    "components": {
                      "booked_net_pnl": {"value": "-0.09"},
                      "execution_spread_cost": {"value": "0.01"},
                      "fees": {"value": "0.01"},
                      "inventory_pnl_at_bid": {"value": "-0.07"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "-0.06"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0.01"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "-0.05"},
                      "terminal_bid_markdown": {"value": "0.01"}
                    },
                    "episodes": {
                      "censored": "1",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "1",
                    "final_mark": {
                      "ask": "99.95",
                      "bid": "99.93",
                      "ingest_sequence": "998",
                      "midpoint": {"value": "99.94"},
                      "receive_at": "2026-09-23T13:30:00.210734812Z"
                    },
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "1",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "1"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"},
                      "signal": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"maximum": "76019", "minimum": "76019", "sum": "76019"},
                      "configured_latency_ns": "1000",
                      "executions": "1",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"maximum": "1000", "minimum": "1000", "sum": "1000"},
                      "signal_to_execution_duration_ns": {"maximum": "77019", "minimum": "77019", "sum": "77019"}
                    }
                  },
                  "cash": {
                    "booked": {
                      "cash": "10000",
                      "fees_paid": "0",
                      "final_quantity": "0",
                      "starting_cash": "10000",
                      "valuation": {"equity": "10000", "market_value": "0", "net_total_pnl": "0", "return_percent": "0"}
                    },
                    "components": {
                      "booked_net_pnl": {"value": "0"},
                      "execution_spread_cost": {"value": "0"},
                      "fees": {"value": "0"},
                      "inventory_pnl_at_bid": {"value": "0"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "0"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0"},
                      "terminal_bid_markdown": {"value": "0"}
                    },
                    "episodes": {
                      "censored": "0",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "0",
                    "final_mark": null,
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "0",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "0"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"unavailable": "no_fills_with_both_sides"},
                      "signal": {"unavailable": "no_fills_with_both_sides"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"unavailable": "no_executions"},
                      "configured_latency_ns": "1000",
                      "executions": "0",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"unavailable": "no_executions"},
                      "signal_to_execution_duration_ns": {"unavailable": "no_executions"}
                    }
                  },
                  "strategy": {
                    "booked": {
                      "cash": "10000",
                      "fees_paid": "0",
                      "final_quantity": "0",
                      "starting_cash": "10000",
                      "valuation": {"equity": "10000", "market_value": "0", "net_total_pnl": "0", "return_percent": "0"}
                    },
                    "components": {
                      "booked_net_pnl": {"value": "0"},
                      "execution_spread_cost": {"value": "0"},
                      "fees": {"value": "0"},
                      "inventory_pnl_at_bid": {"value": "0"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "0"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0"},
                      "terminal_bid_markdown": {"value": "0"}
                    },
                    "episodes": {
                      "censored": "0",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "0",
                    "final_mark": null,
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "0",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "0"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"unavailable": "no_fills_with_both_sides"},
                      "signal": {"unavailable": "no_fills_with_both_sides"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"unavailable": "no_executions"},
                      "configured_latency_ns": "1000",
                      "executions": "0",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"unavailable": "no_executions"},
                      "signal_to_execution_duration_ns": {"unavailable": "no_executions"}
                    }
                  }
                },
                "decisions": {
                  "cohorts": {
                    "filled_orders": {
                      "accuracy": {"undefined": "no_scored_items"},
                      "balanced_accuracy": {"undefined": "no_scored_items"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.4-0.5",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "0", "flat": "0", "up": "0"},
                        "flat": {"down": "0", "flat": "0", "up": "0"},
                        "up": {"down": "0", "flat": "0", "up": "0"}
                      },
                      "correct": "0",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.4-0.5",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        },
                        "not_scored": "0"
                      },
                      "items": "0",
                      "labels": {},
                      "predicted": {"down": "0", "flat": "0", "up": "0"},
                      "realized": {"down": "0", "flat": "0", "up": "0"},
                      "reasons": {},
                      "scored": "0"
                    },
                    "order_scheduling_decisions": {
                      "accuracy": {"undefined": "no_scored_items"},
                      "balanced_accuracy": {"undefined": "no_scored_items"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.4-0.5",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "0", "flat": "0", "up": "0"},
                        "flat": {"down": "0", "flat": "0", "up": "0"},
                        "up": {"down": "0", "flat": "0", "up": "0"}
                      },
                      "correct": "0",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.4-0.5",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        },
                        "not_scored": "0"
                      },
                      "items": "0",
                      "labels": {},
                      "predicted": {"down": "0", "flat": "0", "up": "0"},
                      "realized": {"down": "0", "flat": "0", "up": "0"},
                      "reasons": {},
                      "scored": "0"
                    },
                    "sampled_decisions": {
                      "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                      "balanced_accuracy": {"denominator": "3", "numerator": "1", "rounded": "0.333333"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                          "correct": "43",
                          "items": "99",
                          "name": "0.4-0.5",
                          "scored": "98"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "43", "flat": "0", "up": "0"},
                        "flat": {"down": "15", "flat": "0", "up": "0"},
                        "up": {"down": "40", "flat": "0", "up": "0"}
                      },
                      "correct": "43",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                            "minimum_raw": {"value": "-400160"},
                            "name": "0.4-0.5",
                            "rows": "98",
                            "sum_raw": "-798199"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                            "minimum_raw": {"value": "-400160"},
                            "rows": "98",
                            "sum_raw": "-798199"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "99",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                      "predicted": {"down": "99", "flat": "0", "up": "0"},
                      "realized": {"down": "43", "flat": "15", "up": "40"},
                      "reasons": {"already_at_target": "99"},
                      "scored": "98"
                    }
                  },
                  "evaluated": "99",
                  "journaled": {
                    "already_at_target": "99",
                    "hold_below_threshold": "0",
                    "hold_flat_prediction": "0",
                    "order_pending": "0",
                    "order_scheduled": "0"
                  },
                  "skipped_decision_interval": "401",
                  "skipped_prediction_unavailable": "0"
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "0.09"},
                    "execution_spread_cost": {"value": "-0.01"},
                    "fees": {"value": "-0.01"},
                    "inventory_pnl_at_bid": {"value": "0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "-0.01"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.05"},
                    "terminal_bid_markdown": {"value": "-0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  }
                },
                "engine_replay": {
                  "recorded_by": "study",
                  "statement": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "fee_per_fill": "0.01",
                "fold": "1",
                "journal_sha256": "69451d713f63aa0bb78174611f8042a68a455e6db4eb08906c6be3c89cb21b14",
                "latency_ns": "1000",
                "model": "majority",
                "run_id": "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f",
                "scenario": "base",
                "state": "completed"
              },
              {
                "accounts": {
                  "buy_and_hold": {
                    "booked": {
                      "cash": "9899.95",
                      "fees_paid": "0.05",
                      "final_quantity": "1",
                      "starting_cash": "10000",
                      "valuation": {
                        "equity": "9999.88",
                        "market_value": "99.93",
                        "net_total_pnl": "-0.12",
                        "return_percent": "-0.0012"
                      }
                    },
                    "components": {
                      "booked_net_pnl": {"value": "-0.12"},
                      "execution_spread_cost": {"value": "0.01"},
                      "fees": {"value": "0.05"},
                      "inventory_pnl_at_bid": {"value": "-0.06"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "-0.05"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "-0.05"},
                      "terminal_bid_markdown": {"value": "0.01"}
                    },
                    "episodes": {
                      "censored": "1",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "1",
                    "final_mark": {
                      "ask": "99.95",
                      "bid": "99.93",
                      "ingest_sequence": "998",
                      "midpoint": {"value": "99.94"},
                      "receive_at": "2026-09-23T13:30:00.210734812Z"
                    },
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "1",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "1"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"},
                      "signal": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"maximum": "271700", "minimum": "271700", "sum": "271700"},
                      "configured_latency_ns": "200000",
                      "executions": "1",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"maximum": "200000", "minimum": "200000", "sum": "200000"},
                      "signal_to_execution_duration_ns": {"maximum": "471700", "minimum": "471700", "sum": "471700"}
                    }
                  },
                  "cash": {
                    "booked": {
                      "cash": "10000",
                      "fees_paid": "0",
                      "final_quantity": "0",
                      "starting_cash": "10000",
                      "valuation": {"equity": "10000", "market_value": "0", "net_total_pnl": "0", "return_percent": "0"}
                    },
                    "components": {
                      "booked_net_pnl": {"value": "0"},
                      "execution_spread_cost": {"value": "0"},
                      "fees": {"value": "0"},
                      "inventory_pnl_at_bid": {"value": "0"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "0"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0"},
                      "terminal_bid_markdown": {"value": "0"}
                    },
                    "episodes": {
                      "censored": "0",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "0",
                    "final_mark": null,
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "0",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "0"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"unavailable": "no_fills_with_both_sides"},
                      "signal": {"unavailable": "no_fills_with_both_sides"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"unavailable": "no_executions"},
                      "configured_latency_ns": "200000",
                      "executions": "0",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"unavailable": "no_executions"},
                      "signal_to_execution_duration_ns": {"unavailable": "no_executions"}
                    }
                  },
                  "strategy": {
                    "booked": {
                      "cash": "10000",
                      "fees_paid": "0",
                      "final_quantity": "0",
                      "starting_cash": "10000",
                      "valuation": {"equity": "10000", "market_value": "0", "net_total_pnl": "0", "return_percent": "0"}
                    },
                    "components": {
                      "booked_net_pnl": {"value": "0"},
                      "execution_spread_cost": {"value": "0"},
                      "fees": {"value": "0"},
                      "inventory_pnl_at_bid": {"value": "0"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "0"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0"},
                      "terminal_bid_markdown": {"value": "0"}
                    },
                    "episodes": {
                      "censored": "0",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "0",
                    "final_mark": null,
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "0",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "0"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"unavailable": "no_fills_with_both_sides"},
                      "signal": {"unavailable": "no_fills_with_both_sides"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"unavailable": "no_executions"},
                      "configured_latency_ns": "200000",
                      "executions": "0",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"unavailable": "no_executions"},
                      "signal_to_execution_duration_ns": {"unavailable": "no_executions"}
                    }
                  }
                },
                "decisions": {
                  "cohorts": {
                    "filled_orders": {
                      "accuracy": {"undefined": "no_scored_items"},
                      "balanced_accuracy": {"undefined": "no_scored_items"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.4-0.5",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "0", "flat": "0", "up": "0"},
                        "flat": {"down": "0", "flat": "0", "up": "0"},
                        "up": {"down": "0", "flat": "0", "up": "0"}
                      },
                      "correct": "0",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.4-0.5",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        },
                        "not_scored": "0"
                      },
                      "items": "0",
                      "labels": {},
                      "predicted": {"down": "0", "flat": "0", "up": "0"},
                      "realized": {"down": "0", "flat": "0", "up": "0"},
                      "reasons": {},
                      "scored": "0"
                    },
                    "order_scheduling_decisions": {
                      "accuracy": {"undefined": "no_scored_items"},
                      "balanced_accuracy": {"undefined": "no_scored_items"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.4-0.5",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "0", "flat": "0", "up": "0"},
                        "flat": {"down": "0", "flat": "0", "up": "0"},
                        "up": {"down": "0", "flat": "0", "up": "0"}
                      },
                      "correct": "0",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.4-0.5",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        },
                        "not_scored": "0"
                      },
                      "items": "0",
                      "labels": {},
                      "predicted": {"down": "0", "flat": "0", "up": "0"},
                      "realized": {"down": "0", "flat": "0", "up": "0"},
                      "reasons": {},
                      "scored": "0"
                    },
                    "sampled_decisions": {
                      "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                      "balanced_accuracy": {"denominator": "3", "numerator": "1", "rounded": "0.333333"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                          "correct": "43",
                          "items": "99",
                          "name": "0.4-0.5",
                          "scored": "98"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "43", "flat": "0", "up": "0"},
                        "flat": {"down": "15", "flat": "0", "up": "0"},
                        "up": {"down": "40", "flat": "0", "up": "0"}
                      },
                      "correct": "43",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                            "minimum_raw": {"value": "-400160"},
                            "name": "0.4-0.5",
                            "rows": "98",
                            "sum_raw": "-798199"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                            "minimum_raw": {"value": "-400160"},
                            "rows": "98",
                            "sum_raw": "-798199"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "99",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                      "predicted": {"down": "99", "flat": "0", "up": "0"},
                      "realized": {"down": "43", "flat": "15", "up": "40"},
                      "reasons": {"already_at_target": "99"},
                      "scored": "98"
                    }
                  },
                  "evaluated": "99",
                  "journaled": {
                    "already_at_target": "99",
                    "hold_below_threshold": "0",
                    "hold_flat_prediction": "0",
                    "order_pending": "0",
                    "order_scheduled": "0"
                  },
                  "skipped_decision_interval": "401",
                  "skipped_prediction_unavailable": "0"
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "0.12"},
                    "execution_spread_cost": {"value": "-0.01"},
                    "fees": {"value": "-0.05"},
                    "inventory_pnl_at_bid": {"value": "0.06"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0.05"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.05"},
                    "terminal_bid_markdown": {"value": "-0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  }
                },
                "engine_replay": {
                  "recorded_by": "study",
                  "statement": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "fee_per_fill": "0.05",
                "fold": "1",
                "journal_sha256": "bbd6839948387cc48b1d45baa2ff467577732b5e60fc437dcd1b3e61657d10ed",
                "latency_ns": "200000",
                "model": "majority",
                "run_id": "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765",
                "scenario": "stress",
                "state": "completed"
              },
              {
                "accounts": {
                  "buy_and_hold": {
                    "booked": {
                      "cash": "9899.98",
                      "fees_paid": "0.01",
                      "final_quantity": "1",
                      "starting_cash": "10000",
                      "valuation": {
                        "equity": "9999.91",
                        "market_value": "99.93",
                        "net_total_pnl": "-0.09",
                        "return_percent": "-0.0009"
                      }
                    },
                    "components": {
                      "booked_net_pnl": {"value": "-0.09"},
                      "execution_spread_cost": {"value": "0.01"},
                      "fees": {"value": "0.01"},
                      "inventory_pnl_at_bid": {"value": "-0.07"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "-0.06"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0.01"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "-0.05"},
                      "terminal_bid_markdown": {"value": "0.01"}
                    },
                    "episodes": {
                      "censored": "1",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "1",
                    "final_mark": {
                      "ask": "99.95",
                      "bid": "99.93",
                      "ingest_sequence": "998",
                      "midpoint": {"value": "99.94"},
                      "receive_at": "2026-09-23T13:30:00.210734812Z"
                    },
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "1",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "1"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"},
                      "signal": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"maximum": "76019", "minimum": "76019", "sum": "76019"},
                      "configured_latency_ns": "1000",
                      "executions": "1",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"maximum": "1000", "minimum": "1000", "sum": "1000"},
                      "signal_to_execution_duration_ns": {"maximum": "77019", "minimum": "77019", "sum": "77019"}
                    }
                  },
                  "cash": {
                    "booked": {
                      "cash": "10000",
                      "fees_paid": "0",
                      "final_quantity": "0",
                      "starting_cash": "10000",
                      "valuation": {"equity": "10000", "market_value": "0", "net_total_pnl": "0", "return_percent": "0"}
                    },
                    "components": {
                      "booked_net_pnl": {"value": "0"},
                      "execution_spread_cost": {"value": "0"},
                      "fees": {"value": "0"},
                      "inventory_pnl_at_bid": {"value": "0"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "0"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0"},
                      "terminal_bid_markdown": {"value": "0"}
                    },
                    "episodes": {
                      "censored": "0",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "0",
                    "final_mark": null,
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "0",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "0"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"unavailable": "no_fills_with_both_sides"},
                      "signal": {"unavailable": "no_fills_with_both_sides"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"unavailable": "no_executions"},
                      "configured_latency_ns": "1000",
                      "executions": "0",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"unavailable": "no_executions"},
                      "signal_to_execution_duration_ns": {"unavailable": "no_executions"}
                    }
                  },
                  "strategy": {
                    "booked": {
                      "cash": "9899.26",
                      "fees_paid": "0.37",
                      "final_quantity": "1",
                      "starting_cash": "10000",
                      "valuation": {
                        "equity": "9999.19",
                        "market_value": "99.93",
                        "net_total_pnl": "-0.81",
                        "return_percent": "-0.0081"
                      }
                    },
                    "components": {
                      "booked_net_pnl": {"value": "-0.81"},
                      "execution_spread_cost": {"value": "0.37"},
                      "fees": {"value": "0.37"},
                      "inventory_pnl_at_bid": {"value": "-0.07"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "-0.06"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0.08"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0.02"},
                      "terminal_bid_markdown": {"value": "0.01"}
                    },
                    "episodes": {
                      "censored": "1",
                      "completed": "18",
                      "completed_duration_ns": {"maximum": "2022798", "minimum": "1041497", "sum": "22624666"},
                      "completed_pnl": {"value": "-0.8"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "18"
                    },
                    "fills": "37",
                    "final_mark": {
                      "ask": "99.95",
                      "bid": "99.93",
                      "ingest_sequence": "998",
                      "midpoint": {"value": "99.94"},
                      "receive_at": "2026-09-23T13:30:00.210734812Z"
                    },
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "37",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "37"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"fills": "37", "maximum": "0.02", "minimum": "0.02", "sum": "0.74", "without_both_sides": "0"},
                      "signal": {"fills": "37", "maximum": "0.02", "minimum": "0.02", "sum": "0.74", "without_both_sides": "0"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"maximum": "394145", "minimum": "58694", "sum": "8166838"},
                      "configured_latency_ns": "1000",
                      "executions": "37",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"maximum": "1000", "minimum": "1000", "sum": "37000"},
                      "signal_to_execution_duration_ns": {"maximum": "395145", "minimum": "59694", "sum": "8203838"}
                    }
                  }
                },
                "decisions": {
                  "cohorts": {
                    "filled_orders": {
                      "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                      "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                          "correct": "16",
                          "items": "37",
                          "name": "0.4-0.5",
                          "scored": "36"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "9", "flat": "0", "up": "7"},
                        "flat": {"down": "2", "flat": "0", "up": "4"},
                        "up": {"down": "7", "flat": "0", "up": "7"}
                      },
                      "correct": "16",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                            "minimum_raw": {"value": "-399800"},
                            "name": "0.4-0.5",
                            "rows": "36",
                            "sum_raw": "-698760"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                            "minimum_raw": {"value": "-399800"},
                            "rows": "18",
                            "sum_raw": "-499100"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"value": "200220"},
                            "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                            "minimum_raw": {"value": "-200140"},
                            "rows": "18",
                            "sum_raw": "-199660"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "37",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                      "predicted": {"down": "18", "flat": "0", "up": "19"},
                      "realized": {"down": "16", "flat": "6", "up": "14"},
                      "reasons": {"filled": "37"},
                      "scored": "36"
                    },
                    "order_scheduling_decisions": {
                      "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                      "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                          "correct": "16",
                          "items": "37",
                          "name": "0.4-0.5",
                          "scored": "36"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "9", "flat": "0", "up": "7"},
                        "flat": {"down": "2", "flat": "0", "up": "4"},
                        "up": {"down": "7", "flat": "0", "up": "7"}
                      },
                      "correct": "16",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                            "minimum_raw": {"value": "-399800"},
                            "name": "0.4-0.5",
                            "rows": "36",
                            "sum_raw": "-698760"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                            "minimum_raw": {"value": "-399800"},
                            "rows": "18",
                            "sum_raw": "-499100"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"value": "200220"},
                            "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                            "minimum_raw": {"value": "-200140"},
                            "rows": "18",
                            "sum_raw": "-199660"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "37",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                      "predicted": {"down": "18", "flat": "0", "up": "19"},
                      "realized": {"down": "16", "flat": "6", "up": "14"},
                      "reasons": {"filled": "37"},
                      "scored": "36"
                    },
                    "sampled_decisions": {
                      "accuracy": {"denominator": "49", "numerator": "22", "rounded": "0.44898"},
                      "balanced_accuracy": {"denominator": "645", "numerator": "223", "rounded": "0.345736"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "86", "numerator": "39", "rounded": "0.453488"},
                          "correct": "39",
                          "items": "87",
                          "name": "0.4-0.5",
                          "scored": "86"
                        },
                        {
                          "accuracy": {"denominator": "12", "numerator": "5", "rounded": "0.416667"},
                          "correct": "5",
                          "items": "12",
                          "name": "0.5-0.6",
                          "scored": "12"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "36", "flat": "0", "up": "7"},
                        "flat": {"down": "11", "flat": "0", "up": "4"},
                        "up": {"down": "32", "flat": "0", "up": "8"}
                      },
                      "correct": "44",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "86", "numerator": "-98329", "rounded": "-1143.360465"},
                            "minimum_raw": {"value": "-400160"},
                            "name": "0.4-0.5",
                            "rows": "86",
                            "sum_raw": "-98329"
                          },
                          {
                            "maximum_raw": {"value": "199880"},
                            "mean_raw": {"denominator": "2", "numerator": "-116645", "rounded": "-58322.5"},
                            "minimum_raw": {"value": "-400120"},
                            "name": "0.5-0.6",
                            "rows": "12",
                            "sum_raw": "-699870"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "79", "numerator": "-698549", "rounded": "-8842.392405"},
                            "minimum_raw": {"value": "-400160"},
                            "rows": "79",
                            "sum_raw": "-698549"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"value": "200220"},
                            "mean_raw": {"denominator": "19", "numerator": "-99650", "rounded": "-5244.736842"},
                            "minimum_raw": {"value": "-200140"},
                            "rows": "19",
                            "sum_raw": "-99650"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "99",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                      "predicted": {"down": "79", "flat": "0", "up": "20"},
                      "realized": {"down": "43", "flat": "15", "up": "40"},
                      "reasons": {"already_at_target": "62", "order_scheduled": "37"},
                      "scored": "98"
                    }
                  },
                  "evaluated": "99",
                  "journaled": {
                    "already_at_target": "62",
                    "hold_below_threshold": "0",
                    "hold_flat_prediction": "0",
                    "order_pending": "0",
                    "order_scheduled": "37"
                  },
                  "skipped_decision_interval": "401",
                  "skipped_prediction_unavailable": "0"
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-0.72"},
                    "execution_spread_cost": {"value": "0.36"},
                    "fees": {"value": "0.36"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.07"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.07"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "-0.81"},
                    "execution_spread_cost": {"value": "0.37"},
                    "fees": {"value": "0.37"},
                    "inventory_pnl_at_bid": {"value": "-0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.08"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.02"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  }
                },
                "engine_replay": {
                  "recorded_by": "study",
                  "statement": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "fee_per_fill": "0.01",
                "fold": "1",
                "journal_sha256": "5aaf9b377200ebbdaf9971f60fabe77d52312a18f086189e67821f958d4bad1d",
                "latency_ns": "1000",
                "model": "logistic",
                "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
                "scenario": "base",
                "state": "completed"
              },
              {
                "accounts": {
                  "buy_and_hold": {
                    "booked": {
                      "cash": "9899.95",
                      "fees_paid": "0.05",
                      "final_quantity": "1",
                      "starting_cash": "10000",
                      "valuation": {
                        "equity": "9999.88",
                        "market_value": "99.93",
                        "net_total_pnl": "-0.12",
                        "return_percent": "-0.0012"
                      }
                    },
                    "components": {
                      "booked_net_pnl": {"value": "-0.12"},
                      "execution_spread_cost": {"value": "0.01"},
                      "fees": {"value": "0.05"},
                      "inventory_pnl_at_bid": {"value": "-0.06"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "-0.05"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "-0.05"},
                      "terminal_bid_markdown": {"value": "0.01"}
                    },
                    "episodes": {
                      "censored": "1",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "1",
                    "final_mark": {
                      "ask": "99.95",
                      "bid": "99.93",
                      "ingest_sequence": "998",
                      "midpoint": {"value": "99.94"},
                      "receive_at": "2026-09-23T13:30:00.210734812Z"
                    },
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "1",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "1"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"},
                      "signal": {"fills": "1", "maximum": "0.02", "minimum": "0.02", "sum": "0.02", "without_both_sides": "0"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"maximum": "271700", "minimum": "271700", "sum": "271700"},
                      "configured_latency_ns": "200000",
                      "executions": "1",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"maximum": "200000", "minimum": "200000", "sum": "200000"},
                      "signal_to_execution_duration_ns": {"maximum": "471700", "minimum": "471700", "sum": "471700"}
                    }
                  },
                  "cash": {
                    "booked": {
                      "cash": "10000",
                      "fees_paid": "0",
                      "final_quantity": "0",
                      "starting_cash": "10000",
                      "valuation": {"equity": "10000", "market_value": "0", "net_total_pnl": "0", "return_percent": "0"}
                    },
                    "components": {
                      "booked_net_pnl": {"value": "0"},
                      "execution_spread_cost": {"value": "0"},
                      "fees": {"value": "0"},
                      "inventory_pnl_at_bid": {"value": "0"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "0"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0"},
                      "terminal_bid_markdown": {"value": "0"}
                    },
                    "episodes": {
                      "censored": "0",
                      "completed": "0",
                      "completed_duration_ns": {"unavailable": "no_completed_episodes"},
                      "completed_pnl": {"value": "0"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "0"
                    },
                    "fills": "0",
                    "final_mark": null,
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "0",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "0"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"unavailable": "no_fills_with_both_sides"},
                      "signal": {"unavailable": "no_fills_with_both_sides"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"unavailable": "no_executions"},
                      "configured_latency_ns": "200000",
                      "executions": "0",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"unavailable": "no_executions"},
                      "signal_to_execution_duration_ns": {"unavailable": "no_executions"}
                    }
                  },
                  "strategy": {
                    "booked": {
                      "cash": "9897.8",
                      "fees_paid": "1.85",
                      "final_quantity": "1",
                      "starting_cash": "10000",
                      "valuation": {
                        "equity": "9997.73",
                        "market_value": "99.93",
                        "net_total_pnl": "-2.27",
                        "return_percent": "-0.0227"
                      }
                    },
                    "components": {
                      "booked_net_pnl": {"value": "-2.27"},
                      "execution_spread_cost": {"value": "0.37"},
                      "fees": {"value": "1.85"},
                      "inventory_pnl_at_bid": {"value": "-0.05"},
                      "market_value_rounding": {"value": "0"},
                      "midpoint_inventory_pnl": {"value": "-0.04"},
                      "notional_rounding": {"value": "0"},
                      "observed_midpoint_movement": {"value": "0.06"},
                      "rounding_adjustment": {"value": "0"},
                      "signal_reference_pnl": {"value": "0.02"},
                      "terminal_bid_markdown": {"value": "0.01"}
                    },
                    "episodes": {
                      "censored": "1",
                      "completed": "18",
                      "completed_duration_ns": {"maximum": "2377939", "minimum": "868451", "sum": "21529580"},
                      "completed_pnl": {"value": "-2.22"},
                      "completed_with_gain": "0",
                      "completed_with_loss": "18"
                    },
                    "fills": "37",
                    "final_mark": {
                      "ask": "99.95",
                      "bid": "99.93",
                      "ingest_sequence": "998",
                      "midpoint": {"value": "99.94"},
                      "receive_at": "2026-09-23T13:30:00.210734812Z"
                    },
                    "orders": {
                      "cancellation_reasons": {},
                      "outcomes": {
                        "cancelled": "0",
                        "expired": "0",
                        "filled": "37",
                        "partially_filled_and_cancelled": "0",
                        "rejected": "0",
                        "scheduled": "37"
                      },
                      "rejections": {}
                    },
                    "quoted_spreads": {
                      "execution": {"fills": "37", "maximum": "0.02", "minimum": "0.02", "sum": "0.74", "without_both_sides": "0"},
                      "signal": {"fills": "37", "maximum": "0.02", "minimum": "0.02", "sum": "0.74", "without_both_sides": "0"}
                    },
                    "reconciliation": {"booked_net_pnl": {"state": "reconciled"}, "signal_reference": {"state": "reconciled"}},
                    "timing": {
                      "arrival_to_execution_duration_ns": {"maximum": "361757", "minimum": "1059", "sum": "5538718"},
                      "configured_latency_ns": "200000",
                      "executions": "37",
                      "expired": "0",
                      "signal_to_arrival_duration_ns": {"maximum": "200000", "minimum": "200000", "sum": "7400000"},
                      "signal_to_execution_duration_ns": {"maximum": "561757", "minimum": "201059", "sum": "12938718"}
                    }
                  }
                },
                "decisions": {
                  "cohorts": {
                    "filled_orders": {
                      "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                      "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                          "correct": "16",
                          "items": "37",
                          "name": "0.4-0.5",
                          "scored": "36"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "9", "flat": "0", "up": "7"},
                        "flat": {"down": "2", "flat": "0", "up": "4"},
                        "up": {"down": "7", "flat": "0", "up": "7"}
                      },
                      "correct": "16",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                            "minimum_raw": {"value": "-399800"},
                            "name": "0.4-0.5",
                            "rows": "36",
                            "sum_raw": "-698760"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                            "minimum_raw": {"value": "-399800"},
                            "rows": "18",
                            "sum_raw": "-499100"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"value": "200220"},
                            "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                            "minimum_raw": {"value": "-200140"},
                            "rows": "18",
                            "sum_raw": "-199660"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "37",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                      "predicted": {"down": "18", "flat": "0", "up": "19"},
                      "realized": {"down": "16", "flat": "6", "up": "14"},
                      "reasons": {"filled": "37"},
                      "scored": "36"
                    },
                    "order_scheduling_decisions": {
                      "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                      "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                          "correct": "16",
                          "items": "37",
                          "name": "0.4-0.5",
                          "scored": "36"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.5-0.6",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "9", "flat": "0", "up": "7"},
                        "flat": {"down": "2", "flat": "0", "up": "4"},
                        "up": {"down": "7", "flat": "0", "up": "7"}
                      },
                      "correct": "16",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                            "minimum_raw": {"value": "-399800"},
                            "name": "0.4-0.5",
                            "rows": "36",
                            "sum_raw": "-698760"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.5-0.6",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "399920"},
                            "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                            "minimum_raw": {"value": "-399800"},
                            "rows": "18",
                            "sum_raw": "-499100"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"value": "200220"},
                            "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                            "minimum_raw": {"value": "-200140"},
                            "rows": "18",
                            "sum_raw": "-199660"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "37",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                      "predicted": {"down": "18", "flat": "0", "up": "19"},
                      "realized": {"down": "16", "flat": "6", "up": "14"},
                      "reasons": {"filled": "37"},
                      "scored": "36"
                    },
                    "sampled_decisions": {
                      "accuracy": {"denominator": "49", "numerator": "22", "rounded": "0.44898"},
                      "balanced_accuracy": {"denominator": "645", "numerator": "223", "rounded": "0.345736"},
                      "confidence_bins": [
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.0-0.1",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.1-0.2",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.2-0.3",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.3-0.4",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"denominator": "86", "numerator": "39", "rounded": "0.453488"},
                          "correct": "39",
                          "items": "87",
                          "name": "0.4-0.5",
                          "scored": "86"
                        },
                        {
                          "accuracy": {"denominator": "12", "numerator": "5", "rounded": "0.416667"},
                          "correct": "5",
                          "items": "12",
                          "name": "0.5-0.6",
                          "scored": "12"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.6-0.7",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.7-0.8",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.8-0.9",
                          "scored": "0"
                        },
                        {
                          "accuracy": {"undefined": "no_scored_items"},
                          "correct": "0",
                          "items": "0",
                          "name": "0.9-1.0",
                          "scored": "0"
                        }
                      ],
                      "confusion": {
                        "down": {"down": "36", "flat": "0", "up": "7"},
                        "flat": {"down": "11", "flat": "0", "up": "4"},
                        "up": {"down": "32", "flat": "0", "up": "8"}
                      },
                      "correct": "44",
                      "future_returns": {
                        "all": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        "by_confidence_bin": [
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.0-0.1",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.1-0.2",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.2-0.3",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.3-0.4",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "86", "numerator": "-98329", "rounded": "-1143.360465"},
                            "minimum_raw": {"value": "-400160"},
                            "name": "0.4-0.5",
                            "rows": "86",
                            "sum_raw": "-98329"
                          },
                          {
                            "maximum_raw": {"value": "199880"},
                            "mean_raw": {"denominator": "2", "numerator": "-116645", "rounded": "-58322.5"},
                            "minimum_raw": {"value": "-400120"},
                            "name": "0.5-0.6",
                            "rows": "12",
                            "sum_raw": "-699870"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.6-0.7",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.7-0.8",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.8-0.9",
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "name": "0.9-1.0",
                            "rows": "0",
                            "sum_raw": "0"
                          }
                        ],
                        "by_predicted_class": {
                          "down": {
                            "maximum_raw": {"value": "600601"},
                            "mean_raw": {"denominator": "79", "numerator": "-698549", "rounded": "-8842.392405"},
                            "minimum_raw": {"value": "-400160"},
                            "rows": "79",
                            "sum_raw": "-698549"
                          },
                          "flat": {
                            "maximum_raw": {"undefined": "no_scored_items"},
                            "mean_raw": {"undefined": "no_scored_items"},
                            "minimum_raw": {"undefined": "no_scored_items"},
                            "rows": "0",
                            "sum_raw": "0"
                          },
                          "up": {
                            "maximum_raw": {"value": "200220"},
                            "mean_raw": {"denominator": "19", "numerator": "-99650", "rounded": "-5244.736842"},
                            "minimum_raw": {"value": "-200140"},
                            "rows": "19",
                            "sum_raw": "-99650"
                          }
                        },
                        "not_scored": "1"
                      },
                      "items": "99",
                      "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                      "predicted": {"down": "79", "flat": "0", "up": "20"},
                      "realized": {"down": "43", "flat": "15", "up": "40"},
                      "reasons": {"already_at_target": "62", "order_scheduled": "37"},
                      "scored": "98"
                    }
                  },
                  "evaluated": "99",
                  "journaled": {
                    "already_at_target": "62",
                    "hold_below_threshold": "0",
                    "hold_flat_prediction": "0",
                    "order_pending": "0",
                    "order_scheduled": "37"
                  },
                  "skipped_decision_interval": "401",
                  "skipped_prediction_unavailable": "0"
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-2.15"},
                    "execution_spread_cost": {"value": "0.36"},
                    "fees": {"value": "1.8"},
                    "inventory_pnl_at_bid": {"value": "0.01"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0.01"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.06"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.07"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "-2.27"},
                    "execution_spread_cost": {"value": "0.37"},
                    "fees": {"value": "1.85"},
                    "inventory_pnl_at_bid": {"value": "-0.05"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.04"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.06"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.02"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  }
                },
                "engine_replay": {
                  "recorded_by": "study",
                  "statement": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "fee_per_fill": "0.05",
                "fold": "1",
                "journal_sha256": "54696e4367a0b2ddfbd4e074f7fe7c3c9cbfef73697aa843b9370588d72dfa28",
                "latency_ns": "200000",
                "model": "logistic",
                "run_id": "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084",
                "scenario": "stress",
                "state": "completed"
              }
            ],
            "stream": "synthetic-2026-09-23",
            "window": {
              "end_at": "2026-09-23T13:30:00.210759199Z",
              "name": "fold_1",
              "start_at": "2026-09-23T13:30:00.000363583Z"
            }
          }
        ],
        "instrument": "AAPL",
        "missing": [],
        "plan_id": "8aaebf023671101aa76a34a61365e8e568946d76a8d1d3e9272c8ca38908043b",
        "pooled": {
          "predictions": {
            "logistic": {
              "cohort": {
                "accuracy": {"denominator": "494", "numerator": "213", "rounded": "0.431174"},
                "balanced_accuracy": {"denominator": "199", "numerator": "71", "rounded": "0.356784"},
                "confidence_bins": [
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.0-0.1",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.1-0.2",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.2-0.3",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.3-0.4",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"denominator": "419", "numerator": "174", "rounded": "0.415274"},
                    "correct": "174",
                    "items": "425",
                    "name": "0.4-0.5",
                    "scored": "419"
                  },
                  {
                    "accuracy": {"denominator": "25", "numerator": "13", "rounded": "0.52"},
                    "correct": "39",
                    "items": "75",
                    "name": "0.5-0.6",
                    "scored": "75"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.6-0.7",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.7-0.8",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.8-0.9",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.9-1.0",
                    "scored": "0"
                  }
                ],
                "confusion": {
                  "down": {"down": "160", "flat": "0", "up": "39"},
                  "flat": {"down": "72", "flat": "0", "up": "24"},
                  "up": {"down": "146", "flat": "0", "up": "53"}
                },
                "correct": "213",
                "future_returns": {
                  "all": {
                    "maximum_raw": {"value": "600601"},
                    "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                    "minimum_raw": {"value": "-500350"},
                    "rows": "494",
                    "sum_raw": "-2090231"
                  },
                  "by_confidence_bin": [
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.0-0.1",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.1-0.2",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.2-0.3",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.3-0.4",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"value": "600601"},
                      "mean_raw": {"denominator": "419", "numerator": "3207480", "rounded": "7655.083532"},
                      "minimum_raw": {"value": "-500350"},
                      "name": "0.4-0.5",
                      "rows": "419",
                      "sum_raw": "3207480"
                    },
                    {
                      "maximum_raw": {"value": "400400"},
                      "mean_raw": {"denominator": "75", "numerator": "-5297711", "rounded": "-70636.146667"},
                      "minimum_raw": {"value": "-500150"},
                      "name": "0.5-0.6",
                      "rows": "75",
                      "sum_raw": "-5297711"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.6-0.7",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.7-0.8",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.8-0.9",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.9-1.0",
                      "rows": "0",
                      "sum_raw": "0"
                    }
                  ],
                  "by_predicted_class": {
                    "down": {
                      "maximum_raw": {"value": "600601"},
                      "mean_raw": {"denominator": "126", "numerator": "-1963877", "rounded": "-15586.325397"},
                      "minimum_raw": {"value": "-500150"},
                      "rows": "378",
                      "sum_raw": "-5891631"
                    },
                    "flat": {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    "up": {
                      "maximum_raw": {"value": "500450"},
                      "mean_raw": {"denominator": "29", "numerator": "950350", "rounded": "32770.689655"},
                      "minimum_raw": {"value": "-500350"},
                      "rows": "116",
                      "sum_raw": "3801400"
                    }
                  },
                  "not_scored": "6"
                },
                "items": "500",
                "labels": {"label_unavailable:no_future_observation": "6", "scored": "494"},
                "predicted": {"down": "380", "flat": "0", "up": "120"},
                "realized": {"down": "199", "flat": "96", "up": "199"},
                "reasons": {},
                "scored": "494"
              },
              "folds": {"included": ["1"], "planned": "1"},
              "status": "complete"
            },
            "majority": {
              "cohort": {
                "accuracy": {"denominator": "494", "numerator": "199", "rounded": "0.402834"},
                "balanced_accuracy": {"denominator": "3", "numerator": "1", "rounded": "0.333333"},
                "confidence_bins": [
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.0-0.1",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.1-0.2",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.2-0.3",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.3-0.4",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"denominator": "494", "numerator": "199", "rounded": "0.402834"},
                    "correct": "199",
                    "items": "500",
                    "name": "0.4-0.5",
                    "scored": "494"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.5-0.6",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.6-0.7",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.7-0.8",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.8-0.9",
                    "scored": "0"
                  },
                  {
                    "accuracy": {"undefined": "no_scored_items"},
                    "correct": "0",
                    "items": "0",
                    "name": "0.9-1.0",
                    "scored": "0"
                  }
                ],
                "confusion": {
                  "down": {"down": "199", "flat": "0", "up": "0"},
                  "flat": {"down": "96", "flat": "0", "up": "0"},
                  "up": {"down": "199", "flat": "0", "up": "0"}
                },
                "correct": "199",
                "future_returns": {
                  "all": {
                    "maximum_raw": {"value": "600601"},
                    "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                    "minimum_raw": {"value": "-500350"},
                    "rows": "494",
                    "sum_raw": "-2090231"
                  },
                  "by_confidence_bin": [
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.0-0.1",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.1-0.2",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.2-0.3",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.3-0.4",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"value": "600601"},
                      "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                      "minimum_raw": {"value": "-500350"},
                      "name": "0.4-0.5",
                      "rows": "494",
                      "sum_raw": "-2090231"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.5-0.6",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.6-0.7",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.7-0.8",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.8-0.9",
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "name": "0.9-1.0",
                      "rows": "0",
                      "sum_raw": "0"
                    }
                  ],
                  "by_predicted_class": {
                    "down": {
                      "maximum_raw": {"value": "600601"},
                      "mean_raw": {"denominator": "38", "numerator": "-160787", "rounded": "-4231.236842"},
                      "minimum_raw": {"value": "-500350"},
                      "rows": "494",
                      "sum_raw": "-2090231"
                    },
                    "flat": {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "rows": "0",
                      "sum_raw": "0"
                    },
                    "up": {
                      "maximum_raw": {"undefined": "no_scored_items"},
                      "mean_raw": {"undefined": "no_scored_items"},
                      "minimum_raw": {"undefined": "no_scored_items"},
                      "rows": "0",
                      "sum_raw": "0"
                    }
                  },
                  "not_scored": "6"
                },
                "items": "500",
                "labels": {"label_unavailable:no_future_observation": "6", "scored": "494"},
                "predicted": {"down": "500", "flat": "0", "up": "0"},
                "realized": {"down": "199", "flat": "96", "up": "199"},
                "reasons": {},
                "scored": "494"
              },
              "folds": {"included": ["1"], "planned": "1"},
              "status": "complete"
            }
          },
          "trading": {
            "logistic": {
              "base": {
                "accounts": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-0.09"},
                    "execution_spread_cost": {"value": "0.01"},
                    "fees": {"value": "0.01"},
                    "inventory_pnl_at_bid": {"value": "-0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.01"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "-0.05"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "strategy": {
                    "booked_net_pnl": {"value": "-0.81"},
                    "execution_spread_cost": {"value": "0.37"},
                    "fees": {"value": "0.37"},
                    "inventory_pnl_at_bid": {"value": "-0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.08"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.02"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  }
                },
                "cohorts": {
                  "filled_orders": {
                    "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                    "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                        "correct": "16",
                        "items": "37",
                        "name": "0.4-0.5",
                        "scored": "36"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "9", "flat": "0", "up": "7"},
                      "flat": {"down": "2", "flat": "0", "up": "4"},
                      "up": {"down": "7", "flat": "0", "up": "7"}
                    },
                    "correct": "16",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "399920"},
                        "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                        "minimum_raw": {"value": "-399800"},
                        "rows": "36",
                        "sum_raw": "-698760"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "name": "0.4-0.5",
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "18",
                          "sum_raw": "-499100"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "200220"},
                          "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                          "minimum_raw": {"value": "-200140"},
                          "rows": "18",
                          "sum_raw": "-199660"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "37",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                    "predicted": {"down": "18", "flat": "0", "up": "19"},
                    "realized": {"down": "16", "flat": "6", "up": "14"},
                    "reasons": {"filled": "37"},
                    "scored": "36"
                  },
                  "order_scheduling_decisions": {
                    "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                    "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                        "correct": "16",
                        "items": "37",
                        "name": "0.4-0.5",
                        "scored": "36"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "9", "flat": "0", "up": "7"},
                      "flat": {"down": "2", "flat": "0", "up": "4"},
                      "up": {"down": "7", "flat": "0", "up": "7"}
                    },
                    "correct": "16",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "399920"},
                        "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                        "minimum_raw": {"value": "-399800"},
                        "rows": "36",
                        "sum_raw": "-698760"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "name": "0.4-0.5",
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "18",
                          "sum_raw": "-499100"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "200220"},
                          "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                          "minimum_raw": {"value": "-200140"},
                          "rows": "18",
                          "sum_raw": "-199660"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "37",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                    "predicted": {"down": "18", "flat": "0", "up": "19"},
                    "realized": {"down": "16", "flat": "6", "up": "14"},
                    "reasons": {"filled": "37"},
                    "scored": "36"
                  },
                  "sampled_decisions": {
                    "accuracy": {"denominator": "49", "numerator": "22", "rounded": "0.44898"},
                    "balanced_accuracy": {"denominator": "645", "numerator": "223", "rounded": "0.345736"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "86", "numerator": "39", "rounded": "0.453488"},
                        "correct": "39",
                        "items": "87",
                        "name": "0.4-0.5",
                        "scored": "86"
                      },
                      {
                        "accuracy": {"denominator": "12", "numerator": "5", "rounded": "0.416667"},
                        "correct": "5",
                        "items": "12",
                        "name": "0.5-0.6",
                        "scored": "12"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "36", "flat": "0", "up": "7"},
                      "flat": {"down": "11", "flat": "0", "up": "4"},
                      "up": {"down": "32", "flat": "0", "up": "8"}
                    },
                    "correct": "44",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "600601"},
                        "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                        "minimum_raw": {"value": "-400160"},
                        "rows": "98",
                        "sum_raw": "-798199"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "86", "numerator": "-98329", "rounded": "-1143.360465"},
                          "minimum_raw": {"value": "-400160"},
                          "name": "0.4-0.5",
                          "rows": "86",
                          "sum_raw": "-98329"
                        },
                        {
                          "maximum_raw": {"value": "199880"},
                          "mean_raw": {"denominator": "2", "numerator": "-116645", "rounded": "-58322.5"},
                          "minimum_raw": {"value": "-400120"},
                          "name": "0.5-0.6",
                          "rows": "12",
                          "sum_raw": "-699870"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "79", "numerator": "-698549", "rounded": "-8842.392405"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "79",
                          "sum_raw": "-698549"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "200220"},
                          "mean_raw": {"denominator": "19", "numerator": "-99650", "rounded": "-5244.736842"},
                          "minimum_raw": {"value": "-200140"},
                          "rows": "19",
                          "sum_raw": "-99650"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "99",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                    "predicted": {"down": "79", "flat": "0", "up": "20"},
                    "realized": {"down": "43", "flat": "15", "up": "40"},
                    "reasons": {"already_at_target": "62", "order_scheduled": "37"},
                    "scored": "98"
                  }
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-0.72"},
                    "execution_spread_cost": {"value": "0.36"},
                    "fees": {"value": "0.36"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.07"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.07"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "-0.81"},
                    "execution_spread_cost": {"value": "0.37"},
                    "fees": {"value": "0.37"},
                    "inventory_pnl_at_bid": {"value": "-0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.08"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.02"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  }
                },
                "folds": {"included": ["1"], "missing": [], "planned": "1"},
                "status": "complete"
              },
              "stress": {
                "accounts": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-0.12"},
                    "execution_spread_cost": {"value": "0.01"},
                    "fees": {"value": "0.05"},
                    "inventory_pnl_at_bid": {"value": "-0.06"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.05"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "-0.05"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "strategy": {
                    "booked_net_pnl": {"value": "-2.27"},
                    "execution_spread_cost": {"value": "0.37"},
                    "fees": {"value": "1.85"},
                    "inventory_pnl_at_bid": {"value": "-0.05"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.04"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.06"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.02"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  }
                },
                "cohorts": {
                  "filled_orders": {
                    "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                    "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                        "correct": "16",
                        "items": "37",
                        "name": "0.4-0.5",
                        "scored": "36"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "9", "flat": "0", "up": "7"},
                      "flat": {"down": "2", "flat": "0", "up": "4"},
                      "up": {"down": "7", "flat": "0", "up": "7"}
                    },
                    "correct": "16",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "399920"},
                        "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                        "minimum_raw": {"value": "-399800"},
                        "rows": "36",
                        "sum_raw": "-698760"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "name": "0.4-0.5",
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "18",
                          "sum_raw": "-499100"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "200220"},
                          "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                          "minimum_raw": {"value": "-200140"},
                          "rows": "18",
                          "sum_raw": "-199660"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "37",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                    "predicted": {"down": "18", "flat": "0", "up": "19"},
                    "realized": {"down": "16", "flat": "6", "up": "14"},
                    "reasons": {"filled": "37"},
                    "scored": "36"
                  },
                  "order_scheduling_decisions": {
                    "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                    "balanced_accuracy": {"denominator": "48", "numerator": "17", "rounded": "0.354167"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "9", "numerator": "4", "rounded": "0.444444"},
                        "correct": "16",
                        "items": "37",
                        "name": "0.4-0.5",
                        "scored": "36"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "9", "flat": "0", "up": "7"},
                      "flat": {"down": "2", "flat": "0", "up": "4"},
                      "up": {"down": "7", "flat": "0", "up": "7"}
                    },
                    "correct": "16",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "399920"},
                        "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                        "minimum_raw": {"value": "-399800"},
                        "rows": "36",
                        "sum_raw": "-698760"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "1", "numerator": "-19410", "rounded": "-19410"},
                          "minimum_raw": {"value": "-399800"},
                          "name": "0.4-0.5",
                          "rows": "36",
                          "sum_raw": "-698760"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "399920"},
                          "mean_raw": {"denominator": "9", "numerator": "-249550", "rounded": "-27727.777778"},
                          "minimum_raw": {"value": "-399800"},
                          "rows": "18",
                          "sum_raw": "-499100"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "200220"},
                          "mean_raw": {"denominator": "9", "numerator": "-99830", "rounded": "-11092.222222"},
                          "minimum_raw": {"value": "-200140"},
                          "rows": "18",
                          "sum_raw": "-199660"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "37",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "36"},
                    "predicted": {"down": "18", "flat": "0", "up": "19"},
                    "realized": {"down": "16", "flat": "6", "up": "14"},
                    "reasons": {"filled": "37"},
                    "scored": "36"
                  },
                  "sampled_decisions": {
                    "accuracy": {"denominator": "49", "numerator": "22", "rounded": "0.44898"},
                    "balanced_accuracy": {"denominator": "645", "numerator": "223", "rounded": "0.345736"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "86", "numerator": "39", "rounded": "0.453488"},
                        "correct": "39",
                        "items": "87",
                        "name": "0.4-0.5",
                        "scored": "86"
                      },
                      {
                        "accuracy": {"denominator": "12", "numerator": "5", "rounded": "0.416667"},
                        "correct": "5",
                        "items": "12",
                        "name": "0.5-0.6",
                        "scored": "12"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "36", "flat": "0", "up": "7"},
                      "flat": {"down": "11", "flat": "0", "up": "4"},
                      "up": {"down": "32", "flat": "0", "up": "8"}
                    },
                    "correct": "44",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "600601"},
                        "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                        "minimum_raw": {"value": "-400160"},
                        "rows": "98",
                        "sum_raw": "-798199"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "86", "numerator": "-98329", "rounded": "-1143.360465"},
                          "minimum_raw": {"value": "-400160"},
                          "name": "0.4-0.5",
                          "rows": "86",
                          "sum_raw": "-98329"
                        },
                        {
                          "maximum_raw": {"value": "199880"},
                          "mean_raw": {"denominator": "2", "numerator": "-116645", "rounded": "-58322.5"},
                          "minimum_raw": {"value": "-400120"},
                          "name": "0.5-0.6",
                          "rows": "12",
                          "sum_raw": "-699870"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "79", "numerator": "-698549", "rounded": "-8842.392405"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "79",
                          "sum_raw": "-698549"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"value": "200220"},
                          "mean_raw": {"denominator": "19", "numerator": "-99650", "rounded": "-5244.736842"},
                          "minimum_raw": {"value": "-200140"},
                          "rows": "19",
                          "sum_raw": "-99650"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "99",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                    "predicted": {"down": "79", "flat": "0", "up": "20"},
                    "realized": {"down": "43", "flat": "15", "up": "40"},
                    "reasons": {"already_at_target": "62", "order_scheduled": "37"},
                    "scored": "98"
                  }
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-2.15"},
                    "execution_spread_cost": {"value": "0.36"},
                    "fees": {"value": "1.8"},
                    "inventory_pnl_at_bid": {"value": "0.01"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0.01"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.06"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.07"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "-2.27"},
                    "execution_spread_cost": {"value": "0.37"},
                    "fees": {"value": "1.85"},
                    "inventory_pnl_at_bid": {"value": "-0.05"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.04"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.06"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.02"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  }
                },
                "folds": {"included": ["1"], "missing": [], "planned": "1"},
                "status": "complete"
              }
            },
            "majority": {
              "base": {
                "accounts": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-0.09"},
                    "execution_spread_cost": {"value": "0.01"},
                    "fees": {"value": "0.01"},
                    "inventory_pnl_at_bid": {"value": "-0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0.01"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "-0.05"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "strategy": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  }
                },
                "cohorts": {
                  "filled_orders": {
                    "accuracy": {"undefined": "no_scored_items"},
                    "balanced_accuracy": {"undefined": "no_scored_items"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.4-0.5",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "0", "flat": "0", "up": "0"},
                      "flat": {"down": "0", "flat": "0", "up": "0"},
                      "up": {"down": "0", "flat": "0", "up": "0"}
                    },
                    "correct": "0",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"undefined": "no_scored_items"},
                        "mean_raw": {"undefined": "no_scored_items"},
                        "minimum_raw": {"undefined": "no_scored_items"},
                        "rows": "0",
                        "sum_raw": "0"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.4-0.5",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "0"
                    },
                    "items": "0",
                    "labels": {},
                    "predicted": {"down": "0", "flat": "0", "up": "0"},
                    "realized": {"down": "0", "flat": "0", "up": "0"},
                    "reasons": {},
                    "scored": "0"
                  },
                  "order_scheduling_decisions": {
                    "accuracy": {"undefined": "no_scored_items"},
                    "balanced_accuracy": {"undefined": "no_scored_items"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.4-0.5",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "0", "flat": "0", "up": "0"},
                      "flat": {"down": "0", "flat": "0", "up": "0"},
                      "up": {"down": "0", "flat": "0", "up": "0"}
                    },
                    "correct": "0",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"undefined": "no_scored_items"},
                        "mean_raw": {"undefined": "no_scored_items"},
                        "minimum_raw": {"undefined": "no_scored_items"},
                        "rows": "0",
                        "sum_raw": "0"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.4-0.5",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "0"
                    },
                    "items": "0",
                    "labels": {},
                    "predicted": {"down": "0", "flat": "0", "up": "0"},
                    "realized": {"down": "0", "flat": "0", "up": "0"},
                    "reasons": {},
                    "scored": "0"
                  },
                  "sampled_decisions": {
                    "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                    "balanced_accuracy": {"denominator": "3", "numerator": "1", "rounded": "0.333333"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                        "correct": "43",
                        "items": "99",
                        "name": "0.4-0.5",
                        "scored": "98"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "43", "flat": "0", "up": "0"},
                      "flat": {"down": "15", "flat": "0", "up": "0"},
                      "up": {"down": "40", "flat": "0", "up": "0"}
                    },
                    "correct": "43",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "600601"},
                        "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                        "minimum_raw": {"value": "-400160"},
                        "rows": "98",
                        "sum_raw": "-798199"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "name": "0.4-0.5",
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "99",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                    "predicted": {"down": "99", "flat": "0", "up": "0"},
                    "realized": {"down": "43", "flat": "15", "up": "40"},
                    "reasons": {"already_at_target": "99"},
                    "scored": "98"
                  }
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "0.09"},
                    "execution_spread_cost": {"value": "-0.01"},
                    "fees": {"value": "-0.01"},
                    "inventory_pnl_at_bid": {"value": "0.07"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0.06"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "-0.01"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.05"},
                    "terminal_bid_markdown": {"value": "-0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  }
                },
                "folds": {"included": ["1"], "missing": [], "planned": "1"},
                "status": "complete"
              },
              "stress": {
                "accounts": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "-0.12"},
                    "execution_spread_cost": {"value": "0.01"},
                    "fees": {"value": "0.05"},
                    "inventory_pnl_at_bid": {"value": "-0.06"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "-0.05"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "-0.05"},
                    "terminal_bid_markdown": {"value": "0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  },
                  "strategy": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  }
                },
                "cohorts": {
                  "filled_orders": {
                    "accuracy": {"undefined": "no_scored_items"},
                    "balanced_accuracy": {"undefined": "no_scored_items"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.4-0.5",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "0", "flat": "0", "up": "0"},
                      "flat": {"down": "0", "flat": "0", "up": "0"},
                      "up": {"down": "0", "flat": "0", "up": "0"}
                    },
                    "correct": "0",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"undefined": "no_scored_items"},
                        "mean_raw": {"undefined": "no_scored_items"},
                        "minimum_raw": {"undefined": "no_scored_items"},
                        "rows": "0",
                        "sum_raw": "0"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.4-0.5",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "0"
                    },
                    "items": "0",
                    "labels": {},
                    "predicted": {"down": "0", "flat": "0", "up": "0"},
                    "realized": {"down": "0", "flat": "0", "up": "0"},
                    "reasons": {},
                    "scored": "0"
                  },
                  "order_scheduling_decisions": {
                    "accuracy": {"undefined": "no_scored_items"},
                    "balanced_accuracy": {"undefined": "no_scored_items"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.4-0.5",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "0", "flat": "0", "up": "0"},
                      "flat": {"down": "0", "flat": "0", "up": "0"},
                      "up": {"down": "0", "flat": "0", "up": "0"}
                    },
                    "correct": "0",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"undefined": "no_scored_items"},
                        "mean_raw": {"undefined": "no_scored_items"},
                        "minimum_raw": {"undefined": "no_scored_items"},
                        "rows": "0",
                        "sum_raw": "0"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.4-0.5",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "0"
                    },
                    "items": "0",
                    "labels": {},
                    "predicted": {"down": "0", "flat": "0", "up": "0"},
                    "realized": {"down": "0", "flat": "0", "up": "0"},
                    "reasons": {},
                    "scored": "0"
                  },
                  "sampled_decisions": {
                    "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                    "balanced_accuracy": {"denominator": "3", "numerator": "1", "rounded": "0.333333"},
                    "confidence_bins": [
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.0-0.1",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.1-0.2",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.2-0.3",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.3-0.4",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"denominator": "98", "numerator": "43", "rounded": "0.438776"},
                        "correct": "43",
                        "items": "99",
                        "name": "0.4-0.5",
                        "scored": "98"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.5-0.6",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.6-0.7",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.7-0.8",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.8-0.9",
                        "scored": "0"
                      },
                      {
                        "accuracy": {"undefined": "no_scored_items"},
                        "correct": "0",
                        "items": "0",
                        "name": "0.9-1.0",
                        "scored": "0"
                      }
                    ],
                    "confusion": {
                      "down": {"down": "43", "flat": "0", "up": "0"},
                      "flat": {"down": "15", "flat": "0", "up": "0"},
                      "up": {"down": "40", "flat": "0", "up": "0"}
                    },
                    "correct": "43",
                    "future_returns": {
                      "all": {
                        "maximum_raw": {"value": "600601"},
                        "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                        "minimum_raw": {"value": "-400160"},
                        "rows": "98",
                        "sum_raw": "-798199"
                      },
                      "by_confidence_bin": [
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.0-0.1",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.1-0.2",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.2-0.3",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.3-0.4",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "name": "0.4-0.5",
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.5-0.6",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.6-0.7",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.7-0.8",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.8-0.9",
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "name": "0.9-1.0",
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      ],
                      "by_predicted_class": {
                        "down": {
                          "maximum_raw": {"value": "600601"},
                          "mean_raw": {"denominator": "98", "numerator": "-798199", "rounded": "-8144.887755"},
                          "minimum_raw": {"value": "-400160"},
                          "rows": "98",
                          "sum_raw": "-798199"
                        },
                        "flat": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        },
                        "up": {
                          "maximum_raw": {"undefined": "no_scored_items"},
                          "mean_raw": {"undefined": "no_scored_items"},
                          "minimum_raw": {"undefined": "no_scored_items"},
                          "rows": "0",
                          "sum_raw": "0"
                        }
                      },
                      "not_scored": "1"
                    },
                    "items": "99",
                    "labels": {"label_unavailable:no_future_observation": "1", "scored": "98"},
                    "predicted": {"down": "99", "flat": "0", "up": "0"},
                    "realized": {"down": "43", "flat": "15", "up": "40"},
                    "reasons": {"already_at_target": "99"},
                    "scored": "98"
                  }
                },
                "deltas": {
                  "buy_and_hold": {
                    "booked_net_pnl": {"value": "0.12"},
                    "execution_spread_cost": {"value": "-0.01"},
                    "fees": {"value": "-0.05"},
                    "inventory_pnl_at_bid": {"value": "0.06"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0.05"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0.05"},
                    "terminal_bid_markdown": {"value": "-0.01"}
                  },
                  "cash": {
                    "booked_net_pnl": {"value": "0"},
                    "execution_spread_cost": {"value": "0"},
                    "fees": {"value": "0"},
                    "inventory_pnl_at_bid": {"value": "0"},
                    "market_value_rounding": {"value": "0"},
                    "midpoint_inventory_pnl": {"value": "0"},
                    "notional_rounding": {"value": "0"},
                    "observed_midpoint_movement": {"value": "0"},
                    "rounding_adjustment": {"value": "0"},
                    "signal_reference_pnl": {"value": "0"},
                    "terminal_bid_markdown": {"value": "0"}
                  }
                },
                "folds": {"included": ["1"], "missing": [], "planned": "1"},
                "status": "complete"
              }
            }
          }
        },
        "scenarios": [
          {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
          {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"}
        ],
        "status": "complete",
        "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
        "study_status": "complete"
      }
    ],
    "verification": "at generation: the study and plan by identity; the plan rebuilt from its request over the collection's members, each verified by the C++ tools; every schedule's rows and checksum; every run's manifest identity and journal checksum, and that it is its cell's run; every dataset's events by checksum. Engine replay is the study's own record; nothing here replays or refits"
  },
  "verification": {
    "artifact": {"detail": "the manifest's identity and every file's checksum and row count, read now", "state": "verified"},
    "engine_replay": {
      "detail": "each run's replay by chronos_strategy inspect was recorded by its study when the study ran; diagnostics never replay or refit",
      "state": "recorded_by_study"
    },
    "independent_audit": {
      "command": "python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> <diagnostic_id>",
      "detail": "a standard-library recomputation of every figure from the raw files, run from the CLI",
      "state": "not_run_by_this_read"
    },
    "recomputed_inspection": {
      "command": "python -m chronos_research inspect-strategy-diagnostics --tools <tools> --artifacts <artifacts> --diagnostics <diagnostic_id>",
      "detail": "rebuilds the artifact from its sources, verified again, and requires identical bytes",
      "state": "not_run_by_this_read"
    },
    "sources": {
      "at_generation": "at generation: the study and plan by identity; the plan rebuilt from its request over the collection's members, each verified by the C++ tools; every schedule's rows and checksum; every run's manifest identity and journal checksum, and that it is its cell's run; every dataset's events by checksum. Engine replay is the study's own record; nothing here replays or refits",
      "now": {
        "content": {"hashed_now": "9", "reused_unchanged": "0"},
        "detail": "checked by this read: each study's, plan's, and collection's identity and what it names; each dataset's, feature artifact's, label artifact's, schedule's, and run's manifest identity, its bindings to the others, and the size and SHA-256 of the file the diagnostics consumed, compared with what they read. A SHA-256 is computed now, or reused from this service's own earlier hash of the same file while its device, inode, size, modification time, and change time are unchanged. A figure is verified only if every source it depends on is. Nothing is rebuilt, audited, or replayed here",
        "figures": {
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": {"state": "verified"},
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": {"state": "verified"},
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": {"state": "verified"},
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": {"state": "verified"}
          },
          "studies": {
            "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": {
              "folds": {"1": {"logistic": {"state": "verified"}, "majority": {"state": "verified"}}},
              "pooled": {
                "predictions": {"logistic": {"state": "verified"}, "majority": {"state": "verified"}},
                "trading": {
                  "logistic": {"base": {"state": "verified"}, "stress": {"state": "verified"}},
                  "majority": {"base": {"state": "verified"}, "stress": {"state": "verified"}}
                }
              }
            }
          }
        },
        "sources": {
          "studies": {"48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": "verified"},
          "plans": {"8aaebf023671101aa76a34a61365e8e568946d76a8d1d3e9272c8ca38908043b": "verified"},
          "collections": {"92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879": "verified"},
          "datasets": {"d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a": "verified"},
          "features": {"f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5": "verified"},
          "labels": {"78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6": "verified"},
          "schedules": {
            "106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9": "verified",
            "79c7c2f9c60ddbe467a36235194ae84daeb9337111803c678b9650d6e610b184": "verified"
          },
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": "verified",
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": "verified",
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": "verified",
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": "verified"
          }
        },
        "state": "verified"
      }
    }
  }
};

const LISTING: Json = {
  "diagnostics": [
    {
      "counts": {"episodes": "42", "fills": "78", "runs": "4", "runs_attributed": "4"},
      "diagnostic_id": "ea7c2b639c877f0f26bc53150d97c6576c43309c887b79f2685cd4a4eab46404",
      "status": "complete",
      "studies": [
        {
          "evidence": "synthetic",
          "instrument": "AAPL",
          "missing": "0",
          "status": "complete",
          "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
        }
      ]
    }
  ],
  "invalid_entries": [],
  "next_cursor": null
};

// The strategy account's first four fills, then the next four.
const FILLS_FIRST: Json = {
  "fills": [
    {
      "account": "strategy",
      "cash_after": "9899.98",
      "client_order_id": "strategy-000001",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"}
      },
      "decision": "1",
      "execution": {
        "ask": {"price": "100.01", "quantity": "15"},
        "bid": {"price": "99.99", "quantity": "18"},
        "executed_at": "2026-09-23T13:30:00.000440602Z",
        "ingest_sequence": "2",
        "midpoint": {"value": "100"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000001:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100.01",
      "outcome": "filled",
      "position_after": "1",
      "price": "100.01",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "buy",
      "signal": {
        "ask": "100",
        "available_at": "2026-09-23T13:30:00.000363583Z",
        "bid": "99.98",
        "class": "up",
        "confidence_ppb": "450106022",
        "ingest_sequence": "0",
        "midpoint": {"value": "99.99"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.000363583Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "76019",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "77019"
      }
    },
    {
      "account": "strategy",
      "cash_after": "9999.94",
      "client_order_id": "strategy-000002",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "-0.01"}
      },
      "decision": "2",
      "execution": {
        "ask": {"price": "99.99", "quantity": "19"},
        "bid": {"price": "99.97", "quantity": "19"},
        "executed_at": "2026-09-23T13:30:00.001703734Z",
        "ingest_sequence": "10",
        "midpoint": {"value": "99.98"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000002:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "99.97",
      "outcome": "filled",
      "position_after": "0",
      "price": "99.97",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "sell",
      "signal": {
        "ask": "99.98",
        "available_at": "2026-09-23T13:30:00.001395732Z",
        "bid": "99.96",
        "class": "down",
        "confidence_ppb": "460380903",
        "ingest_sequence": "8",
        "midpoint": {"value": "99.97"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.001395732Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "307002",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "308002"
      }
    },
    {
      "account": "strategy",
      "cash_after": "9899.91",
      "client_order_id": "strategy-000003",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"}
      },
      "decision": "7",
      "execution": {
        "ask": {"price": "100.02", "quantity": "19"},
        "bid": {"price": "100", "quantity": "5"},
        "executed_at": "2026-09-23T13:30:00.007202838Z",
        "ingest_sequence": "58",
        "midpoint": {"value": "100.01"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000003:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100.02",
      "outcome": "filled",
      "position_after": "1",
      "price": "100.02",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "buy",
      "signal": {
        "ask": "100.01",
        "available_at": "2026-09-23T13:30:00.007005964Z",
        "bid": "99.99",
        "class": "up",
        "confidence_ppb": "416618024",
        "ingest_sequence": "56",
        "midpoint": {"value": "100"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.007005964Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "195874",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "196874"
      }
    },
    {
      "account": "strategy",
      "cash_after": "9999.86",
      "client_order_id": "strategy-000004",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"}
      },
      "decision": "8",
      "execution": {
        "ask": {"price": "99.98", "quantity": "4"},
        "bid": {"price": "99.96", "quantity": "16"},
        "executed_at": "2026-09-23T13:30:00.008475006Z",
        "ingest_sequence": "70",
        "midpoint": {"value": "99.97"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000004:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "99.96",
      "outcome": "filled",
      "position_after": "0",
      "price": "99.96",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "sell",
      "signal": {
        "ask": "99.99",
        "available_at": "2026-09-23T13:30:00.008266003Z",
        "bid": "99.97",
        "class": "down",
        "confidence_ppb": "485392922",
        "ingest_sequence": "68",
        "midpoint": {"value": "99.98"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.008266003Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "208003",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "209003"
      }
    }
  ],
  "next_cursor": "djF8ZmlsbHMubmRqc29ufGVhN2MyYjYzOWM4NzdmMGYyNmJjNTMxNTBkOTdjNjU3NmM0MzMwOWM4ODdiNzlmMjY4NWNkNGE0ZWFiNDY0MDR8OTFjYWU2OGY5ZTFmMjQ2NTQ2OGI1OWFhMjAyMTQ2MmIxZjQ0NDkyZWEzNDE2YjFkM2Y2NjFkODQwNTQyYjU4OHxzdHJhdGVneXw0"
};

const FILLS_NEXT: Json = {
  "fills": [
    {
      "account": "strategy",
      "cash_after": "9899.84",
      "client_order_id": "strategy-000005",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"}
      },
      "decision": "13",
      "execution": {
        "ask": {"price": "100.01", "quantity": "10"},
        "bid": {"price": "99.99", "quantity": "4"},
        "executed_at": "2026-09-23T13:30:00.014296732Z",
        "ingest_sequence": "124",
        "midpoint": {"value": "100"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000005:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100.01",
      "outcome": "filled",
      "position_after": "1",
      "price": "100.01",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "buy",
      "signal": {
        "ask": "100",
        "available_at": "2026-09-23T13:30:00.014026728Z",
        "bid": "99.98",
        "class": "up",
        "confidence_ppb": "464461590",
        "ingest_sequence": "122",
        "midpoint": {"value": "99.99"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.014026728Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "269004",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "270004"
      }
    },
    {
      "account": "strategy",
      "cash_after": "9999.84",
      "client_order_id": "strategy-000006",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "-0.01"}
      },
      "decision": "14",
      "execution": {
        "ask": {"price": "100.03", "quantity": "19"},
        "bid": {"price": "100.01", "quantity": "13"},
        "executed_at": "2026-09-23T13:30:00.015471395Z",
        "ingest_sequence": "142",
        "midpoint": {"value": "100.02"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000006:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100.01",
      "outcome": "filled",
      "position_after": "0",
      "price": "100.01",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "sell",
      "signal": {
        "ask": "100.02",
        "available_at": "2026-09-23T13:30:00.015316435Z",
        "bid": "100",
        "class": "down",
        "confidence_ppb": "473697236",
        "ingest_sequence": "140",
        "midpoint": {"value": "100.01"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.015316435Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "153960",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "154960"
      }
    },
    {
      "account": "strategy",
      "cash_after": "9899.82",
      "client_order_id": "strategy-000007",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"}
      },
      "decision": "15",
      "execution": {
        "ask": {"price": "100.01", "quantity": "12"},
        "bid": {"price": "99.99", "quantity": "2"},
        "executed_at": "2026-09-23T13:30:00.016708935Z",
        "ingest_sequence": "156",
        "midpoint": {"value": "100"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000007:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100.01",
      "outcome": "filled",
      "position_after": "1",
      "price": "100.01",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "buy",
      "signal": {
        "ask": "100",
        "available_at": "2026-09-23T13:30:00.016317178Z",
        "bid": "99.98",
        "class": "up",
        "confidence_ppb": "431590535",
        "ingest_sequence": "154",
        "midpoint": {"value": "99.99"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.016317178Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "390757",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "391757"
      }
    },
    {
      "account": "strategy",
      "cash_after": "9999.81",
      "client_order_id": "strategy-000008",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "-0.01"}
      },
      "decision": "16",
      "execution": {
        "ask": {"price": "100.02", "quantity": "13"},
        "bid": {"price": "100", "quantity": "11"},
        "executed_at": "2026-09-23T13:30:00.017780319Z",
        "ingest_sequence": "164",
        "midpoint": {"value": "100.01"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "strategy-000008:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100",
      "outcome": "filled",
      "position_after": "0",
      "price": "100",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "sell",
      "signal": {
        "ask": "100.01",
        "available_at": "2026-09-23T13:30:00.017466339Z",
        "bid": "99.99",
        "class": "down",
        "confidence_ppb": "438268148",
        "ingest_sequence": "162",
        "midpoint": {"value": "100"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.017466339Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "312980",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "313980"
      }
    }
  ],
  "next_cursor": "djF8ZmlsbHMubmRqc29ufGVhN2MyYjYzOWM4NzdmMGYyNmJjNTMxNTBkOTdjNjU3NmM0MzMwOWM4ODdiNzlmMjY4NWNkNGE0ZWFiNDY0MDR8OTFjYWU2OGY5ZTFmMjQ2NTQ2OGI1OWFhMjAyMTQ2MmIxZjQ0NDkyZWEzNDE2YjFkM2Y2NjFkODQwNTQyYjU4OHxzdHJhdGVneXw4"
};

const HOLDER_FILLS: Json = {
  "fills": [
    {
      "account": "buy_and_hold",
      "cash_after": "9899.98",
      "client_order_id": "buy_and_hold-000001",
      "contributions": {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"}
      },
      "decision": "1",
      "execution": {
        "ask": {"price": "100.01", "quantity": "15"},
        "bid": {"price": "99.99", "quantity": "18"},
        "executed_at": "2026-09-23T13:30:00.000440602Z",
        "ingest_sequence": "2",
        "midpoint": {"value": "100"},
        "quoted_spread": {"value": "0.02"}
      },
      "fee": "0.01",
      "fill_id": "buy_and_hold-000001:fill:0",
      "fold": "1",
      "model": "logistic",
      "notional": "100.01",
      "outcome": "filled",
      "position_after": "1",
      "price": "100.01",
      "quantity": "1",
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "side": "buy",
      "signal": {
        "ask": "100",
        "available_at": "2026-09-23T13:30:00.000363583Z",
        "bid": "99.98",
        "class": "up",
        "confidence_ppb": "450106022",
        "ingest_sequence": "0",
        "midpoint": {"value": "99.99"},
        "quoted_spread": {"value": "0.02"},
        "signal_at": "2026-09-23T13:30:00.000363583Z"
      },
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388",
      "timing": {
        "arrival_to_execution_duration_ns": "76019",
        "configured_latency_ns": "1000",
        "signal_to_arrival_duration_ns": "1000",
        "signal_to_execution_duration_ns": "77019"
      }
    }
  ],
  "next_cursor": null
};

const EPISODES: Json = {
  "episodes": [
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.06"},
      "censored_at": null,
      "duration_ns": "1263132",
      "entry_at": "2026-09-23T13:30:00.000440602Z",
      "episode": "1",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.001703734Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.06"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.08"},
      "censored_at": null,
      "duration_ns": "1272168",
      "entry_at": "2026-09-23T13:30:00.007202838Z",
      "episode": "2",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.008475006Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.08"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.02"},
      "censored_at": null,
      "duration_ns": "1174663",
      "entry_at": "2026-09-23T13:30:00.014296732Z",
      "episode": "3",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.015471395Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.02"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.03"},
      "censored_at": null,
      "duration_ns": "1071384",
      "entry_at": "2026-09-23T13:30:00.016708935Z",
      "episode": "4",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.017780319Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.03"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.02"},
      "censored_at": null,
      "duration_ns": "1041497",
      "entry_at": "2026-09-23T13:30:00.041553063Z",
      "episode": "5",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.042594560Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.02"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.06"},
      "censored_at": null,
      "duration_ns": "1136043",
      "entry_at": "2026-09-23T13:30:00.055353775Z",
      "episode": "6",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.056489818Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.06"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.05"},
      "censored_at": null,
      "duration_ns": "1111682",
      "entry_at": "2026-09-23T13:30:00.085799295Z",
      "episode": "7",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.086910977Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.05"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.05"},
      "censored_at": null,
      "duration_ns": "1129097",
      "entry_at": "2026-09-23T13:30:00.090320539Z",
      "episode": "8",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.091449636Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.05"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.04"},
      "censored_at": null,
      "duration_ns": "1218798",
      "entry_at": "2026-09-23T13:30:00.092471375Z",
      "episode": "9",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.093690173Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.04"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.06"},
      "censored_at": null,
      "duration_ns": "1471686",
      "entry_at": "2026-09-23T13:30:00.094779290Z",
      "episode": "10",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.096250976Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.06"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.03"},
      "censored_at": null,
      "duration_ns": "2022798",
      "entry_at": "2026-09-23T13:30:00.099183807Z",
      "episode": "11",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.101206605Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.03"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.01"},
      "censored_at": null,
      "duration_ns": "1254017",
      "entry_at": "2026-09-23T13:30:00.133471110Z",
      "episode": "12",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.134725127Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.01"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.06"},
      "censored_at": null,
      "duration_ns": "1631152",
      "entry_at": "2026-09-23T13:30:00.166224161Z",
      "episode": "13",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.167855313Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.06"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.08"},
      "censored_at": null,
      "duration_ns": "1127697",
      "entry_at": "2026-09-23T13:30:00.169024892Z",
      "episode": "14",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.170152589Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.08"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.04"},
      "censored_at": null,
      "duration_ns": "1284360",
      "entry_at": "2026-09-23T13:30:00.172115237Z",
      "episode": "15",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.173399597Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.04"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.05"},
      "censored_at": null,
      "duration_ns": "1206616",
      "entry_at": "2026-09-23T13:30:00.174474869Z",
      "episode": "16",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.175681485Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.05"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.04"},
      "censored_at": null,
      "duration_ns": "1054218",
      "entry_at": "2026-09-23T13:30:00.180325714Z",
      "episode": "17",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.181379932Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.04"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "booked",
      "bought": "1",
      "cash_flow": {"value": "-0.02"},
      "censored_at": null,
      "duration_ns": "1153658",
      "entry_at": "2026-09-23T13:30:00.184799892Z",
      "episode": "18",
      "execution_spread_cost": {"value": "0.02"},
      "exit_at": "2026-09-23T13:30:00.185953550Z",
      "fees": {"value": "0.02"},
      "fills": "2",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.02"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "1",
      "state": "completed",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    },
    {
      "account": "strategy",
      "basis": "marked_at_final_valuation",
      "bought": "1",
      "cash_flow": {"value": "-99.94"},
      "censored_at": "2026-09-23T13:30:00.210759198Z",
      "duration_ns": "762992",
      "entry_at": "2026-09-23T13:30:00.209996206Z",
      "episode": "19",
      "execution_spread_cost": {"value": "0.01"},
      "exit_at": null,
      "fees": {"value": "0.01"},
      "fills": "1",
      "fold": "1",
      "maximum_position": "1",
      "model": "logistic",
      "pnl": {"value": "-0.01"},
      "run_id": "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588",
      "scenario": "base",
      "sold": "0",
      "state": "censored",
      "study_id": "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"
    }
  ],
  "next_cursor": null
};

// The service's verification of the same read with one source changed: the
// logistic base run's journal, the logistic model's schedule, or the study's
// manifest. Each is what that read returned, trust states and all.
const UNVERIFIED: Record<'run' | 'schedule' | 'study', Json> = {
  "run": {
    "artifact": {"detail": "the manifest's identity and every file's checksum and row count, read now", "state": "verified"},
    "engine_replay": {
      "detail": "each run's replay by chronos_strategy inspect was recorded by its study when the study ran; diagnostics never replay or refit",
      "state": "recorded_by_study"
    },
    "independent_audit": {
      "command": "python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> <diagnostic_id>",
      "detail": "a standard-library recomputation of every figure from the raw files, run from the CLI",
      "state": "not_run_by_this_read"
    },
    "recomputed_inspection": {
      "command": "python -m chronos_research inspect-strategy-diagnostics --tools <tools> --artifacts <artifacts> --diagnostics <diagnostic_id>",
      "detail": "rebuilds the artifact from its sources, verified again, and requires identical bytes",
      "state": "not_run_by_this_read"
    },
    "sources": {
      "at_generation": "at generation: the study and plan by identity; the plan rebuilt from its request over the collection's members, each verified by the C++ tools; every schedule's rows and checksum; every run's manifest identity and journal checksum, and that it is its cell's run; every dataset's events by checksum. Engine replay is the study's own record; nothing here replays or refits",
      "now": {
        "content": {"hashed_now": "0", "reused_unchanged": "8"},
        "detail": "checked by this read: each study's, plan's, and collection's identity and what it names; each dataset's, feature artifact's, label artifact's, schedule's, and run's manifest identity, its bindings to the others, and the size and SHA-256 of the file the diagnostics consumed, compared with what they read. A SHA-256 is computed now, or reused from this service's own earlier hash of the same file while its device, inode, size, modification time, and change time are unchanged. A figure is verified only if every source it depends on is. Nothing is rebuilt, audited, or replayed here",
        "figures": {
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": {"state": "verified"},
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": {"state": "verified"},
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": {
              "state": "invalid",
              "sources": ["runs:91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588"]
            },
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": {"state": "verified"}
          },
          "studies": {
            "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": {
              "folds": {"1": {"logistic": {"state": "verified"}, "majority": {"state": "verified"}}},
              "pooled": {
                "predictions": {"logistic": {"state": "verified"}, "majority": {"state": "verified"}},
                "trading": {
                  "logistic": {
                    "base": {
                      "state": "invalid",
                      "sources": ["runs:91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588"]
                    },
                    "stress": {"state": "verified"}
                  },
                  "majority": {"base": {"state": "verified"}, "stress": {"state": "verified"}}
                }
              }
            }
          }
        },
        "sources": {
          "studies": {"48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": "verified"},
          "plans": {"8aaebf023671101aa76a34a61365e8e568946d76a8d1d3e9272c8ca38908043b": "verified"},
          "collections": {"92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879": "verified"},
          "datasets": {"d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a": "verified"},
          "features": {"f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5": "verified"},
          "labels": {"78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6": "verified"},
          "schedules": {
            "106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9": "verified",
            "79c7c2f9c60ddbe467a36235194ae84daeb9337111803c678b9650d6e610b184": "verified"
          },
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": "verified",
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": "verified",
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": "invalid: journal.ndjson differs from its manifest's size or checksum",
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": "verified"
          }
        },
        "state": "invalid"
      }
    }
  },
  "schedule": {
    "artifact": {"detail": "the manifest's identity and every file's checksum and row count, read now", "state": "verified"},
    "engine_replay": {
      "detail": "each run's replay by chronos_strategy inspect was recorded by its study when the study ran; diagnostics never replay or refit",
      "state": "recorded_by_study"
    },
    "independent_audit": {
      "command": "python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> <diagnostic_id>",
      "detail": "a standard-library recomputation of every figure from the raw files, run from the CLI",
      "state": "not_run_by_this_read"
    },
    "recomputed_inspection": {
      "command": "python -m chronos_research inspect-strategy-diagnostics --tools <tools> --artifacts <artifacts> --diagnostics <diagnostic_id>",
      "detail": "rebuilds the artifact from its sources, verified again, and requires identical bytes",
      "state": "not_run_by_this_read"
    },
    "sources": {
      "at_generation": "at generation: the study and plan by identity; the plan rebuilt from its request over the collection's members, each verified by the C++ tools; every schedule's rows and checksum; every run's manifest identity and journal checksum, and that it is its cell's run; every dataset's events by checksum. Engine replay is the study's own record; nothing here replays or refits",
      "now": {
        "content": {"hashed_now": "1", "reused_unchanged": "7"},
        "detail": "checked by this read: each study's, plan's, and collection's identity and what it names; each dataset's, feature artifact's, label artifact's, schedule's, and run's manifest identity, its bindings to the others, and the size and SHA-256 of the file the diagnostics consumed, compared with what they read. A SHA-256 is computed now, or reused from this service's own earlier hash of the same file while its device, inode, size, modification time, and change time are unchanged. A figure is verified only if every source it depends on is. Nothing is rebuilt, audited, or replayed here",
        "figures": {
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": {"state": "verified"},
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": {"state": "verified"},
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": {
              "state": "invalid",
              "sources": ["schedules:106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9"]
            },
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": {
              "state": "invalid",
              "sources": ["schedules:106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9"]
            }
          },
          "studies": {
            "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": {
              "folds": {
                "1": {
                  "logistic": {
                    "state": "invalid",
                    "sources": ["schedules:106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9"]
                  },
                  "majority": {"state": "verified"}
                }
              },
              "pooled": {
                "predictions": {
                  "logistic": {
                    "state": "invalid",
                    "sources": ["schedules:106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9"]
                  },
                  "majority": {"state": "verified"}
                },
                "trading": {
                  "logistic": {
                    "base": {
                      "state": "invalid",
                      "sources": ["schedules:106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9"]
                    },
                    "stress": {
                      "state": "invalid",
                      "sources": ["schedules:106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9"]
                    }
                  },
                  "majority": {"base": {"state": "verified"}, "stress": {"state": "verified"}}
                }
              }
            }
          }
        },
        "sources": {
          "studies": {"48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": "verified"},
          "plans": {"8aaebf023671101aa76a34a61365e8e568946d76a8d1d3e9272c8ca38908043b": "verified"},
          "collections": {"92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879": "verified"},
          "datasets": {"d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a": "verified"},
          "features": {"f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5": "verified"},
          "labels": {"78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6": "verified"},
          "schedules": {
            "106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9": "invalid: schedule.ndjson differs from its manifest's size or checksum",
            "79c7c2f9c60ddbe467a36235194ae84daeb9337111803c678b9650d6e610b184": "verified"
          },
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": "verified",
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": "verified",
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": "verified",
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": "verified"
          }
        },
        "state": "invalid"
      }
    }
  },
  "study": {
    "artifact": {"detail": "the manifest's identity and every file's checksum and row count, read now", "state": "verified"},
    "engine_replay": {
      "detail": "each run's replay by chronos_strategy inspect was recorded by its study when the study ran; diagnostics never replay or refit",
      "state": "recorded_by_study"
    },
    "independent_audit": {
      "command": "python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> <diagnostic_id>",
      "detail": "a standard-library recomputation of every figure from the raw files, run from the CLI",
      "state": "not_run_by_this_read"
    },
    "recomputed_inspection": {
      "command": "python -m chronos_research inspect-strategy-diagnostics --tools <tools> --artifacts <artifacts> --diagnostics <diagnostic_id>",
      "detail": "rebuilds the artifact from its sources, verified again, and requires identical bytes",
      "state": "not_run_by_this_read"
    },
    "sources": {
      "at_generation": "at generation: the study and plan by identity; the plan rebuilt from its request over the collection's members, each verified by the C++ tools; every schedule's rows and checksum; every run's manifest identity and journal checksum, and that it is its cell's run; every dataset's events by checksum. Engine replay is the study's own record; nothing here replays or refits",
      "now": {
        "content": {"hashed_now": "1", "reused_unchanged": "8"},
        "detail": "checked by this read: each study's, plan's, and collection's identity and what it names; each dataset's, feature artifact's, label artifact's, schedule's, and run's manifest identity, its bindings to the others, and the size and SHA-256 of the file the diagnostics consumed, compared with what they read. A SHA-256 is computed now, or reused from this service's own earlier hash of the same file while its device, inode, size, modification time, and change time are unchanged. A figure is verified only if every source it depends on is. Nothing is rebuilt, audited, or replayed here",
        "figures": {
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": {
              "state": "invalid",
              "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
            },
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": {
              "state": "invalid",
              "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
            },
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": {
              "state": "invalid",
              "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
            },
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": {
              "state": "invalid",
              "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
            }
          },
          "studies": {
            "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": {
              "folds": {
                "1": {
                  "logistic": {
                    "state": "invalid",
                    "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                  },
                  "majority": {
                    "state": "invalid",
                    "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                  }
                }
              },
              "pooled": {
                "predictions": {
                  "logistic": {
                    "state": "invalid",
                    "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                  },
                  "majority": {
                    "state": "invalid",
                    "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                  }
                },
                "trading": {
                  "logistic": {
                    "base": {
                      "state": "invalid",
                      "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                    },
                    "stress": {
                      "state": "invalid",
                      "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                    }
                  },
                  "majority": {
                    "base": {
                      "state": "invalid",
                      "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                    },
                    "stress": {
                      "state": "invalid",
                      "sources": ["studies:48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388"]
                    }
                  }
                }
              }
            }
          }
        },
        "sources": {
          "studies": {
            "48243b5c5e40b72dc3f2125f3402e173cd89ad7073bd290fa18b17283d8f6388": "invalid: manifest.json is not canonical"
          },
          "plans": {"8aaebf023671101aa76a34a61365e8e568946d76a8d1d3e9272c8ca38908043b": "verified"},
          "collections": {"92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879": "verified"},
          "datasets": {"d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a": "verified"},
          "features": {"f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5": "verified"},
          "labels": {"78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6": "verified"},
          "schedules": {
            "106920393565d6b70d9351f301067e8b85c952739bc9588f7dc98833fede84f9": "verified",
            "79c7c2f9c60ddbe467a36235194ae84daeb9337111803c678b9650d6e610b184": "verified"
          },
          "runs": {
            "2bee9c63f00d3add403be921cda15b19e3663880e71a45fc028ef143b3b8a78f": "verified",
            "959877146842ce48de5b80d9379eaa11c0fb9f1fe2f47ec289a84a797b2b7765": "verified",
            "91cae68f9e1f2465468b59aa2021462b1f44492ea3416b1d3f661d840542b588": "verified",
            "128b5c46b55c1cbd568ace261f7840defaf531f4588f022542b66b1fe5ba7084": "verified"
          }
        },
        "state": "invalid"
      }
    }
  }
};

function copy<T>(value: T): T {
  return structuredClone(value);
}

export function diagnosticsDetailJson(unverified?: 'run' | 'schedule' | 'study'): Json {
  const detail = copy(DETAIL);
  return unverified === undefined ? detail : { ...detail, verification: copy(UNVERIFIED[unverified]) };
}

export function diagnosticsPageJson(): Json {
  return copy(LISTING);
}

export function emptyDiagnosticsPageJson(): Json {
  return { diagnostics: [], invalid_entries: [], next_cursor: null };
}

export function fillPageJson(page: 'first' | 'next' | 'buy_and_hold' = 'first'): Json {
  return copy(page === 'first' ? FILLS_FIRST : page === 'next' ? FILLS_NEXT : HOLDER_FILLS);
}

export function episodePageJson(): Json {
  return copy(EPISODES);
}

// A diagnostics job of the study: by default succeeded with the diagnostics.
export function diagnosticsJobJson(changes: Json = {}): Json {
  return {
    created_at: '2026-10-12T15:00:00.000000000Z',
    error: null,
    finished_at: '2026-10-12T15:00:02.000000000Z',
    job_id: DIAGNOSTICS_JOB_ID,
    kind: 'strategy_diagnostics',
    log: '',
    progress: { stage: 'publish' },
    request: { study_ids: [DIAGNOSED_STUDY_ID] },
    result: { diagnostic_id: DIAGNOSTIC_ID, outcome: 'published', status: 'complete', study_ids: [DIAGNOSED_STUDY_ID] },
    started_at: '2026-10-12T15:00:00.100000000Z',
    state: 'succeeded',
    ...changes,
  };
}
