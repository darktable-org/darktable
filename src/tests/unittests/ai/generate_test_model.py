#!/usr/bin/env python3
"""Generate a tiny ONNX test model for AI backend integration tests.

Model: y = x * 2 (element-wise multiply)
Input:  'x' float32 [1, 3, 4, 4]
Output: 'y' float32 [1, 3, 4, 4]

Usage: python3 generate_test_model.py [output_dir]
  Default output_dir: ./models
"""

import json
import os
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def main():
    out_dir = os.path.join(sys.argv[1] if len(sys.argv) > 1 else "models",
                           "test-multiply")
    os.makedirs(out_dir, exist_ok=True)

    # Create model: y = x * 2
    X = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 3, 4, 4])
    Y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 3, 4, 4])
    const_2 = numpy_helper.from_array(
        np.array([2.0], dtype=np.float32), name="const_2"
    )
    mul_node = helper.make_node("Mul", ["x", "const_2"], ["y"])
    graph = helper.make_graph([mul_node], "test_multiply", [X], [Y], [const_2])
    model = helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)]
    )
    # Force IR version 8 for compatibility with ONNX Runtime 1.x
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, os.path.join(out_dir, "model.onnx"))

    # Create config.json
    config = {
        "id": "test-multiply",
        "name": "Test Multiply",
        "description": "Test model: y = x * 2",
        "task": "test",
        "backend": "onnx",
        "num_inputs": 1,
    }
    with open(os.path.join(out_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=2)
        f.write("\n")

    print(f"Generated test model in {out_dir}")


if __name__ == "__main__":
    main()
