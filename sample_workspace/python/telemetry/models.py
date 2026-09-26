"""
Domain models for telemetry data points.
"""

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import Dict, Optional


class DeviceState(Enum):
    BOOTING = "booting"
    ONLINE = "online"
    DEGRADED = "degraded"
    OFFLINE = "offline"


@dataclass
class TelemetryEvent:
    event_id: str
    device_id: str
    metric_name: str
    metric_value: float
    state: DeviceState = DeviceState.ONLINE
    timestamp: datetime = field(default_factory=datetime.utcnow)
    attributes: Dict[str, str] = field(default_factory=dict)

    def is_healthy(self) -> bool:
        return self.state in (DeviceState.ONLINE, DeviceState.BOOTING)

    def to_dict(self) -> Dict[str, object]:
        return {
            "event_id": self.event_id,
            "device_id": self.device_id,
            "metric_name": self.metric_name,
            "metric_value": self.metric_value,
            "state": self.state.value,
        }
