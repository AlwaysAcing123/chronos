// A model version as the research service describes it: the real response
// for the recording's AAPL logistic model, with its experiment configuration
// left out (the web client does not read it).
import { EXPERIMENT_ID } from './researchFixtures';

type Json = Record<string, unknown>;

export const MODEL_VERSION_ID = '2b5b6c77a7f579f621bb2352a7c066565e0085833b7b6be9669637570a4a81b2';
export const REGISTRATION_JOB_ID = 'c'.repeat(32);

export function modelDetailJson(overrides: Json = {}): Json {
  return { ...structuredClone(DETAIL), ...overrides };
}

export function modelListingJson(overrides: Json = {}): Json {
  const { family, instrument, lifecycle, model_version_id, names, payload, training } = modelDetailJson();
  return { family, instrument, lifecycle, model_version_id, names, payload, training, origin: 'experiment', ...overrides };
}

export function registrationJobJson(overrides: Json = {}): Json {
  return {
    job_id: REGISTRATION_JOB_ID,
    kind: 'registration',
    state: 'queued',
    created_at: '2026-10-07T20:00:00.000000000Z',
    started_at: null,
    finished_at: null,
    request: { experiment_id: EXPERIMENT_ID, instrument: 'SPY', model: 'logistic', name: 'spy-logistic', version: '1' },
    result: null,
    error: null,
    log: '',
    ...overrides,
  };
}

const DETAIL: Json = {
  "compatibility": {
    "deployment": {
      "requires": "a paper-trading server started with --model-deployments that trades AAPL, and for an imported model --onnx-docker; the deployment decides only on market data received at or after the information cutoff, and only after an explicit Start",
      "status": "compatible"
    },
    "evaluation": {
      "requires": "a verified split of a stream with AAPL feature rows; the information cutoff must be at or before the evaluation window's start",
      "status": "compatible"
    }
  },
  "family": "logistic",
  "format": {
    "name": "chronos.baseline_parameters",
    "payload": "parameters.json",
    "version": "1"
  },
  "inputs": {
    "conversion": "each exact integer divided by its scale and rounded once to binary64 (model_data.to_floats)",
    "fields": [
      {
        "definition": "feature spread",
        "name": "spread",
        "scale": "1000000000",
        "unit": "price"
      },
      {
        "definition": "feature imbalance",
        "name": "imbalance",
        "scale": "1000000000",
        "unit": "ratio from -1 to 1"
      },
      {
        "definition": "feature microprice minus feature midpoint, exact",
        "name": "microprice_minus_midpoint",
        "scale": "1000000000",
        "unit": "price"
      },
      {
        "definition": "feature bid_size",
        "name": "bid_size",
        "scale": "100000000",
        "unit": "shares"
      },
      {
        "definition": "feature ask_size",
        "name": "ask_size",
        "scale": "100000000",
        "unit": "shares"
      }
    ],
    "name": "top_of_book_baseline",
    "row": "one feature row's inputs, in this order; no label, label time, or future observation",
    "version": "1"
  },
  "instrument": "AAPL",
  "lifecycle": {
    "changed_at": null,
    "reason": null,
    "state": "active"
  },
  "limitations": [
    "The logistic model is linear in five top-of-book inputs, fitted on one instrument's training rows.",
    "It was trained on one short recorded session: its training window covers minutes, not market regimes.",
    "Its predictions classify one-horizon midpoint moves; they are not trading signals or evidence of profit."
  ],
  "model_version_id": "2b5b6c77a7f579f621bb2352a7c066565e0085833b7b6be9669637570a4a81b2",
  "names": [
    {
      "name": "aapl-logistic-20260925",
      "version": "1"
    }
  ],
  "output": {
    "class_order": [
      "down",
      "flat",
      "up"
    ],
    "confidence_ppb": "floor(1e9 x the predicted class's probability), exact from its binary64 value",
    "kind": "class_probabilities",
    "probabilities": "one binary64 probability per class, each from 0 to 1, together summing to one"
  },
  "payload": {
    "name": "parameters.json",
    "sha256": "a0e07429e1654ee4cbe07ba6e2ded7b079e35a9e3671da50c7e281567c538939",
    "size_bytes": "749"
  },
  "provenance": {
    "code": {
      "unavailable": "an experiment records its library versions, not a source revision"
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
    "experiment_id": "05be6dd441394993e67b5efadda6dc4f4ac395750fd9a4bc3ae449ac2c16ba62",
    "model": "logistic",
    "origin": "experiment",
    "sources": {
      "dataset_id": "02adcb6d6a1658fb0346b5ac5552131f19c1819cadf5d43d58763e9562855ae8",
      "feature_artifact_id": "06e5d86e14e7123801dce78414895bf3fdb22ceaa51a96f89af9a0b041386854",
      "feature_set": "top_of_book_v1",
      "features_sha256": "abc01f7ffc881de3164fe68afc9bd797b77c724643703c097c6bd75751142d93",
      "horizon_ns": "1000000000",
      "label_artifact_id": "2edb8ddf894593a6718bd2a33a44aa043e7f54d77c6c1ddfb448bb4dd096efba",
      "label_set": "future_midpoint_return_v1",
      "labels_sha256": "7852c973ab62a94d5c0e7365a01d2f770bd618ef3d8008a6ed9c05a879a237c9",
      "split_artifact_id": "4f4bed5c10acbc2d43b85ca45966c9d69d30bdbfb0837b7be0d230eb8c020fa2",
      "split_set": "chronological_purged_v1",
      "splits_sha256": "ed152c338be636b414ab764d18f2a147910175ef4805af3f06d6eab91c3be30e",
      "stream": "databento-equs-mini-mbp1-20260925-1330-1340",
      "test_start_at": "2026-09-25T13:37:46.577861679Z",
      "validation_start_at": "2026-09-25T13:35:19.090266460Z"
    }
  },
  "runtime": {
    "class": "the model's predicted class: the largest logit, ties to the earliest class in class_order",
    "description": "each input standardized with the stored mean and scale, the logits summed in a fixed elementwise order, and the softmax of the logits shifted by their largest; binary64, one row at a time (schedule.logistic_predictions)",
    "name": "chronos.row_pure_logistic",
    "version": "1"
  },
  "source_check": {
    "experiment_id": "05be6dd441394993e67b5efadda6dc4f4ac395750fd9a4bc3ae449ac2c16ba62",
    "state": "matches"
  },
  "target": {
    "definition": "the direction of the midpoint return from the prediction time to the first row of the instrument at least one horizon later (future_midpoint_return version 1): down, flat, or up",
    "horizon_ns": "1000000000",
    "label_set": "future_midpoint_return_v1"
  },
  "training": {
    "class_counts": {
      "down": "8426",
      "flat": "795",
      "up": "7698"
    },
    "information_cutoff": {
      "at": "2026-09-25T13:35:19.090266460Z",
      "basis": "verified",
      "meaning": "no training row, label, or fitted statistic uses information at or after this time"
    },
    "labels_available_through": "2026-09-25T13:35:19.086964037Z",
    "rows": "16919",
    "split": "train"
  },
  "verification": {
    "cutoff": "the information cutoff is the verified split's validation start: every training row is before it, and every training label became available before it",
    "experiment": "the experiment's identity, file checksums, and every stored prediction re-derived from its parameters, at registration",
    "payload": "the payload is the verified model's canonical parameters; its SHA-256 is the parameters_sha256 that schedules and deployments of this model record",
    "sources": "the experiment's split, labels, and features verified by chronos_dataset, each reproduced from its source and the features from their dataset; the experiment names exactly those"
  }
};
