from . import config
from .utils import helper
from ..parent import base_service
from ..core.worker import Worker as ServiceWorker
from . import *

def run_task():
    helper()
    config.load()
