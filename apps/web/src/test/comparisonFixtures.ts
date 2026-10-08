// A comparison as the research service describes it: the real response for
// the synthetic market's AAPL logistic version, its ONNX export imported
// through the sandbox, and a SPY version refused for its instrument (hence
// the 1970 times). The job is that comparison's, with its ID replaced.
type Json = Record<string, unknown>;

export const COMPARISON_ID = '8951fed9e5bb76d3f19443d2e7a5e087e12d719c9d018b38cefddbc287b0f61e';
export const COMPARISON_JOB_ID = '3'.repeat(32);

export function comparisonDetailJson(): Json {
  return structuredClone(DETAIL);
}

export function comparisonPageJson(): Json {
  return structuredClone(LISTING);
}

export function comparisonJobJson(overrides: Json = {}): Json {
  return { ...structuredClone(JOB), job_id: COMPARISON_JOB_ID, ...overrides };
}

const DETAIL: Json = {
  "artifacts": [
    {
      "model_version_id": "45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
      "run": "verified",
      "schedule": "verified"
    },
    {
      "model_version_id": "f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360",
      "run": "verified",
      "schedule": "verified"
    },
    {
      "model_version_id": "9cc6ae3d1d5b1796041f9f439615d60118e75a5caf11a211c613eec090f4ebac"
    }
  ],
  "comparison": {
    "benchmarks": {
      "accounts": {
        "buy_and_hold": {
          "drawdown": {
            "maximum": "0.04",
            "maximum_percent_of_peak": "0.0004",
            "points_unvalued": "0"
          },
          "fills": "1",
          "orders": {
            "expired": "0",
            "filled": "1",
            "rejected": "0",
            "scheduled": "1"
          },
          "turnover": {
            "notional": "99.96",
            "percent_of_starting_cash": "0.9996"
          },
          "valuation": {
            "equity": "9999.96",
            "fees_paid": "0.01",
            "net_total_pnl": "-0.04",
            "return_percent": "-0.0004"
          }
        },
        "cash": {
          "drawdown": {
            "maximum": "0",
            "maximum_percent_of_peak": null,
            "points_unvalued": "0"
          },
          "fills": "0",
          "orders": {
            "expired": "0",
            "filled": "0",
            "rejected": "0",
            "scheduled": "0"
          },
          "turnover": {
            "notional": "0",
            "percent_of_starting_cash": "0"
          },
          "valuation": {
            "equity": "10000",
            "fees_paid": "0",
            "net_total_pnl": "0",
            "return_percent": "0"
          }
        }
      },
      "identical_across_members": true,
      "members_differing": []
    },
    "comparison_id": "8951fed9e5bb76d3f19443d2e7a5e087e12d719c9d018b38cefddbc287b0f61e",
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
    "fairness": [
      "every member is predicted on the same feature rows of the same verified split, instrument, and window, the unlabelled tail included, and run with the same configuration over the same recorded events",
      "each member trades in its own strategy account beside its own cash and buy-and-hold accounts; no account shares liquidity, orders, or fills with another member's",
      "no member was retrained: an experiment model's verified parameters and an import's exact bytes are used as registered",
      "every member's information cutoff is at or before the window's start; a cutoff its uploader declared makes that member's result conditional on the declaration"
    ],
    "members": [
      {
        "accounts": {
          "buy_and_hold": {
            "drawdown": {
              "maximum": "0.04",
              "maximum_percent_of_peak": "0.0004",
              "points_unvalued": "0"
            },
            "fills": "1",
            "orders": {
              "expired": "0",
              "filled": "1",
              "rejected": "0",
              "scheduled": "1"
            },
            "turnover": {
              "notional": "99.96",
              "percent_of_starting_cash": "0.9996"
            },
            "valuation": {
              "equity": "9999.96",
              "fees_paid": "0.01",
              "net_total_pnl": "-0.04",
              "return_percent": "-0.0004"
            }
          },
          "cash": {
            "drawdown": {
              "maximum": "0",
              "maximum_percent_of_peak": null,
              "points_unvalued": "0"
            },
            "fills": "0",
            "orders": {
              "expired": "0",
              "filled": "0",
              "rejected": "0",
              "scheduled": "0"
            },
            "turnover": {
              "notional": "0",
              "percent_of_starting_cash": "0"
            },
            "valuation": {
              "equity": "10000",
              "fees_paid": "0",
              "net_total_pnl": "0",
              "return_percent": "0"
            }
          },
          "strategy": {
            "drawdown": {
              "maximum": "0.04",
              "maximum_percent_of_peak": "0.0004",
              "points_unvalued": "0"
            },
            "fills": "1",
            "orders": {
              "expired": "0",
              "filled": "1",
              "rejected": "0",
              "scheduled": "1"
            },
            "turnover": {
              "notional": "99.96",
              "percent_of_starting_cash": "0.9996"
            },
            "valuation": {
              "equity": "9999.96",
              "fees_paid": "0.01",
              "net_total_pnl": "-0.04",
              "return_percent": "-0.0004"
            }
          }
        },
        "conditional": false,
        "deltas": {
          "buy_and_hold": {
            "net_total_pnl": {
              "value": "0"
            },
            "return_percent": {
              "value": "0"
            }
          },
          "cash": {
            "net_total_pnl": {
              "value": "-0.04"
            },
            "return_percent": {
              "value": "-0.0004"
            }
          }
        },
        "family": "logistic",
        "information_cutoff": {
          "at": "1970-01-01T00:00:01.048000000Z",
          "basis": "verified"
        },
        "model_version_id": "45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
        "names": [
          {
            "name": "aapl-logistic",
            "version": "1"
          }
        ],
        "origin": "experiment",
        "predictive": {
          "metrics": {
            "accuracy": {
              "value": "0.3291139240506329"
            },
            "balanced_accuracy": {
              "value": "0.3076923076923077"
            },
            "brier_score": {
              "value": "0.7513276604267667"
            },
            "confusion_matrix": {
              "down": {
                "down": "15",
                "flat": "0",
                "up": "15"
              },
              "flat": {
                "down": "13",
                "flat": "0",
                "up": "10"
              },
              "up": {
                "down": "15",
                "flat": "0",
                "up": "11"
              }
            },
            "log_loss": {
              "value": "1.2250644527911807"
            },
            "per_class": {
              "down": {
                "f1": {
                  "value": "0.410958904109589"
                },
                "precision": {
                  "value": "0.3488372093023256"
                },
                "recall": {
                  "value": "0.5"
                }
              },
              "flat": {
                "f1": {
                  "value": "0.0"
                },
                "precision": {
                  "undefined": "class_never_predicted"
                },
                "recall": {
                  "value": "0.0"
                }
              },
              "up": {
                "f1": {
                  "value": "0.3548387096774194"
                },
                "precision": {
                  "value": "0.3055555555555556"
                },
                "recall": {
                  "value": "0.4230769230769231"
                }
              }
            },
            "predicted_distribution": {
              "down": "43",
              "flat": "0",
              "up": "36"
            },
            "rows": "79",
            "true_distribution": {
              "down": "30",
              "flat": "23",
              "up": "26"
            }
          },
          "rows": {
            "not_scored": {
              "label_unavailable:no_future_observation": "1"
            },
            "scheduled": "80",
            "scored": "79"
          }
        },
        "run": {
          "artifact_id": "d10d705487ae6e66deb26ee41124dcea1477ce985a7f41ca2059bd62c1f5b8c2",
          "replay": "ran the schedule again over its verified dataset: identical journal and results",
          "replayed": true
        },
        "schedule": {
          "artifact_id": "110a6f428053643f536ffbefd816888347914b2f7f6e9dcfad80d4fee6ee5f21",
          "counts": {
            "classes": {
              "down": "44",
              "flat": "0",
              "up": "36"
            },
            "predicted": "80",
            "rows": "80",
            "unavailable": {}
          },
          "reproduced": true
        },
        "state": "completed"
      },
      {
        "accounts": {
          "buy_and_hold": {
            "drawdown": {
              "maximum": "0.04",
              "maximum_percent_of_peak": "0.0004",
              "points_unvalued": "0"
            },
            "fills": "1",
            "orders": {
              "expired": "0",
              "filled": "1",
              "rejected": "0",
              "scheduled": "1"
            },
            "turnover": {
              "notional": "99.96",
              "percent_of_starting_cash": "0.9996"
            },
            "valuation": {
              "equity": "9999.96",
              "fees_paid": "0.01",
              "net_total_pnl": "-0.04",
              "return_percent": "-0.0004"
            }
          },
          "cash": {
            "drawdown": {
              "maximum": "0",
              "maximum_percent_of_peak": null,
              "points_unvalued": "0"
            },
            "fills": "0",
            "orders": {
              "expired": "0",
              "filled": "0",
              "rejected": "0",
              "scheduled": "0"
            },
            "turnover": {
              "notional": "0",
              "percent_of_starting_cash": "0"
            },
            "valuation": {
              "equity": "10000",
              "fees_paid": "0",
              "net_total_pnl": "0",
              "return_percent": "0"
            }
          },
          "strategy": {
            "drawdown": {
              "maximum": "0.04",
              "maximum_percent_of_peak": "0.0004",
              "points_unvalued": "0"
            },
            "fills": "1",
            "orders": {
              "expired": "0",
              "filled": "1",
              "rejected": "0",
              "scheduled": "1"
            },
            "turnover": {
              "notional": "99.96",
              "percent_of_starting_cash": "0.9996"
            },
            "valuation": {
              "equity": "9999.96",
              "fees_paid": "0.01",
              "net_total_pnl": "-0.04",
              "return_percent": "-0.0004"
            }
          }
        },
        "conditional": true,
        "deltas": {
          "buy_and_hold": {
            "net_total_pnl": {
              "value": "0"
            },
            "return_percent": {
              "value": "0"
            }
          },
          "cash": {
            "net_total_pnl": {
              "value": "-0.04"
            },
            "return_percent": {
              "value": "-0.0004"
            }
          },
          "reference": {
            "net_total_pnl": {
              "value": "0"
            },
            "return_percent": {
              "value": "0"
            }
          }
        },
        "family": "onnx",
        "information_cutoff": {
          "at": "1970-01-01T00:00:01.048000000Z",
          "basis": "declared"
        },
        "model_version_id": "f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360",
        "names": [
          {
            "name": "aapl-logistic-onnx",
            "version": "1"
          }
        ],
        "origin": "import",
        "predictive": {
          "metrics": {
            "accuracy": {
              "value": "0.3291139240506329"
            },
            "balanced_accuracy": {
              "value": "0.3076923076923077"
            },
            "brier_score": {
              "value": "0.7513276604267667"
            },
            "confusion_matrix": {
              "down": {
                "down": "15",
                "flat": "0",
                "up": "15"
              },
              "flat": {
                "down": "13",
                "flat": "0",
                "up": "10"
              },
              "up": {
                "down": "15",
                "flat": "0",
                "up": "11"
              }
            },
            "log_loss": {
              "value": "1.2250644527911807"
            },
            "per_class": {
              "down": {
                "f1": {
                  "value": "0.410958904109589"
                },
                "precision": {
                  "value": "0.3488372093023256"
                },
                "recall": {
                  "value": "0.5"
                }
              },
              "flat": {
                "f1": {
                  "value": "0.0"
                },
                "precision": {
                  "undefined": "class_never_predicted"
                },
                "recall": {
                  "value": "0.0"
                }
              },
              "up": {
                "f1": {
                  "value": "0.3548387096774194"
                },
                "precision": {
                  "value": "0.3055555555555556"
                },
                "recall": {
                  "value": "0.4230769230769231"
                }
              }
            },
            "predicted_distribution": {
              "down": "43",
              "flat": "0",
              "up": "36"
            },
            "rows": "79",
            "true_distribution": {
              "down": "30",
              "flat": "23",
              "up": "26"
            }
          },
          "rows": {
            "not_scored": {
              "label_unavailable:no_future_observation": "1"
            },
            "scheduled": "80",
            "scored": "79"
          }
        },
        "run": {
          "artifact_id": "08b6ed7d558966dc00e771f75014b8638f90c3d6cf1f4cdce49778a3a2df5675",
          "replay": "ran the schedule again over its verified dataset: identical journal and results",
          "replayed": true
        },
        "schedule": {
          "artifact_id": "c2b2d7b253e847a61fbd0db67f38c894dba8b7897145538f6c64caa4e5f796f9",
          "counts": {
            "classes": {
              "down": "44",
              "flat": "0",
              "up": "36"
            },
            "predicted": "80",
            "rows": "80",
            "unavailable": {}
          },
          "reproduced": true
        },
        "state": "completed"
      },
      {
        "conditional": false,
        "detail": "the version predicts SPY, not AAPL",
        "family": "logistic",
        "information_cutoff": {
          "at": "1970-01-01T00:00:01.048000000Z",
          "basis": "verified"
        },
        "model_version_id": "9cc6ae3d1d5b1796041f9f439615d60118e75a5caf11a211c613eec090f4ebac",
        "names": [
          {
            "name": "spy-logistic",
            "version": "1"
          }
        ],
        "origin": "experiment",
        "reason": "instrument_mismatch",
        "state": "refused"
      }
    ],
    "reference_model_version_id": "45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
    "schema": {
      "name": "chronos.model_comparison",
      "version": "1"
    },
    "scoring": [
      "prediction quality is scored only after every member's predictions exist, from each completed member's stored predictions, against the split's realized labels for the window; the model is not run again",
      "rows are joined to labels by stream, instrument, and ingest sequence; a row is scored when the split assigns it to the window with a usable label, and every other row is counted under the split's reason",
      "the metrics are the experiment's definitions, in binary floating point; a metric without a value is undefined with its reason; classification quality is not trading performance"
    ],
    "sources": {
      "dataset_id": "e9dbb1e2a9fa4e5fa31801c66c4d7c4e7a6517fad19c4b4a547a065e90fe53e3",
      "feature_artifact_id": "ed8232d02a85720097e87947282897279584f1385d68aed48aadfb41a9354322",
      "feature_set": "top_of_book_v1",
      "features_sha256": "73f47d008250aa22ed387ae4536cd92b737b797d005d4a10ce8af59c0ca0b870",
      "horizon_ns": "150000",
      "label_artifact_id": "9828e67a6ecc854ad9ba97e6b146d5a511f6f5d8bf74487c4a51647398e806df",
      "label_set": "future_midpoint_return_v1",
      "labels_sha256": "1364e2238172866e540e246f2b465218d208078f80c7870ee2c58261ebfcd14d",
      "split_artifact_id": "6b00dbe49a1cf736bf4042b3f61995496e320a73f9a01261c767f512c1fadcfe",
      "split_set": "chronological_purged_v1",
      "splits_sha256": "a5fb81760298c733606293e7df6451d26426aee26b679b62bcf6ad9841d76518",
      "stream": "synthetic-stream",
      "test_start_at": "1970-01-01T00:00:01.064000000Z",
      "validation_start_at": "1970-01-01T00:00:01.048000000Z"
    },
    "spec": {
      "configuration": {
        "decision_interval_ns": "1000000000",
        "fee_per_fill": "0.01",
        "latency_ns": "1000000",
        "maximum_order_notional": "1000000",
        "maximum_quote_age_ns": "5000000000",
        "starting_cash": "10000",
        "target_quantity": "1",
        "threshold_ppb": "500000000"
      },
      "instrument": "AAPL",
      "model_version_ids": [
        "45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
        "f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360",
        "9cc6ae3d1d5b1796041f9f439615d60118e75a5caf11a211c613eec090f4ebac"
      ],
      "schema": {
        "name": "chronos.model_comparison_spec",
        "version": "1"
      },
      "split_artifact_id": "6b00dbe49a1cf736bf4042b3f61995496e320a73f9a01261c767f512c1fadcfe",
      "window": "test"
    },
    "spec_sha256": "468b9f0e10cff6a85a31da69b970421a806f59a3f4ab24036cd7950265db8a04",
    "status": {
      "completed": "2",
      "failed": "0",
      "refused": "1"
    },
    "window": {
      "end_at": null,
      "name": "test",
      "start_at": "1970-01-01T00:00:01.064000000Z"
    }
  }
};

