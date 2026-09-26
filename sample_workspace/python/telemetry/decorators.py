"""
Execution decorators for telemetry operations.
"""

import functools
import time
from typing import Any, Callable


def timed_operation(func: Callable[..., Any]) -> Callable[..., Any]:
    """Decorator measuring and recording the execution time of a function."""
    @functools.wraps(func)
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        start_time = time.perf_counter()
        result = func(*args, **kwargs)
        duration_ms = (time.perf_counter() - start_time) * 1000.0
        return result
    return wrapper


def retry_on_error(max_retries: int = 3) -> Callable[..., Any]:
    """Decorator retrying an operation on exception."""
    def decorator(func: Callable[..., Any]) -> Callable[..., Any]:
        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            last_err = None
            for _ in range(max_retries):
                try:
                    return func(*args, **kwargs)
                except Exception as err:
                    last_err = err
            if last_err is not None:
                raise last_err
        return wrapper
    return decorator
