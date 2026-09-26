"""
Telemetry package for sample workspace.
"""

__version__ = "1.0.0"
__author__ = "CodeLenses Sample"

from .models import DeviceState, TelemetryEvent
from .processor import EventProcessor, aggregate_events

__all__ = [
    "DeviceState",
    "TelemetryEvent",
    "EventProcessor",
    "aggregate_events",
]