const LISTING: Json = {
  "comparisons": [
    {
      "comparison_id": "8951fed9e5bb76d3f19443d2e7a5e087e12d719c9d018b38cefddbc287b0f61e",
      "instrument": "AAPL",
      "members": [
        {
          "model_version_id": "45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
          "names": [
            {
              "name": "aapl-logistic",
              "version": "1"
            }
          ],
          "state": "completed"
        },
        {
          "model_version_id": "f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360",
          "names": [
            {
              "name": "aapl-logistic-onnx",
              "version": "1"
            }
          ],
          "state": "completed"
        },
        {
          "model_version_id": "9cc6ae3d1d5b1796041f9f439615d60118e75a5caf11a211c613eec090f4ebac",
          "names": [
            {
              "name": "spy-logistic",
              "version": "1"
            }
          ],
          "state": "refused"
        }
      ],
      "status": {
        "completed": "2",
        "failed": "0",
        "refused": "1"
      },
      "window": {
        "end_at": null,
        "name": "test",
        "start_at": "1970-01-01T00:00:01.064000000Z"
      }
    }
  ],
  "invalid_entries": [
    {
      "comparison_id": "0c9f8ac46d8860e679c509164d38b8fe2d5a349aeb0e9beca5b023e78087d617",
      "reason": "every completed member of a comparison has its prediction quality"
    },
    {
      "comparison_id": "20c45a9e9b7a2563ff9b7a9678a3627e13486c6c112db18df3202bb7dc1b52a4",
      "reason": "every completed member of a comparison has its prediction quality"
    }
  ],
  "next_cursor": null
};

