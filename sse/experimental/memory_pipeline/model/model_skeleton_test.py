import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import legacy_midmix_sse as model

assert len(model.FACTOR_NAMES) == model.FEATURE_COUNT == 50
assert model.is_batch_end(1000, 1101)
assert not model.is_batch_end(1000, 1100)
assert not model.is_batch_end(1000, None)
value = model.target_permille(100.0, 100.1, 99.9, 100.2)
assert abs(value - 0.6666666667) < 1e-6, value
print("sse_model_skeleton_test: PASS")
