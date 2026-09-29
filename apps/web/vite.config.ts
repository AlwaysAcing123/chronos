import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

// The C++ API listens on localhost only. The browser talks to this server's
// origin, and the proxy forwards API paths to the C++ process, so the C++
// server never needs to approve cross-origin requests.
const chronosApi = process.env.CHRONOS_API_ORIGIN ?? 'http://127.0.0.1:8080';

const apiProxy = {
  '/api': { target: chronosApi },
  '/health': { target: chronosApi },
};

export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    // Without CORS approval here, a cross-origin page cannot use this dev
    // server to reach the proxied API either.
    cors: false,
    proxy: apiProxy,
  },
  preview: {
    host: '127.0.0.1',
    port: 4173,
    strictPort: true,
    cors: false,
    proxy: apiProxy,
  },
  build: {
    target: 'es2023',
    sourcemap: true,
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.{ts,tsx}'],
    setupFiles: ['src/test/setup.ts'],
    restoreMocks: true,
    unstubGlobals: true,
  },
});