const JOB: Json = {
  "created_at": "2026-10-08T11:39:36.541185024Z",
  "error": null,
  "finished_at": "2026-10-08T11:39:40.733798912Z",
  "job_id": "c354b6f0537d6cc24bd9ad29598e57b4",
  "kind": "comparison",
  "log": "",
  "request": {
    "configuration": {
      "decision_interval_ns": "1000000000",
      "fee_per_fill": "0.01",
      "latency_ns": "1000000",
      "maximum_order_notional": "1000000",
      "maximum_quote_age_ns": "5000000000",
      "starting_cash": "10000",
      "target_quantity": "1",
      "threshold_ppb": "500000000"
    },
    "instrument": "AAPL",
    "model_version_ids": [
      "45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
      "f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360",
      "9cc6ae3d1d5b1796041f9f439615d60118e75a5caf11a211c613eec090f4ebac"
    ],
    "schema": {
      "name": "chronos.model_comparison_spec",
      "version": "1"
    },
    "split_artifact_id": "6b00dbe49a1cf736bf4042b3f61995496e320a73f9a01261c767f512c1fadcfe",
    "window": "test"
  },
  "result": {
    "comparison_id": "8951fed9e5bb76d3f19443d2e7a5e087e12d719c9d018b38cefddbc287b0f61e",
    "outcome": "published",
    "status": {
      "completed": "2",
      "failed": "0",
      "refused": "1"
    }
  },
  "started_at": "2026-10-08T11:39:36.542290944Z",
  "state": "succeeded"
};
