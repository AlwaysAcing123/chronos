// Walk-forward resources exactly as the research service returned them over
// synthetic recordings (three generated days; a thin day whose logistic fit is
// unevaluable; one day alone). Generated from a scratch run of the real service;
// shared by the component tests and the mocked browser suite. The partial study
// holds every outcome a study shows: completed, unevaluable, and not-run cells;
// complete, unavailable, and partial totals; complete and partial pools.
import type { Json } from './fixtures';

export const WEEK_COLLECTION_ID = '92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879';
export const ONE_RECORDING_COLLECTION_ID = 'deba665a20df808170e3995ee148f8d810989f03204fd536099ea877ebd2d1b9';
export const PARTIAL_COLLECTION_ID = 'a823b3caab14b2dd1f6f6d5ea34f76e4aeadea07243189e25212ca0a1c01c5d5';
export const PLAN_ID = '0be48271f72edb602469f66c7fabb28ab92fde1ae214235be52f9ca65149e33e';
export const COMPLETE_STUDY_ID = '67067b4f7f7a939f6b58fa3e9fcf6df09e77fb6821fcd4b7329f23cc27bd53be';
export const PARTIAL_STUDY_ID = 'b362061df2556445e9622708767e6571f80e7cbce341616bf7943b6e71d2e4c9';
export const PLAN_JOB_ID = '441eaae21aa9c8b1795ae82d09628e35';
export const STUDY_JOB_ID = '740e1d1f1c3f460f75f51fc6c0e7f61e';
export const FOLD_RUN_ID = 'd5959e9a651c30f8a6aff57c94f0289d120d42a0dc4591a7944029a717159dd6';

const COLLECTIONS: readonly Json[] = [
  {
    "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
    "contract": {"feature_set": "top_of_book_v1", "horizon_ns": "1000000", "label_set": "future_midpoint_return_v1"},
    "definitions": {
      "completeness": "partial: Chronos has no exchange calendar, so no recording is called a complete session; each member states its actual receive-time coverage",
      "coverage": "receive-time range of the dataset's events; first and last available_at per instrument",
      "evidence": "synthetic: the test generator's events; observed: a provider recording normalized by a known converter; neither claims more than its events show",
      "identity": "rows are identified by stream, instrument, and ingest sequence; nothing is renumbered",
      "order": "members by first receive time, strictly disjoint: a member ends before the next begins",
      "session": "a session or date label is the creator's declaration, not proof of a session"
    },
    "evidence": "synthetic",
    "instruments": {"AAPL": "3", "SPY": "3"},
    "members": [
      {
        "coverage": {"completeness": "partial", "duration_ns": "175768113"},
        "dataset_id": "2d44accc13eda244d4fac00c78a912c07b58ea7dfeb29b281a613f61704dd7e5",
        "events": "840",
        "feature_artifact_id": "3e2f8323a411b08f2a139771fad47f4f652077d558504f38e969725eed9e1093",
        "instruments": {
          "AAPL": {
            "feature_rows": "420",
            "first_available_at": "2026-09-21T13:30:00.000096830Z",
            "last_available_at": "2026-09-21T13:30:00.175860977Z",
            "usable_labels": "416"
          },
          "SPY": {
            "feature_rows": "420",
            "first_available_at": "2026-09-21T13:30:00.000096830Z",
            "last_available_at": "2026-09-21T13:30:00.175864943Z",
            "usable_labels": "416"
          }
        },
        "label_artifact_id": "5cc4ec8d1eddac2f707c7e06c05cdcfe64e5a9abfa62bd04ebaba98d90a27fdc",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-21T13:30:00.000096830Z", "latest": "2026-09-21T13:30:00.175864943Z"},
        "session": {"unavailable": "not_declared"},
        "stream": "synthetic-2026-09-21"
      },
      {
        "coverage": {"completeness": "partial", "duration_ns": "139026639"},
        "dataset_id": "1ed1c1d58ead1e6a8b97a4c4f2e6ff09b24562bca7f060f95e2fc86d4aa8bf30",
        "events": "720",
        "feature_artifact_id": "7cc1faed167879a67259be4fedc7905919675835a854a5abf8e5722b8546f8f0",
        "instruments": {
          "AAPL": {
            "feature_rows": "360",
            "first_available_at": "2026-09-22T13:30:00.000264444Z",
            "last_available_at": "2026-09-22T13:30:00.139263199Z",
            "usable_labels": "354"
          },
          "SPY": {
            "feature_rows": "360",
            "first_available_at": "2026-09-22T13:30:00.000264444Z",
            "last_available_at": "2026-09-22T13:30:00.139291083Z",
            "usable_labels": "354"
          }
        },
        "label_artifact_id": "74c11c44acf05237e0edceec003cb27edda81b005ef246cc946ea0826239a276",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-22T13:30:00.000264444Z", "latest": "2026-09-22T13:30:00.139291083Z"},
        "session": {"unavailable": "not_declared"},
        "stream": "synthetic-2026-09-22"
      },
      {
        "coverage": {"completeness": "partial", "duration_ns": "210395615"},
        "dataset_id": "d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a",
        "events": "1000",
        "feature_artifact_id": "f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5",
        "instruments": {
          "AAPL": {
            "feature_rows": "500",
            "first_available_at": "2026-09-23T13:30:00.000363583Z",
            "last_available_at": "2026-09-23T13:30:00.210734812Z",
            "usable_labels": "494"
          },
          "SPY": {
            "feature_rows": "500",
            "first_available_at": "2026-09-23T13:30:00.000363583Z",
            "last_available_at": "2026-09-23T13:30:00.210759198Z",
            "usable_labels": "494"
          }
        },
        "label_artifact_id": "78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-23T13:30:00.000363583Z", "latest": "2026-09-23T13:30:00.210759198Z"},
        "session": {"unavailable": "not_declared"},
        "stream": "synthetic-2026-09-23"
      }
    ],
    "verification": [
      "each label artifact reproduced from its feature artifact by chronos_dataset labels inspect --features",
      "each feature artifact reproduced from its dataset by chronos_dataset features inspect --catalog",
      "each dataset's every event and quality report verified by chronos_dataset inspect",
      "the label, feature, and dataset IDs, checksums, stream, and row counts cross-checked",
      "the counts and coverage here computed from feature and label rows read while hashing, used only if they are exactly the verified bytes"
    ]
  },
  {
    "collection_id": "a823b3caab14b2dd1f6f6d5ea34f76e4aeadea07243189e25212ca0a1c01c5d5",
    "contract": {"feature_set": "top_of_book_v1", "horizon_ns": "1000000", "label_set": "future_midpoint_return_v1"},
    "definitions": {
      "completeness": "partial: Chronos has no exchange calendar, so no recording is called a complete session; each member states its actual receive-time coverage",
      "coverage": "receive-time range of the dataset's events; first and last available_at per instrument",
      "evidence": "synthetic: the test generator's events; observed: a provider recording normalized by a known converter; neither claims more than its events show",
      "identity": "rows are identified by stream, instrument, and ingest sequence; nothing is renumbered",
      "order": "members by first receive time, strictly disjoint: a member ends before the next begins",
      "session": "a session or date label is the creator's declaration, not proof of a session"
    },
    "evidence": "synthetic",
    "instruments": {"AAPL": "3", "SPY": "3"},
    "members": [
      {
        "coverage": {"completeness": "partial", "duration_ns": "5873422"},
        "dataset_id": "4b0efb6cc69e762ab54eccb418b5ae5291c244fe255b3c098fad0237efa7579c",
        "events": "48",
        "feature_artifact_id": "2dd3194cb4b83b880b10da6f93b9ee5b515ac7dedc1f47902e0a383f4e4c6ea0",
        "instruments": {
          "AAPL": {
            "feature_rows": "24",
            "first_available_at": "2026-09-19T13:30:00.000319935Z",
            "last_available_at": "2026-09-19T13:30:00.006154117Z",
            "usable_labels": "19"
          },
          "SPY": {
            "feature_rows": "24",
            "first_available_at": "2026-09-19T13:30:00.000319935Z",
            "last_available_at": "2026-09-19T13:30:00.006193357Z",
            "usable_labels": "19"
          }
        },
        "label_artifact_id": "cf0ee9090e8bc4d6cd222dfc7a67fb7460739fc2a368a7d3ce0c5ef765dd882c",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-19T13:30:00.000319935Z", "latest": "2026-09-19T13:30:00.006193357Z"},
        "session": {"basis": "declared", "label": "2026-09-19 (synthetic, declared)"},
        "stream": "synthetic-2026-09-19-thin"
      },
      {
        "coverage": {"completeness": "partial", "duration_ns": "175768113"},
        "dataset_id": "2d44accc13eda244d4fac00c78a912c07b58ea7dfeb29b281a613f61704dd7e5",
        "events": "840",
        "feature_artifact_id": "3e2f8323a411b08f2a139771fad47f4f652077d558504f38e969725eed9e1093",
        "instruments": {
          "AAPL": {
            "feature_rows": "420",
            "first_available_at": "2026-09-21T13:30:00.000096830Z",
            "last_available_at": "2026-09-21T13:30:00.175860977Z",
            "usable_labels": "416"
          },
          "SPY": {
            "feature_rows": "420",
            "first_available_at": "2026-09-21T13:30:00.000096830Z",
            "last_available_at": "2026-09-21T13:30:00.175864943Z",
            "usable_labels": "416"
          }
        },
        "label_artifact_id": "5cc4ec8d1eddac2f707c7e06c05cdcfe64e5a9abfa62bd04ebaba98d90a27fdc",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-21T13:30:00.000096830Z", "latest": "2026-09-21T13:30:00.175864943Z"},
        "session": {"unavailable": "not_declared"},
        "stream": "synthetic-2026-09-21"
      },
      {
        "coverage": {"completeness": "partial", "duration_ns": "139026639"},
        "dataset_id": "1ed1c1d58ead1e6a8b97a4c4f2e6ff09b24562bca7f060f95e2fc86d4aa8bf30",
        "events": "720",
        "feature_artifact_id": "7cc1faed167879a67259be4fedc7905919675835a854a5abf8e5722b8546f8f0",
        "instruments": {
          "AAPL": {
            "feature_rows": "360",
            "first_available_at": "2026-09-22T13:30:00.000264444Z",
            "last_available_at": "2026-09-22T13:30:00.139263199Z",
            "usable_labels": "354"
          },
          "SPY": {
            "feature_rows": "360",
            "first_available_at": "2026-09-22T13:30:00.000264444Z",
            "last_available_at": "2026-09-22T13:30:00.139291083Z",
            "usable_labels": "354"
          }
        },
        "label_artifact_id": "74c11c44acf05237e0edceec003cb27edda81b005ef246cc946ea0826239a276",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-22T13:30:00.000264444Z", "latest": "2026-09-22T13:30:00.139291083Z"},
        "session": {"unavailable": "not_declared"},
        "stream": "synthetic-2026-09-22"
      }
    ],
    "verification": [
      "each label artifact reproduced from its feature artifact by chronos_dataset labels inspect --features",
      "each feature artifact reproduced from its dataset by chronos_dataset features inspect --catalog",
      "each dataset's every event and quality report verified by chronos_dataset inspect",
      "the label, feature, and dataset IDs, checksums, stream, and row counts cross-checked",
      "the counts and coverage here computed from feature and label rows read while hashing, used only if they are exactly the verified bytes"
    ]
  },
  {
    "collection_id": "deba665a20df808170e3995ee148f8d810989f03204fd536099ea877ebd2d1b9",
    "contract": {"feature_set": "top_of_book_v1", "horizon_ns": "1000000", "label_set": "future_midpoint_return_v1"},
    "definitions": {
      "completeness": "partial: Chronos has no exchange calendar, so no recording is called a complete session; each member states its actual receive-time coverage",
      "coverage": "receive-time range of the dataset's events; first and last available_at per instrument",
      "evidence": "synthetic: the test generator's events; observed: a provider recording normalized by a known converter; neither claims more than its events show",
      "identity": "rows are identified by stream, instrument, and ingest sequence; nothing is renumbered",
      "order": "members by first receive time, strictly disjoint: a member ends before the next begins",
      "session": "a session or date label is the creator's declaration, not proof of a session"
    },
    "evidence": "synthetic",
    "instruments": {"AAPL": "1", "SPY": "1"},
    "members": [
      {
        "coverage": {"completeness": "partial", "duration_ns": "175768113"},
        "dataset_id": "2d44accc13eda244d4fac00c78a912c07b58ea7dfeb29b281a613f61704dd7e5",
        "events": "840",
        "feature_artifact_id": "3e2f8323a411b08f2a139771fad47f4f652077d558504f38e969725eed9e1093",
        "instruments": {
          "AAPL": {
            "feature_rows": "420",
            "first_available_at": "2026-09-21T13:30:00.000096830Z",
            "last_available_at": "2026-09-21T13:30:00.175860977Z",
            "usable_labels": "416"
          },
          "SPY": {
            "feature_rows": "420",
            "first_available_at": "2026-09-21T13:30:00.000096830Z",
            "last_available_at": "2026-09-21T13:30:00.175864943Z",
            "usable_labels": "416"
          }
        },
        "label_artifact_id": "5cc4ec8d1eddac2f707c7e06c05cdcfe64e5a9abfa62bd04ebaba98d90a27fdc",
        "provenance": {
          "description": "synthetic events for tests",
          "evidence": "synthetic",
          "provider": "synthetic",
          "provider_dataset": "SYNTHETIC"
        },
        "receive_time": {"earliest": "2026-09-21T13:30:00.000096830Z", "latest": "2026-09-21T13:30:00.175864943Z"},
        "session": {"unavailable": "not_declared"},
        "stream": "synthetic-2026-09-21"
      }
    ],
    "verification": [
      "each label artifact reproduced from its feature artifact by chronos_dataset labels inspect --features",
      "each feature artifact reproduced from its dataset by chronos_dataset features inspect --catalog",
      "each dataset's every event and quality report verified by chronos_dataset inspect",
      "the label, feature, and dataset IDs, checksums, stream, and row counts cross-checked",
      "the counts and coverage here computed from feature and label rows read while hashing, used only if they are exactly the verified bytes"
    ]
  }
];

