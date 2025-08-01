# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


from typing import Callable

import torch

from executorch.backends.test.suite import dtype_test, operator_test, OperatorTest


class Model(torch.nn.Module):
    def __init__(self, inplace=False):
        super().__init__()
        self.inplace = inplace

    def forward(self, x):
        return torch.nn.functional.hardsigmoid(x, inplace=self.inplace)


@operator_test
class TestHardsigmoid(OperatorTest):
    @dtype_test
    def test_hardsigmoid_dtype(self, dtype, tester_factory: Callable) -> None:
        self._test_op(Model(), ((torch.rand(2, 10)).to(dtype),), tester_factory)

    def test_hardsigmoid_f32_single_dim(self, tester_factory: Callable) -> None:
        self._test_op(Model(), (torch.randn(20),), tester_factory)

    def test_hardsigmoid_f32_multi_dim(self, tester_factory: Callable) -> None:
        self._test_op(Model(), (torch.randn(2, 3, 4, 5),), tester_factory)

    def test_hardsigmoid_f32_inplace(self, tester_factory: Callable) -> None:
        self._test_op(Model(inplace=True), (torch.randn(3, 4, 5),), tester_factory)

    def test_hardsigmoid_f32_boundary_values(self, tester_factory: Callable) -> None:
        # Test with values that span the hardsigmoid's piecewise regions
        x = torch.tensor([-5.0, -3.0, -1.0, 0.0, 1.0, 3.0, 5.0])
        self._test_op(Model(), (x,), tester_factory)
