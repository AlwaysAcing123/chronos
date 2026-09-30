import type { NoticeStream, OpenNoticeStream } from '../state/marketDataStream';

// A controllable stand-in for the browser's EventSource.
export class FakeNoticeStream implements NoticeStream {
  readyState = 0;
  onopen: ((event: Event) => void) | null = null;
  onerror: ((event: Event) => void) | null = null;
  closed = false;
  private readonly listeners: ((event: MessageEvent) => void)[] = [];

  addEventListener(_type: 'market-data', listener: (event: MessageEvent) => void): void {
    this.listeners.push(listener);
  }

  close(): void {
    this.closed = true;
    this.readyState = 2;
  }

  open(): void {
    this.readyState = 1;
    this.onopen?.(new Event('open'));
  }

  // A change notice; with an activity revision, as servers with working
  // orders send.
  notice(revision: string, activityRevision?: string): void {
    const data = activityRevision === undefined ? { revision } : { revision, activity_revision: activityRevision };
    for (const listener of this.listeners) {
      listener(new MessageEvent('market-data', { data: JSON.stringify(data) }));
    }
  }

  // A dropped connection the browser retries by itself.
  drop(): void {
    this.readyState = 0;
    this.onerror?.(new Event('error'));
  }

  // A refused connection the browser abandons.
  refuse(): void {
    this.readyState = 2;
    this.onerror?.(new Event('error'));
  }
}

export function fakeNoticeStreams(): { open: OpenNoticeStream; streams: FakeNoticeStream[] } {
  const streams: FakeNoticeStream[] = [];
  return {
    streams,
    open: () => {
      const stream = new FakeNoticeStream();
      streams.push(stream);
      return stream;
    },
  };
}
