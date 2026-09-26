"""
Telemetry event processor and aggregation engine.
"""

from typing import List, Dict, Optional

from .decorators import timed_operation
from .models import DeviceState, TelemetryEvent


@timed_operation
def aggregate_events(events: List[TelemetryEvent]) -> Dict[str, float]:
    """Calculate average metric values grouped by metric name."""
    totals: Dict[str, float] = {}
    counts: Dict[str, int] = {}

    for evt in events:
        if evt.is_healthy():
            totals[evt.metric_name] = totals.get(evt.metric_name, 0.0) + evt.metric_value
            counts[evt.metric_name] = counts.get(evt.metric_name, 0) + 1

    averages: Dict[str, float] = {}
    for name, sum_val in totals.items():
        cnt = counts.get(name, 1)
        averages[name] = sum_val / cnt

    return averages


class EventProcessor:
    """Processor managing telemetry ingestion and batching."""

    def __init__(self, name: str):
        self.name = name
        self._buffer: List[TelemetryEvent] = []

    def ingest(self, event: TelemetryEvent) -> None:
        self._buffer.append(event)

    @timed_operation
    def process_batch(self) -> Dict[str, float]:
        summary = aggregate_events(self._buffer)
        self._buffer.clear()
        return summary

    def pending_count(self) -> int:
        return len(self._buffer)
