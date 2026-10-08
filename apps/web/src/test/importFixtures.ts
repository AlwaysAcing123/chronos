// An imported ONNX model version as the research service describes it: the
// real response for a logistic version exported with export-onnx and
// imported through the sandbox, from synthetic data (hence its 1970 times).
// The manifest is that export's import-manifest.json, byte for byte.
type Json = Record<string, unknown>;

export const IMPORTED_VERSION_ID = 'f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360';
export const IMPORT_ID = '1'.repeat(32);
export const IMPORT_JOB_ID = '2'.repeat(32);
export const IMPORTED_MODEL_SHA256 = '88ce1171085cba29a74e97a376a2473f09b1553f7e50da2facdc9de01b923c0d';

export const IMPORT_MANIFEST_TEXT = "{\n  \"class_order\": [\n    \"down\",\n    \"flat\",\n    \"up\"\n  ],\n  \"declared\": {\n    \"cutoff_covers\": \"training, preprocessing, tuning, and model selection\",\n    \"information_cutoff_ns\": \"1048000000\",\n    \"source\": \"chronos model version 45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855\",\n    \"training\": \"exported by chronos-research export-onnx from the registered logistic version 45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855 of experiment a2181e447bb097255a007b9550329bf3aab51cf914f5c1ff70d9e383bb324aa6, whose cutoff Chronos verified as the split's validation start\"\n  },\n  \"input_tensor\": {\n    \"dtype\": \"float64\",\n    \"name\": \"inputs\",\n    \"shape\": [\n      \"1\",\n      \"5\"\n    ]\n  },\n  \"inputs\": {\n    \"name\": \"top_of_book_baseline\",\n    \"version\": \"1\"\n  },\n  \"instrument\": \"AAPL\",\n  \"name\": \"aapl-logistic-onnx\",\n  \"output_tensor\": {\n    \"dtype\": \"float64\",\n    \"name\": \"probabilities\",\n    \"shape\": [\n      \"1\",\n      \"3\"\n    ]\n  },\n  \"schema\": {\n    \"name\": \"chronos.model_import\",\n    \"version\": \"1\"\n  },\n  \"target\": {\n    \"horizon_ns\": \"150000\",\n    \"label_set\": \"future_midpoint_return_v1\"\n  },\n  \"version\": \"1\"\n}\n";

export function importedDetailJson(overrides: Json = {}): Json {
  return { ...structuredClone(IMPORTED_DETAIL), ...overrides };
}

export function importedListingJson(): Json {
  const { family, instrument, lifecycle, model_version_id, names, payload, training } = importedDetailJson();
  return { family, instrument, lifecycle, model_version_id, names, payload, training, origin: 'import' };
}

export function stagedImportJson(model: Json | null = null): Json {
  return {
    import: {
      created_at: '2026-10-07T21:00:00.000000000Z',
      import_id: IMPORT_ID,
      manifest: JSON.parse(IMPORT_MANIFEST_TEXT) as Json,
      manifest_sha256: 'c2e853798cad5fb341dc6ac26f4ddb4c32fe8d7bff4dd705e0fd093b83ff6201',
      model,
    },
    job: null,
  };
}

export const UPLOADED_MODEL: Json = {
  sha256: IMPORTED_MODEL_SHA256,
  size_bytes: '626',
  uploaded_at: '2026-10-07T21:00:01.000000000Z',
};

export function importJobJson(overrides: Json = {}): Json {
  return {
    job_id: IMPORT_JOB_ID,
    kind: 'import',
    state: 'queued',
    created_at: '2026-10-07T21:00:02.000000000Z',
    started_at: null,
    finished_at: null,
    request: {
      import_id: IMPORT_ID,
      instrument: 'AAPL',
      manifest_sha256: 'c2e853798cad5fb341dc6ac26f4ddb4c32fe8d7bff4dd705e0fd093b83ff6201',
      model_sha256: IMPORTED_MODEL_SHA256,
      name: 'aapl-logistic-onnx',
      version: '1',
    },
    result: null,
    error: null,
    log: '',
    ...overrides,
  };
}

