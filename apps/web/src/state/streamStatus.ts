import { createContext, useContext } from 'react';

// Whether market-dependent reads follow the server's change stream.
export type StreamStatus = 'connecting' | 'streaming' | 'polling';

export const StreamStatusContext = createContext<StreamStatus>('polling');

export function useStreamStatus(): StreamStatus {
  return useContext(StreamStatusContext);
}
