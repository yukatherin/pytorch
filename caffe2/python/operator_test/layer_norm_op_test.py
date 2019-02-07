from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from caffe2.python import brew, core
from caffe2.python.model_helper import ModelHelper
from hypothesis import given
import caffe2.python.hypothesis_test_util as hu
import caffe2.python.serialized_test.serialized_test_util as serial
import numpy as np
import os
import unittest
from functools import partial
import torch


def _layer_norm_ref(axis, epsilon, X):
    left = int(np.prod(X.shape[:axis]))
    reshaped = np.reshape(X, [left, -1])
    mean = np.mean(reshaped, axis=1).reshape([left, 1])
    stdev = np.sqrt(
        np.mean(np.square(reshaped), axis=1).reshape([left, 1]) -
        np.power(mean, 2) + epsilon
    )
    norm = (reshaped - mean) / (stdev)
    norm = np.reshape(norm, X.shape)
    mean = np.reshape(mean, X.shape[:axis] + (1,))
    stdev = np.reshape(stdev, X.shape[:axis] + (1,))
    return [norm, mean, stdev]


def _layer_norm_grad_ref(axis, gout_full, norm, mean_full, stdev_full, X_full):
    left = int(np.prod(X_full.shape[:axis]))
    right = int(np.prod(X_full.shape[axis:]))
    X = np.reshape(X_full, [left, right])
    stdev = np.reshape(stdev_full, [left, 1])
    mean = np.reshape(mean_full, [left, 1])
    gout = np.reshape(gout_full, [left, right])
    dstdev_end = (-1.0) / np.power(stdev, 2.0) \
            * np.sum((X - mean) * gout, axis=1).reshape([left, 1])
    dmean_end = np.sum(-1.0 / stdev * gout, axis=1).reshape([left, 1])
    dx_end = 1.0 / stdev * gout

    # stdev block
    dmean_stdev = -1.0 * mean / stdev * dstdev_end
    dx_stdev = X / (right * stdev) * dstdev_end

    # mean block
    dmean = dmean_end + dmean_stdev
    dxmean = (1.0 / right) * dmean

    # final outputs
    dx = dx_end + dx_stdev + dxmean
    dx = dx.reshape(X_full.shape)

    return [dx]


class TestLayerNormOp(serial.SerializedTestCase):
    @serial.given(X=hu.tensor(min_dim=2), **hu.gcs)
    def test_layer_norm_grad_op(self, X, gc, dc):
        axis = np.random.randint(0, len(X.shape))
        epsilon = 1e-4
        op = core.CreateOperator(
            "LayerNormGradient",
            ["gout", "out", "mean", "stdev", "in"],
            ["gin"],
            axis=axis,
            epsilon=epsilon,
        )

        norm, mean, stdev = _layer_norm_ref(axis, epsilon, X)
        gout = norm

        self.assertReferenceChecks(
            device_option=gc,
            op=op,
            inputs=[gout, norm, mean, stdev, X],
            reference=partial(_layer_norm_grad_ref, axis)
        )
        self.assertDeviceChecks(
            device_options=dc,
            op=op,
            inputs=[gout, norm, mean, stdev, X],
            outputs_to_check=[0],
        )

    @given(X=hu.tensor(min_dim=2), **hu.gcs)
    def test_layer_norm_op(self, X, gc, dc):
        axis = np.random.randint(0, len(X.shape))
        epsilon = 1e-4
        op = core.CreateOperator(
            "LayerNorm",
            ["input"],
            ["output", "mean", "stdev"],
            axis=axis,
            epsilon=epsilon,
        )

        self.assertReferenceChecks(
            device_option=gc,
            op=op,
            inputs=[X],
            reference=partial(_layer_norm_ref, axis, epsilon)
        )
        self.assertDeviceChecks(
            device_options=dc,
            op=op,
            inputs=[X],
            outputs_to_check=[0, 1, 2],
        )

    @given(X=hu.tensor(min_dim=2), **hu.gcs_cpu_only)
    @unittest.skip("Tensor interop enforcement needs fixing")
    def test_layer_norm_op_c10(self, X, gc, dc):
        axis = np.random.randint(0, len(X.shape))
        epsilon = 1e-4
        op = core.CreateOperator(
            "C10LayerNorm_DontUseThisOpYet",
            ["input"],
            ["output", "mean", "stdev"],
            axis=axis,
            epsilon=epsilon,
        )

        self.assertReferenceChecks(
            device_option=gc,
            op=op,
            inputs=[X],
            reference=partial(_layer_norm_ref, axis, epsilon)
        )
        self.assertDeviceChecks(
            device_options=dc,
            op=op,
            inputs=[X],
            outputs_to_check=[0, 1, 2],
        )

    @given(X=hu.tensor(min_dim=2), **hu.gcs)
    def test_layer_norm_op_pytorch(self, X, gc, dc):
        axis = np.random.randint(0, len(X.shape))
        epsilon = 1e-4

        expected_norm, expected_mean, expected_stdev = _layer_norm_ref(axis, epsilon, X)
        actual_norm, actual_mean, actual_stdev = torch.ops.caffe2.layer_norm_dont_use_this_op_yet(torch.tensor(X), axis, epsilon)

        torch.testing.assert_allclose(expected_norm, actual_norm)
        torch.testing.assert_allclose(expected_mean, actual_mean)
        torch.testing.assert_allclose(expected_stdev, actual_stdev)

    @given(X=hu.tensor(min_dim=2), **hu.gcs)
    def test_layer_norm_op_jit(self, X, gc, dc):
        @torch.jit.script
        def jit_layer_norm(tensor, axis, epsilon):
            # type: (Tensor, int, float) -> Tuple[Tensor, Tensor, Tensor]
            norm, mean, stdev = torch.ops.caffe2.layer_norm_dont_use_this_op_yet(tensor, axis, epsilon)
            return norm, mean, stdev

        axis = np.random.randint(0, len(X.shape))
        epsilon = 1e-4

        actual_norm, actual_mean, actual_stdev = jit_layer_norm(torch.tensor(X), axis, epsilon)
        expected_norm, expected_mean, expected_stdev = _layer_norm_ref(axis, epsilon, X)

        torch.testing.assert_allclose(expected_norm, actual_norm)
        torch.testing.assert_allclose(expected_mean, actual_mean)
        torch.testing.assert_allclose(expected_stdev, actual_stdev)

    @given(X=hu.tensor(min_dim=2), **hu.gcs)
    def test_layer_norm_brew_wrapper(self, X, gc, dc):
        axis = np.random.randint(0, len(X.shape))
        scale_dim = [1] * np.ndim(X)
        scale_dim[axis] = X.shape[axis]

        self.ws.create_blob('input').feed(X)

        model = ModelHelper(name='test_layer_norm_brew_wrapper')
        brew.layer_norm(
            model,
            'input',
            'output',
            dim_in=X.shape[axis],
            axis=axis,
            epsilon=1e-4,
        )

        self.ws.create_net(model.param_init_net).run()
        self.ws.create_net(model.net).run()