const IMPORTED_DETAIL: Json = {
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
  "family": "onnx",
  "format": {
    "name": "onnx",
    "payload": "model.onnx",
    "profile": "chronos.onnx_classifier/1",
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
    "An imported model's training, data, target, and cutoff are its uploader's declarations. Chronos verified the file, its contract, and its outputs on a fixed smoke corpus, not how it was made.",
    "Any result from it is conditional on the declared cutoff being true; a cutoff Chronos cannot check cannot rule out look-ahead.",
    "Its predictions classify one-horizon midpoint moves; they are not trading signals or evidence of profit."
  ],
  "model_version_id": "f40a3cafd9653d5362d1fefea09cbf45a7abdcd4cf340237ee53de70fcb09360",
  "names": [
    {
      "name": "aapl-logistic-onnx",
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
    "name": "model.onnx",
    "sha256": "88ce1171085cba29a74e97a376a2473f09b1553f7e50da2facdc9de01b923c0d",
    "size_bytes": "626"
  },
  "provenance": {
    "declared": {
      "cutoff_covers": "training, preprocessing, tuning, and model selection",
      "information_cutoff": "1970-01-01T00:00:01.048000000Z",
      "source": "chronos model version 45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855",
      "training": "exported by chronos-research export-onnx from the registered logistic version 45fed5d925de85e9398418ab482b14ddaee7b85dbbac9c2e5a86419f2a9b5855 of experiment a2181e447bb097255a007b9550329bf3aab51cf914f5c1ff70d9e383bb324aa6, whose cutoff Chronos verified as the split's validation start"
    },
    "manifest_sha256": "c2e853798cad5fb341dc6ac26f4ddb4c32fe8d7bff4dd705e0fd093b83ff6201",
    "not_verified": {
      "cutoff": "the declared information cutoff: a file's checksum says nothing about when, or on what data, it was trained, preprocessed, tuned, or selected",
      "target": "that the model was trained to predict this target at this horizon",
      "training": "how the model was trained, and on what data"
    },
    "origin": "import",
    "validation": {
      "onnxruntime": "1.30.0",
      "profile": {
        "initializer_elements": "28",
        "initializers": "4",
        "ir_version": "8",
        "nodes": "5",
        "operators": [
          "Add",
          "Div",
          "MatMul",
          "Softmax",
          "Sub"
        ],
        "opset": "17",
        "profile": {
          "name": "chronos.onnx_classifier",
          "version": "1"
        }
      },
      "sandbox_content_sha256": "0f2742da590ccc4f54a2acc7c8dba6c3d9b52c6d2f5c94e384ef31daaa299f15",
      "smoke": {
        "corpus": "chronos.onnx_smoke/1",
        "corpus_sha256": "44046cf61e660ea338ba2c34021e1101e2fc141b9891ace6204697b1aa7a26ce",
        "outputs_sha256": "60e427a3c015bd64f493211836901239c1707037f44bb81d161cc1c52fc2724c",
        "rows": "24"
      }
    }
  },
  "runtime": {
    "class": "the first largest probability, so ties go to the earliest class in class_order",
    "description": "ONNX Runtime on its CPU provider alone, one thread, no graph optimization, one row per run, inside the restricted ONNX sandbox container, which checks the profile again at every load",
    "name": "chronos.onnx_sandbox",
    "version": "1"
  },
  "source_check": {
    "reason": "imported_model_has_no_source_experiment",
    "state": "not_applicable"
  },
  "target": {
    "definition": "the direction of the midpoint return from the prediction time to the first row of the instrument at least one horizon later (future_midpoint_return version 1): down, flat, or up",
    "horizon_ns": "150000",
    "label_set": "future_midpoint_return_v1"
  },
  "training": {
    "information_cutoff": {
      "at": "1970-01-01T00:00:01.048000000Z",
      "basis": "declared",
      "meaning": "the uploader declares that no information at or after this time was used in training, preprocessing, tuning, or model selection; Chronos did not verify it"
    }
  },
  "verification": {
    "artifact": "the payload is exactly the bytes the sandbox validated: their size and SHA-256",
    "isolation": "the bytes were parsed, checked, and run only inside the restricted ONNX sandbox container: no network, a read-only filesystem with nothing of the host mounted, an unprivileged user, and bounded memory, CPU, processes, and time",
    "outputs": "every smoke row gave exactly three finite probabilities from 0 to 1 summing to one within 1e-12, judged on the host",
    "profile": "chronos.onnx_classifier/1: one double input [1, 5], one double output [1, 3], a small static graph of allowed default-domain operators, inline finite parameters, and no subgraph, function, or external data",
    "smoke_test": "24 fixed rows of the top_of_book_baseline/1 inputs, one at a time, and the first again with identical output"
  }
};
