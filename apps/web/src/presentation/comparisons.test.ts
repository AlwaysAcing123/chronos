import { describe, expect, it } from 'vitest';

import { memberReason } from './comparisons';

describe('member reasons', () => {
  it('names each refusal and failure the service sends', () => {
    expect(memberReason('horizon_mismatch')).toBe('Predicts another horizon than the split’s labels');
    expect(memberReason('schedule_not_published')).toBe('Its schedule could not be stored');
    expect(memberReason('replay_failed')).toBe('The run did not replay identically');
  });

  it('tells an unavailable sandbox from a failed one', () => {
    expect(memberReason('sandbox_unavailable:image_not_built')).toBe('The ONNX sandbox is unavailable');
    expect(memberReason('sandbox_timeout')).toBe('The ONNX sandbox failed');
  });

  it('reads any other code as an invalid prediction', () => {
    expect(memberReason('non_finite_output')).toBe('Its predictions are not valid');
  });
});
