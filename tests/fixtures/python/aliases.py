import numpy as np
import os.path as osp
from math import sqrt as square_root, cos as cosine
from datetime import datetime as dt

def compute_dist(x, y):
    val = np.array([x, y])
    return square_root(x * x + y * y)