const PLAN: Json = {
  "class_order": ["down", "flat", "up"],
  "collection": {
    "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
    "evidence": "synthetic",
    "skipped_members": []
  },
  "configuration": {
    "decision_interval_ns": "1000000",
    "fee_per_fill": "0.01",
    "latency_ns": "1000",
    "maximum_order_notional": "1000000",
    "maximum_quote_age_ns": "5000000000",
    "starting_cash": "10000",
    "target_quantity": "1",
    "threshold_ppb": "400000000"
  },
  "definitions": {
    "information_boundary": "the evaluation window's start, exclusive: every training prediction time and every training label's availability time is strictly before it",
    "membership": "expanding: fold k trains on every earlier member of the collection holding the instrument and evaluates the next one",
    "retraining": "a later fold learns from earlier evaluation recordings once their observations and labels are history; this is predeclared chronological retraining, not tuning",
    "training_row": "a feature row of the instrument in a training member, with available inputs and a usable label, whose prediction_at_ns and label_available_at_ns are both strictly before the boundary; each other row is excluded with the first reason that applies, in the order feature_unavailable, label_unavailable, prediction_at_not_before_boundary, label_available_at_not_before_boundary",
    "window": "[the evaluation recording's first receive time, its last receive time + 1)"
  },
  "folds": [
    {
      "evaluation": {
        "dataset_id": "1ed1c1d58ead1e6a8b97a4c4f2e6ff09b24562bca7f060f95e2fc86d4aa8bf30",
        "feature_artifact_id": "7cc1faed167879a67259be4fedc7905919675835a854a5abf8e5722b8546f8f0",
        "feature_rows": "360",
        "label_artifact_id": "74c11c44acf05237e0edceec003cb27edda81b005ef246cc946ea0826239a276",
        "stream": "synthetic-2026-09-22",
        "window": {
          "end_at": "2026-09-22T13:30:00.139291084Z",
          "name": "fold_1",
          "start_at": "2026-09-22T13:30:00.000264444Z"
        }
      },
      "index": "1",
      "information_boundary": "2026-09-22T13:30:00.000264444Z",
      "training": {
        "class_support": {"down": "124", "flat": "78", "up": "214"},
        "eligible_rows": "416",
        "max_label_available_at": "2026-09-21T13:30:00.175860977Z",
        "max_prediction_at": "2026-09-21T13:30:00.174832306Z",
        "members": [
          {
            "dataset_id": "2d44accc13eda244d4fac00c78a912c07b58ea7dfeb29b281a613f61704dd7e5",
            "eligible_rows": "416",
            "excluded": {"label_unavailable:no_future_observation": "4"},
            "feature_rows": "420",
            "interval": {"end_at": "2026-09-21T13:30:00.175864944Z", "start_at": "2026-09-21T13:30:00.000096830Z"},
            "label_artifact_id": "5cc4ec8d1eddac2f707c7e06c05cdcfe64e5a9abfa62bd04ebaba98d90a27fdc",
            "stream": "synthetic-2026-09-21"
          }
        ]
      }
    },
    {
      "evaluation": {
        "dataset_id": "d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a",
        "feature_artifact_id": "f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5",
        "feature_rows": "500",
        "label_artifact_id": "78f6e1562a4b90015b295364500a15444ebaafee167f4bd4441a5cc338c5cef6",
        "stream": "synthetic-2026-09-23",
        "window": {
          "end_at": "2026-09-23T13:30:00.210759199Z",
          "name": "fold_2",
          "start_at": "2026-09-23T13:30:00.000363583Z"
        }
      },
      "index": "2",
      "information_boundary": "2026-09-23T13:30:00.000363583Z",
      "training": {
        "class_support": {"down": "346", "flat": "128", "up": "296"},
        "eligible_rows": "770",
        "max_label_available_at": "2026-09-22T13:30:00.139211324Z",
        "max_prediction_at": "2026-09-22T13:30:00.138122322Z",
        "members": [
          {
            "dataset_id": "2d44accc13eda244d4fac00c78a912c07b58ea7dfeb29b281a613f61704dd7e5",
            "eligible_rows": "416",
            "excluded": {"label_unavailable:no_future_observation": "4"},
            "feature_rows": "420",
            "interval": {"end_at": "2026-09-21T13:30:00.175864944Z", "start_at": "2026-09-21T13:30:00.000096830Z"},
            "label_artifact_id": "5cc4ec8d1eddac2f707c7e06c05cdcfe64e5a9abfa62bd04ebaba98d90a27fdc",
            "stream": "synthetic-2026-09-21"
          },
          {
            "dataset_id": "1ed1c1d58ead1e6a8b97a4c4f2e6ff09b24562bca7f060f95e2fc86d4aa8bf30",
            "eligible_rows": "354",
            "excluded": {"label_unavailable:no_future_observation": "6"},
            "feature_rows": "360",
            "interval": {"end_at": "2026-09-22T13:30:00.139291084Z", "start_at": "2026-09-22T13:30:00.000264444Z"},
            "label_artifact_id": "74c11c44acf05237e0edceec003cb27edda81b005ef246cc946ea0826239a276",
            "stream": "synthetic-2026-09-22"
          }
        ]
      }
    }
  ],
  "horizon_ns": "1000000",
  "instrument": "AAPL",
  "model_feature_set": {
    "inputs": [
      {"definition": "feature spread", "name": "spread", "scale": "1000000000"},
      {"definition": "feature imbalance", "name": "imbalance", "scale": "1000000000"},
      {
        "definition": "feature microprice minus feature midpoint, exact",
        "name": "microprice_minus_midpoint",
        "scale": "1000000000"
      },
      {"definition": "feature bid_size", "name": "bid_size", "scale": "100000000"},
      {"definition": "feature ask_size", "name": "ask_size", "scale": "100000000"}
    ],
    "name": "top_of_book_baseline",
    "version": "1"
  },
  "models": {
    "logistic": {
      "class_weight": null,
      "estimator": "sklearn.linear_model.LogisticRegression 1.9.1",
      "fit_intercept": true,
      "l1_ratio": "0.0",
      "maximum_iterations": "1000",
      "minimum_training_rows_per_class": "10",
      "multinomial": true,
      "random_state": "0",
      "regularization_c": "1.0",
      "scaler": "sklearn.preprocessing.StandardScaler, fitted on training rows",
      "solver": "lbfgs",
      "threads": "1",
      "tolerance": "1e-06"
    },
    "majority": {
      "prediction": "most frequent training class; ties go to the earliest class in class_order",
      "probabilities": "training class frequencies"
    }
  },
  "plan_id": "0be48271f72edb602469f66c7fabb28ab92fde1ae214235be52f9ca65149e33e",
  "request": {
    "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
    "configuration": {
      "decision_interval_ns": "1000000",
      "fee_per_fill": "0.01",
      "latency_ns": "1000",
      "maximum_order_notional": "1000000",
      "maximum_quote_age_ns": "5000000000",
      "starting_cash": "10000",
      "target_quantity": "1",
      "threshold_ppb": "400000000"
    },
    "instrument": "AAPL",
    "minimum_training_sources": "1",
    "scenarios": [
      {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
      {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"}
    ],
    "schema": {"name": "chronos.walk_forward_request", "version": "1"}
  },
  "request_sha256": "4f12ea88308951349fd94e120640514fd85db63daa7b2c18758616bc5e4bf140",
  "scenario_rules": [
    "every scenario runs the same predictions through the C++ engine, with only the fee per fill and the positive latency changed; spreads and recorded prices are never changed",
    "the base scenario is the configuration's own fee and latency",
    "the stress scenario is no cheaper and no faster than the base on either axis, and costlier or slower on at least one",
    "a zero fee is used only where a scenario names it"
  ],
  "scenarios": [
    {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
    {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"}
  ],
  "schema": {"name": "chronos.walk_forward_plan", "version": "1"},
  "scoring": {
    "cutoff": "the evaluation window's end, exclusive",
    "rule": "a scheduled row is scored when its label is usable and label_available_at_ns < end_at_ns",
    "unscored": "every other scheduled row is counted by reason: feature_unavailable:<reason>, label_unavailable:<reason>, or label_after_scoring_cutoff"
  },
  "workload": {
    "caps": {
      "feature_rows_per_source": "2000000",
      "folds": "10",
      "scenarios": "4",
      "sources": "12",
      "strategy_runs": "80",
      "training_rows_per_fold": "4000000"
    },
    "evaluation_rows": "860",
    "folds": "2",
    "models": "2",
    "scenarios": "2",
    "schedules": "4",
    "sources": "3",
    "strategy_runs": "8",
    "training_rows": "1186"
  }
};

const PARTIAL_STUDY_DETAIL: Json = {
  "artifacts": [
    {
      "fold": "1",
      "fold_model": "verified",
      "models": {
        "logistic": {"runs": {}},
        "majority": {"runs": {"base": "verified", "stress": "verified"}, "schedule": "verified"}
      }
    },
    {
      "fold": "2",
      "fold_model": "verified",
      "models": {
        "logistic": {"runs": {"base": "verified", "stress": "verified"}, "schedule": "verified"},
        "majority": {"runs": {"base": "verified", "stress": "verified"}, "schedule": "verified"}
      }
    }
  ],
  "study": {
    "aggregates": {
      "predictive": {
        "logistic": {
          "folds": {"included": ["2"], "planned": "2"},
          "pooled": {
            "metrics": {
              "accuracy": {"value": "0.23163841807909605"},
              "balanced_accuracy": {"value": "0.3333333333333333"},
              "brier_score": {"value": "0.7242112192844746"},
              "confusion_matrix": {
                "down": {"down": "0", "flat": "0", "up": "222"},
                "flat": {"down": "0", "flat": "0", "up": "50"},
                "up": {"down": "0", "flat": "0", "up": "82"}
              },
              "log_loss": {"value": "1.1510684088927292"},
              "per_class": {
                "down": {
                  "f1": {"value": "0.0"},
                  "precision": {"undefined": "class_never_predicted"},
                  "recall": {"value": "0.0"}
                },
                "flat": {
                  "f1": {"value": "0.0"},
                  "precision": {"undefined": "class_never_predicted"},
                  "recall": {"value": "0.0"}
                },
                "up": {
                  "f1": {"value": "0.3761467889908257"},
                  "precision": {"value": "0.23163841807909605"},
                  "recall": {"value": "1.0"}
                }
              },
              "predicted_distribution": {"down": "0", "flat": "0", "up": "354"},
              "rows": "354",
              "true_distribution": {"down": "222", "flat": "50", "up": "82"}
            },
            "rows": {
              "not_scored": {"label_unavailable:no_future_observation": "6"},
              "scheduled": "360",
              "scored": "354"
            }
          },
          "status": "partial"
        },
        "majority": {
          "folds": {"included": ["1", "2"], "planned": "2"},
          "pooled": {
            "metrics": {
              "accuracy": {"value": "0.38441558441558443"},
              "balanced_accuracy": {"value": "0.3333333333333333"},
              "brier_score": {"value": "0.6892526803699166"},
              "confusion_matrix": {
                "down": {"down": "0", "flat": "0", "up": "346"},
                "flat": {"down": "0", "flat": "0", "up": "128"},
                "up": {"down": "0", "flat": "0", "up": "296"}
              },
              "log_loss": {"value": "1.1163584613272706"},
              "per_class": {
                "down": {
                  "f1": {"value": "0.0"},
                  "precision": {"undefined": "class_never_predicted"},
                  "recall": {"value": "0.0"}
                },
                "flat": {
                  "f1": {"value": "0.0"},
                  "precision": {"undefined": "class_never_predicted"},
                  "recall": {"value": "0.0"}
                },
                "up": {
                  "f1": {"value": "0.5553470919324578"},
                  "precision": {"value": "0.38441558441558443"},
                  "recall": {"value": "1.0"}
                }
              },
              "predicted_distribution": {"down": "0", "flat": "0", "up": "770"},
              "rows": "770",
              "true_distribution": {"down": "346", "flat": "128", "up": "296"}
            },
            "rows": {
              "not_scored": {"label_unavailable:no_future_observation": "10"},
              "scheduled": "780",
              "scored": "770"
            }
          },
          "status": "complete"
        }
      },
      "trading": {
        "logistic": {
          "base": {
            "complete": {
              "missing": [{"fold": "1", "reason": "cell_unavailable:model_unevaluable"}],
              "unavailable": "folds_missing"
            },
            "folds": {"included": ["2"], "planned": "2"},
            "partial": {
              "accounts": {
                "buy_and_hold": {
                  "fees_paid": "0.01",
                  "fills": "1",
                  "net_total_pnl": "-0.77",
                  "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                  "return_percent": "-0.0077",
                  "turnover": "99.99"
                },
                "cash": {
                  "fees_paid": "0",
                  "fills": "0",
                  "net_total_pnl": "0",
                  "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                  "return_percent": "0",
                  "turnover": "0"
                },
                "strategy": {
                  "fees_paid": "0.01",
                  "fills": "1",
                  "net_total_pnl": "-0.77",
                  "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                  "return_percent": "-0.0077",
                  "turnover": "99.99"
                }
              },
              "deltas": {
                "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                "cash": {"net_total_pnl": "-0.77", "return_percent": "-0.0077"}
              },
              "starting_capital": "10000"
            }
          },
          "stress": {
            "complete": {
              "missing": [{"fold": "1", "reason": "cell_unavailable:model_unevaluable"}],
              "unavailable": "folds_missing"
            },
            "folds": {"included": ["2"], "planned": "2"},
            "partial": {
              "accounts": {
                "buy_and_hold": {
                  "fees_paid": "0.05",
                  "fills": "1",
                  "net_total_pnl": "-0.8",
                  "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                  "return_percent": "-0.008",
                  "turnover": "99.98"
                },
                "cash": {
                  "fees_paid": "0",
                  "fills": "0",
                  "net_total_pnl": "0",
                  "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                  "return_percent": "0",
                  "turnover": "0"
                },
                "strategy": {
                  "fees_paid": "0.05",
                  "fills": "1",
                  "net_total_pnl": "-0.8",
                  "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                  "return_percent": "-0.008",
                  "turnover": "99.98"
                }
              },
              "deltas": {
                "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                "cash": {"net_total_pnl": "-0.8", "return_percent": "-0.008"}
              },
              "starting_capital": "10000"
            }
          }
        },
        "majority": {
          "base": {
            "complete": {
              "accounts": {
                "buy_and_hold": {
                  "fees_paid": "0.02",
                  "fills": "2",
                  "net_total_pnl": "-0.28",
                  "orders": {"expired": "0", "filled": "2", "rejected": "0", "scheduled": "2"},
                  "return_percent": "-0.0014",
                  "turnover": "200.01"
                },
                "cash": {
                  "fees_paid": "0",
                  "fills": "0",
                  "net_total_pnl": "0",
                  "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                  "return_percent": "0",
                  "turnover": "0"
                },
                "strategy": {
                  "fees_paid": "0.02",
                  "fills": "2",
                  "net_total_pnl": "-0.28",
                  "orders": {"expired": "0", "filled": "2", "rejected": "0", "scheduled": "2"},
                  "return_percent": "-0.0014",
                  "turnover": "200.01"
                }
              },
              "deltas": {
                "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                "cash": {"net_total_pnl": "-0.28", "return_percent": "-0.0014"}
              },
              "starting_capital": "20000"
            },
            "folds": {"included": ["1", "2"], "planned": "2"}
          },
          "stress": {
            "complete": {
              "accounts": {
                "buy_and_hold": {
                  "fees_paid": "0.1",
                  "fills": "2",
                  "net_total_pnl": "-0.36",
                  "orders": {"expired": "0", "filled": "2", "rejected": "0", "scheduled": "2"},
                  "return_percent": "-0.0018",
                  "turnover": "200.01"
                },
                "cash": {
                  "fees_paid": "0",
                  "fills": "0",
                  "net_total_pnl": "0",
                  "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                  "return_percent": "0",
                  "turnover": "0"
                },
                "strategy": {
                  "fees_paid": "0.1",
                  "fills": "2",
                  "net_total_pnl": "-0.36",
                  "orders": {"expired": "0", "filled": "2", "rejected": "0", "scheduled": "2"},
                  "return_percent": "-0.0018",
                  "turnover": "200.01"
                }
              },
              "deltas": {
                "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                "cash": {"net_total_pnl": "-0.36", "return_percent": "-0.0018"}
              },
              "starting_capital": "20000"
            },
            "folds": {"included": ["1", "2"], "planned": "2"}
          }
        }
      }
    },
    "collection": {
      "collection_id": "a823b3caab14b2dd1f6f6d5ea34f76e4aeadea07243189e25212ca0a1c01c5d5",
      "evidence": "synthetic",
      "skipped_members": []
    },
    "configuration": {
      "decision_interval_ns": "1000000",
      "fee_per_fill": "0.01",
      "latency_ns": "1000",
      "maximum_order_notional": "1000000",
      "maximum_quote_age_ns": "5000000000",
      "starting_cash": "10000",
      "target_quantity": "1",
      "threshold_ppb": "400000000"
    },
    "definitions": {
      "aggregation": {
        "classification": "pooled over the scored rows of every included fold of a model, so every scored row weighs the same; fold accuracies are never averaged; per-fold scores are reported beside it",
        "complete": "a complete aggregate needs every planned fold's cell completed with every account's final valuation; otherwise it is unavailable, with what is missing",
        "money": "independent reset accounts, one per fold: PnL, fees paid, and turnover are summed, as is the starting capital each fold's account was given; nothing is compounded",
        "not_computed": "no spliced equity curve, portfolio drawdown, annualized ratio, confidence interval, or winner",
        "partial": "a partial aggregate covers only the included folds, which it names beside the planned count; it is never the complete result",
        "return_percent": "total net PnL as a percentage of total starting capital, rounded once, half away from zero, to 8 decimal places, as the engine rounds a run's return; never a sum or average of returns"
      },
      "execution": [
        "each fold model's schedule predicts every feature row of its evaluation window from its features and frozen parameters alone; labels are read only afterwards, to score it",
        "each schedule runs once per scenario through chronos_strategy with only the fee per fill and the latency changed, and each run is replayed by chronos_strategy inspect",
        "every run is its own strategy, cash, and buy-and-hold accounts with the configured starting cash; nothing carries from one fold, model, or scenario to another",
        "no stress result is derived from another run: latency changes fills, quantities, risk outcomes, and later decisions, so every cell is executed"
      ],
      "scoring": {
        "cutoff": "the evaluation window's end, exclusive",
        "rule": "a scheduled row is scored when its label is usable and label_available_at_ns < end_at_ns",
        "unscored": "every other scheduled row is counted by reason: feature_unavailable:<reason>, label_unavailable:<reason>, or label_after_scoring_cutoff"
      }
    },
    "environment": {
      "blas": "accelerate unknown",
      "byteorder": "little",
      "machine": "arm64",
      "numpy": "2.5.3",
      "platform": "macOS-15.5-arm64-arm-64bit",
      "python": "CPython 3.12.13",
      "python_build": "main Apr 14 2026 14:33:40",
      "scikit_learn": "1.9.1",
      "scipy": "1.18.1",
      "threadpoolctl": "3.7.0",
      "training_threads": "1"
    },
    "folds": [
      {
        "evaluation": {
          "dataset_id": "2d44accc13eda244d4fac00c78a912c07b58ea7dfeb29b281a613f61704dd7e5",
          "feature_rows": "420",
          "stream": "synthetic-2026-09-21",
          "window": {
            "end_at": "2026-09-21T13:30:00.175864944Z",
            "name": "fold_1",
            "start_at": "2026-09-21T13:30:00.000096830Z"
          }
        },
        "fold_model": {
          "fold_model_id": "d27ed8931f866e66d47c1ea8505d1efbeffed9fb754810024d2ec472074431ce",
          "state": "completed"
        },
        "index": "1",
        "information_boundary": "2026-09-21T13:30:00.000096830Z",
        "models": {
          "logistic": {
            "cells": [
              {
                "fee_per_fill": "0.01",
                "latency_ns": "1000",
                "reason": "model_unevaluable",
                "scenario": "base",
                "state": "unavailable"
              },
              {
                "fee_per_fill": "0.05",
                "latency_ns": "200000",
                "reason": "model_unevaluable",
                "scenario": "stress",
                "state": "unavailable"
              }
            ],
            "detail": {"down": "4", "flat": "2", "up": "13"},
            "reason": "insufficient_class_support",
            "state": "unevaluable"
          },
          "majority": {
            "cells": [
              {
                "accounts": {
                  "buy_and_hold": {
                    "drawdown": {"maximum": "0.07", "maximum_percent_of_peak": "0.00069998", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "100.02", "percent_of_starting_cash": "1.0002"},
                    "valuation": {
                      "equity": "10000.49",
                      "fees_paid": "0.01",
                      "net_total_pnl": "0.49",
                      "return_percent": "0.0049"
                    }
                  },
                  "cash": {
                    "drawdown": {"maximum": "0", "maximum_percent_of_peak": null, "points_unvalued": "0"},
                    "fills": "0",
                    "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                    "turnover": {"notional": "0", "percent_of_starting_cash": "0"},
                    "valuation": {"equity": "10000", "fees_paid": "0", "net_total_pnl": "0", "return_percent": "0"}
                  },
                  "strategy": {
                    "drawdown": {"maximum": "0.07", "maximum_percent_of_peak": "0.00069998", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "100.02", "percent_of_starting_cash": "1.0002"},
                    "valuation": {
                      "equity": "10000.49",
                      "fees_paid": "0.01",
                      "net_total_pnl": "0.49",
                      "return_percent": "0.0049"
                    }
                  }
                },
                "deltas": {
                  "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                  "cash": {"net_total_pnl": "0.49", "return_percent": "0.0049"}
                },
                "fee_per_fill": "0.01",
                "latency_ns": "1000",
                "run": {
                  "artifact_id": "6c1e5ec1773f0790557cee842c16930b841d32352353e7faa89da5cb31f271c7",
                  "replay": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "scenario": "base",
                "starting_cash": "10000",
                "state": "completed"
              },
              {
                "accounts": {
                  "buy_and_hold": {
                    "drawdown": {"maximum": "0.07", "maximum_percent_of_peak": "0.0007", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "100.03", "percent_of_starting_cash": "1.0003"},
                    "valuation": {
                      "equity": "10000.44",
                      "fees_paid": "0.05",
                      "net_total_pnl": "0.44",
                      "return_percent": "0.0044"
                    }
                  },
                  "cash": {
                    "drawdown": {"maximum": "0", "maximum_percent_of_peak": null, "points_unvalued": "0"},
                    "fills": "0",
                    "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                    "turnover": {"notional": "0", "percent_of_starting_cash": "0"},
                    "valuation": {"equity": "10000", "fees_paid": "0", "net_total_pnl": "0", "return_percent": "0"}
                  },
                  "strategy": {
                    "drawdown": {"maximum": "0.07", "maximum_percent_of_peak": "0.0007", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "100.03", "percent_of_starting_cash": "1.0003"},
                    "valuation": {
                      "equity": "10000.44",
                      "fees_paid": "0.05",
                      "net_total_pnl": "0.44",
                      "return_percent": "0.0044"
                    }
                  }
                },
                "deltas": {
                  "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                  "cash": {"net_total_pnl": "0.44", "return_percent": "0.0044"}
                },
                "fee_per_fill": "0.05",
                "latency_ns": "200000",
                "run": {
                  "artifact_id": "b77e745b96c0cdd057fc807bd46387662969614521195d3dd0d27752bfea4a08",
                  "replay": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "scenario": "stress",
                "starting_cash": "10000",
                "state": "completed"
              }
            ],
            "predictive": {
              "metrics": {
                "accuracy": {"value": "0.5144230769230769"},
                "balanced_accuracy": {"value": "0.3333333333333333"},
                "brier_score": {"value": "0.6546185808651184"},
                "confusion_matrix": {
                  "down": {"down": "0", "flat": "0", "up": "124"},
                  "flat": {"down": "0", "flat": "0", "up": "78"},
                  "up": {"down": "0", "flat": "0", "up": "214"}
                },
                "log_loss": {"value": "1.0817823845527121"},
                "per_class": {
                  "down": {
                    "f1": {"value": "0.0"},
                    "precision": {"undefined": "class_never_predicted"},
                    "recall": {"value": "0.0"}
                  },
                  "flat": {
                    "f1": {"value": "0.0"},
                    "precision": {"undefined": "class_never_predicted"},
                    "recall": {"value": "0.0"}
                  },
                  "up": {
                    "f1": {"value": "0.6793650793650794"},
                    "precision": {"value": "0.5144230769230769"},
                    "recall": {"value": "1.0"}
                  }
                },
                "predicted_distribution": {"down": "0", "flat": "0", "up": "416"},
                "rows": "416",
                "true_distribution": {"down": "124", "flat": "78", "up": "214"}
              },
              "rows": {
                "not_scored": {"label_unavailable:no_future_observation": "4"},
                "scheduled": "420",
                "scored": "416"
              }
            },
            "schedule": {
              "artifact_id": "747c3b7f51042808db8477826b1688111d2ef0cbcd26d6adb22351a5759659cd",
              "counts": {
                "classes": {"down": "0", "flat": "0", "up": "420"},
                "predicted": "420",
                "rows": "420",
                "unavailable": {}
              }
            },
            "state": "completed"
          }
        },
        "training": {
          "class_support": {"down": "4", "flat": "2", "up": "13"},
          "max_label_available_at": "2026-09-19T13:30:00.006154117Z",
          "max_prediction_at": "2026-09-19T13:30:00.004833231Z",
          "rows": "19"
        }
      },
      {
        "evaluation": {
          "dataset_id": "1ed1c1d58ead1e6a8b97a4c4f2e6ff09b24562bca7f060f95e2fc86d4aa8bf30",
          "feature_rows": "360",
          "stream": "synthetic-2026-09-22",
          "window": {
            "end_at": "2026-09-22T13:30:00.139291084Z",
            "name": "fold_2",
            "start_at": "2026-09-22T13:30:00.000264444Z"
          }
        },
        "fold_model": {
          "fold_model_id": "fb57169c14a520c4cd0e9609d7703fb3a9dbd02a529331a95995dbc135e6f270",
          "state": "completed"
        },
        "index": "2",
        "information_boundary": "2026-09-22T13:30:00.000264444Z",
        "models": {
          "logistic": {
            "cells": [
              {
                "accounts": {
                  "buy_and_hold": {
                    "drawdown": {"maximum": "0.79", "maximum_percent_of_peak": "0.0079", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.99", "percent_of_starting_cash": "0.9999"},
                    "valuation": {
                      "equity": "9999.23",
                      "fees_paid": "0.01",
                      "net_total_pnl": "-0.77",
                      "return_percent": "-0.0077"
                    }
                  },
                  "cash": {
                    "drawdown": {"maximum": "0", "maximum_percent_of_peak": null, "points_unvalued": "0"},
                    "fills": "0",
                    "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                    "turnover": {"notional": "0", "percent_of_starting_cash": "0"},
                    "valuation": {"equity": "10000", "fees_paid": "0", "net_total_pnl": "0", "return_percent": "0"}
                  },
                  "strategy": {
                    "drawdown": {"maximum": "0.79", "maximum_percent_of_peak": "0.0079", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.99", "percent_of_starting_cash": "0.9999"},
                    "valuation": {
                      "equity": "9999.23",
                      "fees_paid": "0.01",
                      "net_total_pnl": "-0.77",
                      "return_percent": "-0.0077"
                    }
                  }
                },
                "deltas": {
                  "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                  "cash": {"net_total_pnl": "-0.77", "return_percent": "-0.0077"}
                },
                "fee_per_fill": "0.01",
                "latency_ns": "1000",
                "run": {
                  "artifact_id": "6e18c56f6f6a733a9938d1ca341e82a5a9340f8cf82ec0b910bfe32e40938ec0",
                  "replay": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "scenario": "base",
                "starting_cash": "10000",
                "state": "completed"
              },
              {
                "accounts": {
                  "buy_and_hold": {
                    "drawdown": {"maximum": "0.82", "maximum_percent_of_peak": "0.0082", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.98", "percent_of_starting_cash": "0.9998"},
                    "valuation": {
                      "equity": "9999.2",
                      "fees_paid": "0.05",
                      "net_total_pnl": "-0.8",
                      "return_percent": "-0.008"
                    }
                  },
                  "cash": {
                    "drawdown": {"maximum": "0", "maximum_percent_of_peak": null, "points_unvalued": "0"},
                    "fills": "0",
                    "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                    "turnover": {"notional": "0", "percent_of_starting_cash": "0"},
                    "valuation": {"equity": "10000", "fees_paid": "0", "net_total_pnl": "0", "return_percent": "0"}
                  },
                  "strategy": {
                    "drawdown": {"maximum": "0.82", "maximum_percent_of_peak": "0.0082", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.98", "percent_of_starting_cash": "0.9998"},
                    "valuation": {
                      "equity": "9999.2",
                      "fees_paid": "0.05",
                      "net_total_pnl": "-0.8",
                      "return_percent": "-0.008"
                    }
                  }
                },
                "deltas": {
                  "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                  "cash": {"net_total_pnl": "-0.8", "return_percent": "-0.008"}
                },
                "fee_per_fill": "0.05",
                "latency_ns": "200000",
                "run": {
                  "artifact_id": "095d522f52529f182c9f8b159e0020a90775ff9dace449a9a313184ab57c4a05",
                  "replay": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "scenario": "stress",
                "starting_cash": "10000",
                "state": "completed"
              }
            ],
            "predictive": {
              "metrics": {
                "accuracy": {"value": "0.23163841807909605"},
                "balanced_accuracy": {"value": "0.3333333333333333"},
                "brier_score": {"value": "0.7242112192844746"},
                "confusion_matrix": {
                  "down": {"down": "0", "flat": "0", "up": "222"},
                  "flat": {"down": "0", "flat": "0", "up": "50"},
                  "up": {"down": "0", "flat": "0", "up": "82"}
                },
                "log_loss": {"value": "1.1510684088927292"},
                "per_class": {
                  "down": {
                    "f1": {"value": "0.0"},
                    "precision": {"undefined": "class_never_predicted"},
                    "recall": {"value": "0.0"}
                  },
                  "flat": {
                    "f1": {"value": "0.0"},
                    "precision": {"undefined": "class_never_predicted"},
                    "recall": {"value": "0.0"}
                  },
                  "up": {
                    "f1": {"value": "0.3761467889908257"},
                    "precision": {"value": "0.23163841807909605"},
                    "recall": {"value": "1.0"}
                  }
                },
                "predicted_distribution": {"down": "0", "flat": "0", "up": "354"},
                "rows": "354",
                "true_distribution": {"down": "222", "flat": "50", "up": "82"}
              },
              "rows": {
                "not_scored": {"label_unavailable:no_future_observation": "6"},
                "scheduled": "360",
                "scored": "354"
              }
            },
            "schedule": {
              "artifact_id": "04a3f31b224a5e940229b17b0e62d49d3d817100f121fc21ad8497e29838fa00",
              "counts": {
                "classes": {"down": "0", "flat": "0", "up": "360"},
                "predicted": "360",
                "rows": "360",
                "unavailable": {}
              }
            },
            "state": "completed"
          },
          "majority": {
            "cells": [
              {
                "accounts": {
                  "buy_and_hold": {
                    "drawdown": {"maximum": "0.79", "maximum_percent_of_peak": "0.0079", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.99", "percent_of_starting_cash": "0.9999"},
                    "valuation": {
                      "equity": "9999.23",
                      "fees_paid": "0.01",
                      "net_total_pnl": "-0.77",
                      "return_percent": "-0.0077"
                    }
                  },
                  "cash": {
                    "drawdown": {"maximum": "0", "maximum_percent_of_peak": null, "points_unvalued": "0"},
                    "fills": "0",
                    "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                    "turnover": {"notional": "0", "percent_of_starting_cash": "0"},
                    "valuation": {"equity": "10000", "fees_paid": "0", "net_total_pnl": "0", "return_percent": "0"}
                  },
                  "strategy": {
                    "drawdown": {"maximum": "0.79", "maximum_percent_of_peak": "0.0079", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.99", "percent_of_starting_cash": "0.9999"},
                    "valuation": {
                      "equity": "9999.23",
                      "fees_paid": "0.01",
                      "net_total_pnl": "-0.77",
                      "return_percent": "-0.0077"
                    }
                  }
                },
                "deltas": {
                  "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                  "cash": {"net_total_pnl": "-0.77", "return_percent": "-0.0077"}
                },
                "fee_per_fill": "0.01",
                "latency_ns": "1000",
                "run": {
                  "artifact_id": "d149f027c68642516a0b94abb3ccc2cb3c7f2e72ca23140552c8e317e57d3e7c",
                  "replay": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "scenario": "base",
                "starting_cash": "10000",
                "state": "completed"
              },
              {
                "accounts": {
                  "buy_and_hold": {
                    "drawdown": {"maximum": "0.82", "maximum_percent_of_peak": "0.0082", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.98", "percent_of_starting_cash": "0.9998"},
                    "valuation": {
                      "equity": "9999.2",
                      "fees_paid": "0.05",
                      "net_total_pnl": "-0.8",
                      "return_percent": "-0.008"
                    }
                  },
                  "cash": {
                    "drawdown": {"maximum": "0", "maximum_percent_of_peak": null, "points_unvalued": "0"},
                    "fills": "0",
                    "orders": {"expired": "0", "filled": "0", "rejected": "0", "scheduled": "0"},
                    "turnover": {"notional": "0", "percent_of_starting_cash": "0"},
                    "valuation": {"equity": "10000", "fees_paid": "0", "net_total_pnl": "0", "return_percent": "0"}
                  },
                  "strategy": {
                    "drawdown": {"maximum": "0.82", "maximum_percent_of_peak": "0.0082", "points_unvalued": "0"},
                    "fills": "1",
                    "orders": {"expired": "0", "filled": "1", "rejected": "0", "scheduled": "1"},
                    "turnover": {"notional": "99.98", "percent_of_starting_cash": "0.9998"},
                    "valuation": {
                      "equity": "9999.2",
                      "fees_paid": "0.05",
                      "net_total_pnl": "-0.8",
                      "return_percent": "-0.008"
                    }
                  }
                },
                "deltas": {
                  "buy_and_hold": {"net_total_pnl": "0", "return_percent": "0"},
                  "cash": {"net_total_pnl": "-0.8", "return_percent": "-0.008"}
                },
                "fee_per_fill": "0.05",
                "latency_ns": "200000",
                "run": {
                  "artifact_id": "dfff709f800295af7dbd47442b19baa4865051e8b3ce24f0004a2e29183e4962",
                  "replay": "ran the schedule again over its verified dataset: identical journal and results"
                },
                "scenario": "stress",
                "starting_cash": "10000",
                "state": "completed"
              }
            ],
            "predictive": {
              "metrics": {
                "accuracy": {"value": "0.23163841807909605"},
                "balanced_accuracy": {"value": "0.3333333333333333"},
                "brier_score": {"value": "0.7299526391100184"},
                "confusion_matrix": {
                  "down": {"down": "0", "flat": "0", "up": "222"},
                  "flat": {"down": "0", "flat": "0", "up": "50"},
                  "up": {"down": "0", "flat": "0", "up": "82"}
                },
                "log_loss": {"value": "1.1569902351640398"},
                "per_class": {
                  "down": {
                    "f1": {"value": "0.0"},
                    "precision": {"undefined": "class_never_predicted"},
                    "recall": {"value": "0.0"}
                  },
                  "flat": {
                    "f1": {"value": "0.0"},
                    "precision": {"undefined": "class_never_predicted"},
                    "recall": {"value": "0.0"}
                  },
                  "up": {
                    "f1": {"value": "0.3761467889908257"},
                    "precision": {"value": "0.23163841807909605"},
                    "recall": {"value": "1.0"}
                  }
                },
                "predicted_distribution": {"down": "0", "flat": "0", "up": "354"},
                "rows": "354",
                "true_distribution": {"down": "222", "flat": "50", "up": "82"}
              },
              "rows": {
                "not_scored": {"label_unavailable:no_future_observation": "6"},
                "scheduled": "360",
                "scored": "354"
              }
            },
            "schedule": {
              "artifact_id": "e5b5e404b9c47b7b9147d6647f8e8f1082aab9948afba2cd0f9922a3cc27f3ef",
              "counts": {
                "classes": {"down": "0", "flat": "0", "up": "360"},
                "predicted": "360",
                "rows": "360",
                "unavailable": {}
              }
            },
            "state": "completed"
          }
        },
        "training": {
          "class_support": {"down": "128", "flat": "80", "up": "227"},
          "max_label_available_at": "2026-09-21T13:30:00.175860977Z",
          "max_prediction_at": "2026-09-21T13:30:00.174832306Z",
          "rows": "435"
        }
      }
    ],
    "instrument": "AAPL",
    "missing": [{"fold": "1", "item": "model:logistic", "reason": "insufficient_class_support"}],
    "models": ["majority", "logistic"],
    "outcomes": {"cells": {"completed": "6", "unavailable": "2"}, "models": {"completed": "3", "unevaluable": "1"}},
    "plan_id": "8894b1983dc910061b41d35c23546d4950578af5caf9e32e310bacea9c9f481d",
    "scenarios": [
      {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
      {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"}
    ],
    "schema": {"name": "chronos.walk_forward_study", "version": "1"},
    "scope": "historical evaluation only: every figure describes what the frozen fold models' predictions did in these recordings; nothing is promoted, registered, or deployed",
    "status": "partial",
    "study_id": "b362061df2556445e9622708767e6571f80e7cbce341616bf7943b6e71d2e4c9"
  }
};

const STUDIES: readonly Json[] = [
  {
    "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
    "evidence": "synthetic",
    "folds": "2",
    "instrument": "AAPL",
    "missing": "0",
    "plan_id": "215ce8c0e73108596213990c2d0b051da4451c5f099ded0e71e2b29016de6772",
    "status": "complete",
    "study_id": "2b707a451eb836e7cb9fb4b91f2ee75cb2a05370549a5bc27b00755ddc6e8b27"
  },
  {
    "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
    "evidence": "synthetic",
    "folds": "2",
    "instrument": "AAPL",
    "missing": "0",
    "plan_id": "0be48271f72edb602469f66c7fabb28ab92fde1ae214235be52f9ca65149e33e",
    "status": "complete",
    "study_id": "67067b4f7f7a939f6b58fa3e9fcf6df09e77fb6821fcd4b7329f23cc27bd53be"
  },
  {
    "collection_id": "a823b3caab14b2dd1f6f6d5ea34f76e4aeadea07243189e25212ca0a1c01c5d5",
    "evidence": "synthetic",
    "folds": "2",
    "instrument": "AAPL",
    "missing": "1",
    "plan_id": "8894b1983dc910061b41d35c23546d4950578af5caf9e32e310bacea9c9f481d",
    "status": "partial",
    "study_id": "b362061df2556445e9622708767e6571f80e7cbce341616bf7943b6e71d2e4c9"
  }
];

const FAILED_PLAN_JOB: Json = {
  "created_at": "2026-10-08T17:39:48.730864128Z",
  "error": {
    "code": "plan_refused",
    "kind": "PlanError",
    "message": "1 recording(s) of collection deba665a20df808170e3995ee148f8d810989f03204fd536099ea877ebd2d1b9 hold AAPL; a study training on at least 1 earlier recording(s) needs at least 2 independent recordings of it",
    "reason": "insufficient_coverage"
  },
  "finished_at": "2026-10-08T17:39:50.935151104Z",
  "job_id": "441eaae21aa9c8b1795ae82d09628e35",
  "kind": "walk_forward_plan",
  "log": "{\n  \"error\": {\n    \"kind\": \"PlanError\",\n    \"message\": \"1 recording(s) of collection deba665a20df808170e3995ee148f8d810989f03204fd536099ea877ebd2d1b9 hold AAPL; a study training on at least 1 earlier recording(s) needs at least 2 independent recordings of it\",\n    \"reason\": \"insufficient_coverage\"\n  }\n}\n",
  "request": {
    "collection_id": "deba665a20df808170e3995ee148f8d810989f03204fd536099ea877ebd2d1b9",
    "configuration": {
      "decision_interval_ns": "1000000",
      "fee_per_fill": "0.01",
      "latency_ns": "1000",
      "maximum_order_notional": "1000000",
      "maximum_quote_age_ns": "5000000000",
      "starting_cash": "10000",
      "target_quantity": "1",
      "threshold_ppb": "400000000"
    },
    "instrument": "AAPL",
    "minimum_training_sources": "1",
    "scenarios": [
      {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
      {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"}
    ],
    "schema": {"name": "chronos.walk_forward_request", "version": "1"}
  },
  "result": null,
  "started_at": "2026-10-08T17:39:48.732423168Z",
  "state": "failed"
};

const STUDY_JOB: Json = {
  "created_at": "2026-10-08T17:39:52.931393024Z",
  "error": null,
  "finished_at": "2026-10-08T17:39:59.170674944Z",
  "job_id": "740e1d1f1c3f460f75f51fc6c0e7f61e",
  "kind": "walk_forward",
  "log": "",
  "progress": {"stage": "publish"},
  "request": {"plan_id": "0be48271f72edb602469f66c7fabb28ab92fde1ae214235be52f9ca65149e33e"},
  "result": {
    "missing": "0",
    "outcome": "already_present",
    "status": "complete",
    "study_id": "67067b4f7f7a939f6b58fa3e9fcf6df09e77fb6821fcd4b7329f23cc27bd53be"
  },
  "started_at": "2026-10-08T17:39:52.932367104Z",
  "state": "succeeded"
};

const FOLD_RESULT: Json = {
  "result": {
    "accounts": {
      "buy_and_hold": {
        "drawdown": {
          "maximum": "0.21",
          "maximum_percent_of_peak": "0.00209999",
          "peak_at": "2026-09-23T13:30:00.060748708Z",
          "points_unvalued": "0",
          "points_valued": "137",
          "trough_at": "2026-09-23T13:30:00.170900277Z"
        },
        "fills": "1",
        "final": {
          "ledger": {
            "cash": "9899.98",
            "fees_paid": "0.01",
            "gross_realized_pnl": "0",
            "open_cost_basis": "100.01",
            "position": "1"
          },
          "valuation": {
            "cash": "9899.98",
            "equity": "9999.91",
            "fees_paid": "0.01",
            "gross_realized_pnl": "0",
            "gross_unrealized_pnl": "-0.08",
            "mark": {"bid": "99.93", "ingest_sequence": "998", "receive_timestamp": "2026-09-23T13:30:00.210734812Z"},
            "market_value": "99.93",
            "net_total_pnl": "-0.09",
            "position": "1",
            "return_percent": "-0.0009"
          }
        },
        "orders": {
          "cancelled": "0",
          "expired": "0",
          "filled": "1",
          "partially_filled_and_cancelled": "0",
          "rejected": "0",
          "rejections": {},
          "scheduled": "1"
        },
        "traded_quantity": "1",
        "turnover": {"notional": "100.01", "percent_of_starting_cash": "1.0001"}
      },
      "cash": {
        "drawdown": {
          "maximum": "0",
          "maximum_percent_of_peak": null,
          "peak_at": null,
          "points_unvalued": "0",
          "points_valued": "137",
          "trough_at": null
        },
        "fills": "0",
        "final": {
          "ledger": {
            "cash": "10000",
            "fees_paid": "0",
            "gross_realized_pnl": "0",
            "open_cost_basis": "0",
            "position": "0"
          },
          "valuation": {
            "cash": "10000",
            "equity": "10000",
            "fees_paid": "0",
            "gross_realized_pnl": "0",
            "gross_unrealized_pnl": "0",
            "mark": null,
            "market_value": "0",
            "net_total_pnl": "0",
            "position": "0",
            "return_percent": "0"
          }
        },
        "orders": {
          "cancelled": "0",
          "expired": "0",
          "filled": "0",
          "partially_filled_and_cancelled": "0",
          "rejected": "0",
          "rejections": {},
          "scheduled": "0"
        },
        "traded_quantity": "0",
        "turnover": {"notional": "0", "percent_of_starting_cash": "0"}
      },
      "strategy": {
        "drawdown": {
          "maximum": "0.83",
          "maximum_percent_of_peak": "0.0083",
          "peak_at": "2026-09-23T13:30:00.000363583Z",
          "points_unvalued": "0",
          "points_valued": "137",
          "trough_at": "2026-09-23T13:30:00.209996206Z"
        },
        "fills": "37",
        "final": {
          "ledger": {
            "cash": "9899.26",
            "fees_paid": "0.37",
            "gross_realized_pnl": "-0.44",
            "open_cost_basis": "99.93",
            "position": "1"
          },
          "valuation": {
            "cash": "9899.26",
            "equity": "9999.19",
            "fees_paid": "0.37",
            "gross_realized_pnl": "-0.44",
            "gross_unrealized_pnl": "0",
            "mark": {"bid": "99.93", "ingest_sequence": "998", "receive_timestamp": "2026-09-23T13:30:00.210734812Z"},
            "market_value": "99.93",
            "net_total_pnl": "-0.81",
            "position": "1",
            "return_percent": "-0.0081"
          }
        },
        "orders": {
          "cancelled": "0",
          "expired": "0",
          "filled": "37",
          "partially_filled_and_cancelled": "0",
          "rejected": "0",
          "rejections": {},
          "scheduled": "37"
        },
        "traded_quantity": "37",
        "turnover": {"notional": "3699.19", "percent_of_starting_cash": "36.9919"}
      }
    },
    "assumptions": [
      "Executions use recorded top-of-book quotes only: a market order fills at the opposite displayed best price, up to its displayed quantity, with no depth, queue position, hidden liquidity, or market impact.",
      "An order arrives latency_ns after its signal and executes against the first top-of-book event of the instrument received at or after its arrival; whatever that observation is, its outcome is final.",
      "An order whose execution observation arrives while the instrument is halted or closed is rejected.",
      "An account takes each observation's displayed liquidity once; the strategy and the benchmarks are separate counterfactual accounts and do not share it.",
      "fee_per_fill is charged on every fill; it is an assumption, not a broker's fee schedule, and there are no borrow, financing, or exchange fees.",
      "Equity marks an open position at the best bid of the instrument's latest quote, which must be valid, unsuspect, and no older than maximum_quote_age_ns; a position that cannot be marked makes the valuation unavailable, never zero.",
      "Buy and hold buys target_quantity once, at the strategy's first decision, and holds; cash never trades.",
      "Orders still pending when the window ends expire unexecuted; open positions are not liquidated."
    ],
    "configuration": {
      "execution": {
        "fee_per_fill": "0.01",
        "latency_ns": "1000",
        "maximum_order_notional": "1000000",
        "maximum_quote_age_ns": "5000000000",
        "order_type": "market",
        "time_in_force": "immediate_or_cancel"
      },
      "policy": {
        "decision_interval_ns": "1000000",
        "name": "long_flat_threshold",
        "target_quantity": "1",
        "threshold_ppb": "400000000",
        "version": "1"
      },
      "starting_cash": "10000"
    },
    "decisions": {
      "already_at_target": "62",
      "evaluated": "99",
      "hold_below_threshold": "0",
      "hold_flat_prediction": "0",
      "order_pending": "0",
      "orders": "37",
      "skipped_decision_interval": "401",
      "skipped_prediction_unavailable": "0"
    },
    "definitions": {
      "decision": "a schedule row with a prediction, at least decision_interval_ns after the previous decision: up with confidence_ppb >= threshold_ppb targets target_quantity, down with confidence_ppb >= threshold_ppb targets flat, and anything else holds; an order is the target minus the position, and none is made while one is pending",
      "drawdown": "the largest fall of equity from its running peak over the valuation points, in money and as a percentage of that peak; points whose valuation is unavailable are counted and skipped",
      "return_percent": "net total PnL as a percentage of starting cash",
      "turnover": "the sum of every fill's notional, also as a percentage of starting cash",
      "valuation_points": "after each decision and each execution, and at the window's last applied event"
    },
    "environment": {
      "architecture": "arm64",
      "arithmetic": "fixed-point integers with checked 128-bit intermediates; no floating point",
      "compiler": "clang 17.0.0 (clang-1700.0.13.5)",
      "platform": "darwin"
    },
    "events": {"after_window_unused": "0", "read": "1000", "schedule_rows": "500", "warmup": "0", "window": "1000"},
    "journal_entries": "313",
    "limitations": [
      "Each dataset is one short recorded session: its validation and test windows cover minutes, not market regimes.",
      "Prediction metrics measure classification of one-second midpoint moves, not trading profit.",
      "Strategy runs are paper simulations against recorded top-of-book quotes: one share at a time, the displayed best price, no depth, queue position, or market impact, and an assumed fee per fill.",
      "No result here is evidence that a model or policy would be profitable."
    ],
    "run_id": "d5959e9a651c30f8a6aff57c94f0289d120d42a0dc4591a7944029a717159dd6",
    "runner": {"name": "chronos.strategy.event_loop", "version": "1"},
    "scope": "a deterministic paper simulation of one policy on a few minutes of one recorded session; it verifies the evaluation machinery and is not evidence that any model or policy would be profitable",
    "source": {
      "collection_id": "92bb761b166ab88d977d77d60d97a45209400092b9b48feb6c4a22b034295879",
      "dataset_id": "d3539363f099593ffd67063d80e2b1e5208642939f14934dbb9cc67e4504da9a",
      "events_sha256": "42b87ee28997a472bdab0152a01741e54d219cc25aaa2fa1939b9d1c3d1de4d9",
      "feature_artifact_id": "f06364b8275705edabbe42a96af1b6787913b57848635a7d2cebf8cfc35c40a5",
      "fold": "2",
      "fold_model_id": "b7987fabe64bc5427afcc8b6b9e3bc9f41e2ce179c2c122666894ea96cd5e8d2",
      "information_boundary": "2026-09-23T13:30:00.000363583Z",
      "instrument": "AAPL",
      "model": "logistic",
      "parameters_sha256": "2e77cf897ee49d2c4f71dce1e20af1c23927472b49a3bb4578f1b3dbd7d5abce",
      "plan_id": "0be48271f72edb602469f66c7fabb28ab92fde1ae214235be52f9ca65149e33e",
      "schedule_id": "d52959a848afd448a75d8f6c7c4c7eb45a92ec199a027c75dff3e7553301fd56",
      "schedule_sha256": "5333428e0b9732d507c4f23c1d0ae8d6ed6bf1e5adbe58abc9ff9ad3bfeb813b",
      "stream": "synthetic-2026-09-23",
      "training_labels_available_through": "2026-09-22T13:30:00.139211324Z",
      "training_predictions_through": "2026-09-22T13:30:00.138122322Z"
    },
    "window": {
      "end_at": "2026-09-23T13:30:00.210759199Z",
      "first_event_at": "2026-09-23T13:30:00.000363583Z",
      "name": "fold_2",
      "start_at": "2026-09-23T13:30:00.000363583Z",
      "valued_at": "2026-09-23T13:30:00.210759198Z"
    }
  }
};

function copy<T>(value: T): T {
  return structuredClone(value);
}

export function collectionPageJson(collections: readonly Json[] = COLLECTIONS): Json {
  return { collections: copy(collections), invalid_entries: [], next_cursor: null };
}

export function collectionJson(collectionId: string): Json {
  const found = COLLECTIONS.find((entry) => entry.collection_id === collectionId);
  if (found === undefined) {
    throw new Error(`no fixture collection ${collectionId}`);
  }
  return copy(found);
}

export function planJson(): Json {
  return { plan: copy(PLAN) };
}

export function partialStudyDetailJson(): Json {
  return copy(PARTIAL_STUDY_DETAIL);
}

export function studyPageJson(studies: readonly Json[] = STUDIES): Json {
  return { studies: copy(studies), invalid_entries: [], next_cursor: null };
}

// A Preview job: by default the week's, succeeded with its plan.
export function planJobJson(changes: Json = {}): Json {
  return {
    ...copy(FAILED_PLAN_JOB),
    error: null,
    request: { ...(copy(FAILED_PLAN_JOB.request) as Json), collection_id: WEEK_COLLECTION_ID },
    result: { folds: '2', outcome: 'published', plan_id: PLAN_ID },
    state: 'succeeded',
    ...changes,
  };
}

// The real refusal of a collection with one recording.
export function failedPlanJobJson(): Json {
  return copy(FAILED_PLAN_JOB);
}

export function studyJobJson(changes: Json = {}): Json {
  return { ...copy(STUDY_JOB), ...changes };
}

export function studyJobPageJson(jobs: readonly Json[] = [studyJobJson()]): Json {
  return { jobs: copy(jobs), next_cursor: null };
}

export function foldResultJson(): Json {
  return copy(FOLD_RESULT);
}
