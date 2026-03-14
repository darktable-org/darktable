# General Case Performance Tests for Diffuse Module

These tests are designed to verify that optimizations targeting built-in presets do not degrade performance for arbitrary parameter combinations.

## Test Case G1: Full Anisotropic Mess
- **Description**: All 4 scales have different, non-zero weights and anisotropy values. Non-zero sharpness and regularization.
- **Parameters**:
  - iterations: 10
  - first: -0.12, second: 0.08, third: -0.15, fourth: 0.11
  - anisotropy_first: 1.5, anisotropy_second: -0.5, anisotropy_third: 0.7, anisotropy_fourth: 1.2
  - radius: 8, radius_center: 4
  - sharpness: 0.01, regularization: 2.0, variance_threshold: 0.5

## Test Case G2: Strong Mixed Diffusion
- **Description**: High iterations with strong, mixed-sign weights and anisotropy.
- **Parameters**:
  - iterations: 20
  - first: 0.25, second: -0.15, third: 0.10, fourth: -0.05
  - anisotropy_first: -2.0, anisotropy_second: 3.0, anisotropy_third: -1.0, anisotropy_fourth: 0.5
  - radius: 12, radius_center: 0
  - sharpness: 0.005, regularization: 1.5, variance_threshold: -0.1

## Test Case G3: Subtle Complex Sharpening
- **Description**: Lower iterations but high anisotropy values and non-zero sharpness.
- **Parameters**:
  - iterations: 5
  - first: -0.05, second: -0.05, third: -0.05, fourth: -0.05
  - anisotropy_first: 5.0, anisotropy_second: 2.5, anisotropy_third: 7.5, anisotropy_fourth: 1.0
  - radius: 4, radius_center: 2
  - sharpness: 0.02, regularization: 3.5, variance_threshold: 0.0

## Execution Protocol
- **Command**: `python3 benchmark_general_case.py` (to be created)
- **Iteration Count**: 20
- **Baseline Source**: `master` branch (established on 2026-03-07)
