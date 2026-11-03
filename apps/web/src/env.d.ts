/// <reference types="vite/client" />

interface ImportMetaEnv {
  // "1" only in a demo workspace's build (tools/demo, ADR 0025).
  readonly VITE_CHRONOS_DEMO?: string;
}
