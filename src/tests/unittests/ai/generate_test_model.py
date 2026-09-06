#!/usr/bin/env python3
"""Generate tiny ONNX test models for AI backend integration tests.

test-multiply          y = x * 2, static  [1, 3, 4, 4]
test-multiply-dynamic  y = x * 2, symbolic spatial dims [1, 3, H, W]

The dynamic variant exists so the backend's ORT-allocated copy-back path
is reachable: it only runs when an output dimension is unresolved.

Usage: python3 generate_test_model.py [output_dir]
  Default output_dir: ./models
"""

import json
import os
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def _build(out_dir, model_id, name, shape):
    os.makedirs(out_dir, exist_ok=True)

    X = helper.make_tensor_value_info("x", TensorProto.FLOAT, shape)
    Y = helper.make_tensor_value_info("y", TensorProto.FLOAT, shape)
    const_2 = numpy_helper.from_array(
        np.array([2.0], dtype=np.float32), name="const_2"
    )
    mul_node = helper.make_node("Mul", ["x", "const_2"], ["y"])
    graph = helper.make_graph(
        [mul_node], model_id.replace("-", "_"), [X], [Y], [const_2]
    )
    model = helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)]
    )
    # Force IR version 8 for compatibility with ONNX Runtime 1.x
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, os.path.join(out_dir, "model.onnx"))

    config = {
        "id": model_id,
        "name": name,
        "description": "Test model: y = x * 2",
        "task": "test",
        "backend": "onnx",
        "num_inputs": 1,
    }
    with open(os.path.join(out_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=2)
        f.write("\n")

    print(f"Generated {model_id} in {out_dir}")


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "models"

    _build(os.path.join(base, "test-multiply"),
           "test-multiply", "Test Multiply", [1, 3, 4, 4])

    # "H" and "W" are dim_params, so ORT reports the output shape as
    # unresolved and dt_ai_run() takes its ORT-allocated copy-back path
    _build(os.path.join(base, "test-multiply-dynamic"),
           "test-multiply-dynamic", "Test Multiply Dynamic",
           [1, 3, "H", "W"])


if __name__ == "__main__":
    main()
