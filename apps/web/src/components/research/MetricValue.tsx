import type { Metric } from '../../api/researchSchemas';
import { metricNumber, metricPercent } from '../../presentation/research';
import { Unavailable } from '../ui/DecimalText';

// A classification metric, as a percentage or as a plain number, with its
// exact value in the title; or why it has no value, never a zero.
export function MetricValue({ metric, as = 'percent' }: { readonly metric: Metric; readonly as?: 'percent' | 'number' }) {
  return 'value' in metric ? (
    <span className="numeric" title={metric.value}>
      {as === 'percent' ? metricPercent(metric.value) : metricNumber(metric.value)}
    </span>
  ) : (
    <Unavailable>{`Undefined: ${metric.undefined.replaceAll('_', ' ')}`}</Unavailable>
  );
}
