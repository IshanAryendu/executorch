# Copyright 2023-2025 Arm Limited and/or its affiliates.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import itertools
from typing import Any, List

import torch

from executorch.backends.arm._passes.fold_qdq_with_annotated_qparams_pass import (
    get_input_qparams,
    get_output_qparams,
)
from executorch.backends.arm.operators.node_visitor import (
    NodeVisitor,
    register_node_visitor,
)
from executorch.backends.arm.operators.operator_validation_utils import (
    validate_num_inputs,
)
from executorch.backends.arm.tosa_mapping import TosaArg
from executorch.backends.arm.tosa_quant_utils import build_rescale, build_rescale_v0_80
from executorch.backends.arm.tosa_specification import TosaSpecification
from executorch.backends.arm.tosa_utils import build_reshape, tosa_shape


@register_node_visitor
class Conv2dVisitor_0_80(NodeVisitor):
    target = "aten.convolution.default"

    tosa_specs = [
        TosaSpecification.create_from_string("TOSA-0.80+BI"),
        TosaSpecification.create_from_string("TOSA-0.80+MI"),
    ]

    def __init__(self, *args):
        super().__init__(*args)

    # torch.nn.Conv2d does not require the result of
    # `(input + 2 * pad - dilation * (weight - 1) - 1) / stride`
    # must be an integer, but tosa currently strictly require this property.
    # This function adjusts the pad value to meet the requirement.
    def adjust_pad_if_needed(
        self, input_size: int, input_weight: int, stride: int, pad: int, dilation: int
    ) -> int:
        mod_remainder = (
            input_size + 2 * pad - dilation * (input_weight - 1) - 1
        ) % stride

        # No need to adjust
        if mod_remainder == 0:
            return pad

        if mod_remainder > pad:
            raise RuntimeError(
                "This case should be handled by the SizeAdjustConv2d pass, is it enabled?"
            )
        return pad - mod_remainder

    def define_node(
        self,
        node: torch.fx.Node,
        tosa_graph: Any,
        inputs: List[TosaArg],
        output: TosaArg,
    ) -> None:

        import tosa_tools.v0_80.serializer.tosa_serializer as ts  # type: ignore

        input, weight, bias, stride, pad, dilation, _, _, group = inputs
        validate_num_inputs(self.target, inputs, 9)

        # Get the attributes of convolution.
        attr = ts.TosaSerializerAttribute()
        pad_attr = [val for val in pad.special for _ in (0, 1)]
        stride_attr = stride.special
        dilation_attr = dilation.special

        # Adjust the pad value if needed to meet the strict convolution output shape calculation.
        pad_attr[1] = self.adjust_pad_if_needed(
            input.shape[2],
            weight.shape[2],
            stride_attr[0],
            pad_attr[1],
            dilation_attr[0],
        )
        pad_attr[3] = self.adjust_pad_if_needed(
            input.shape[3],
            weight.shape[3],
            stride_attr[1],
            pad_attr[3],
            dilation_attr[1],
        )

        input_zp = 0
        if inputs[0].dtype == ts.DType.INT8:
            # int8 input requires quantization information
            input_qparams = get_input_qparams(node)
            input_zp = input_qparams[0].get_zp_per_tensor()

        attr.ConvAttribute(
            pad=pad_attr,
            stride=stride_attr,
            dilation=dilation_attr,
            input_zp=input_zp,
            weight_zp=0,
            local_bound=False,
        )

        # The output type is int32 when input type is int8.
        conv2d_output_name = output.name
        if output.dtype == ts.DType.INT8:
            conv2d_res = tosa_graph.addIntermediate(
                tosa_shape(output.shape, output.dim_order), ts.DType.INT32
            )
            conv2d_output_name = conv2d_res.name

        # Given input.shape is (N, Ci, H, W), and weight.shape is (Co, Ci/G, H, W)
        in_channels = input.shape[1]
        out_channels = weight.shape[0]
        if (in_channels == group.number) and (out_channels % in_channels) == 0:
            """Depthwise convolution case"""
            # Reshape torch shape format of weight tensor to tosa required format.
            # https://www.mlplatform.org/tosa/tosa_spec.html#_depthwise_conv2d
            m_length = int(out_channels / in_channels)
            weight_post_shape = (
                weight.shape[2],
                weight.shape[3],
                in_channels,
                m_length,
            )

            weight_reshaped = tosa_graph.addIntermediate(
                weight_post_shape,
                weight.dtype,
            )
            build_reshape(
                tosa_graph, weight.name, weight_post_shape, weight_reshaped.name
            )
            tosa_op = ts.TosaOp.Op().DEPTHWISE_CONV2D
            weight_name = weight_reshaped.name
        else:
            """Regular convolution case"""
            tosa_op = ts.TosaOp.Op().CONV2D
            weight_name = weight.name

        tosa_graph.addOperator(
            tosa_op,
            [
                input.name,
                weight_name,
                bias.name,
            ],
            [conv2d_output_name],
            attr,
        )

        # For quantized convolution, rescale the output value back to the same
        # integer value domain of the next op. Otherwise return float32 output.
        if inputs[0].dtype == ts.DType.INT8:
            # Get scale_factor from input, weight, and output.
            input_scale = input_qparams[0].get_scale_per_tensor()  # type: ignore[possibly-undefined]  # pyre-ignore [61]

            per_channel_quant = input_qparams[1].per_channel  # pyre-ignore [61]
            if per_channel_quant:
                weight_scale = input_qparams[1].get_scale_per_channel()
            else:
                weight_scale = [
                    input_qparams[1].get_scale_per_tensor()
                ]  # pyre-ignore [61]
            output_qargs = get_output_qparams(node)
            post_conv2d_scale = [
                (inp * w) / out
                for inp, w, out in zip(
                    itertools.cycle([input_scale]),
                    weight_scale,
                    itertools.cycle([output_qargs[0].get_scale_per_tensor()]),
                )
            ]

            build_rescale_v0_80(
                tosa_fb=tosa_graph,
                scale=post_conv2d_scale,
                input_node=conv2d_res,  # type: ignore[possibly-undefined]
                output_name=output.name,
                output_type=output.dtype,
                input_zp=[0],
                output_zp=[output_qargs[0].get_zp_per_tensor()],
                per_channel=per_channel_quant,
            )  # type: ignore[call-arg]


@register_node_visitor
class Conv2dVisitor(NodeVisitor):
    target = "aten.convolution.default"

    tosa_specs = [
        TosaSpecification.create_from_string("TOSA-1.0+INT"),
        TosaSpecification.create_from_string("TOSA-1.0+FP"),
    ]

    def __init__(self, *args):
        super().__init__(*args)

    # torch.nn.Conv2d does not require the result of
    # `(input + 2 * pad - dilation * (weight - 1) - 1) / stride`
    # to be an integer, but tosa currently strictly require this property.
    # This function adjusts the pad value to meet the requirement.
    def adjust_pad_if_needed(
        self, input_size: int, input_weight: int, stride: int, pad: int, dilation: int
    ) -> int:
        mod_remainder = (
            input_size + 2 * pad - dilation * (input_weight - 1) - 1
        ) % stride

        # No need to adjust
        if mod_remainder == 0:
            return pad

        if mod_remainder > pad:
            raise RuntimeError(
                "This case should be handled by the SizeAdjustConv2d pass, is it enabled?"
            )
        return pad - mod_remainder

    def define_node(
        self,
        node: torch.fx.Node,
        tosa_graph: Any,
        inputs: List[TosaArg],
        output: TosaArg,
    ) -> None:

        import serializer.tosa_serializer as ts  # type: ignore
        from tosa.RoundingMode import RoundingMode  # type: ignore

        input, weight, bias, stride, pad, dilation, _, _, group = inputs
        validate_num_inputs(self.target, inputs, 9)

        # Get the attributes of convolution.
        attr = ts.TosaSerializerAttribute()
        pad_attr = [val for val in pad.special for _ in (0, 1)]
        stride_attr = stride.special
        dilation_attr = dilation.special

        # Adjust the pad value if needed to meet the
        # strict convolution output shape calculation.
        pad_attr[1] = self.adjust_pad_if_needed(
            input.shape[2],
            weight.shape[2],
            stride_attr[0],
            pad_attr[1],
            dilation_attr[0],
        )
        pad_attr[3] = self.adjust_pad_if_needed(
            input.shape[3],
            weight.shape[3],
            stride_attr[1],
            pad_attr[3],
            dilation_attr[1],
        )

        input_zp = 0
        if inputs[0].dtype == ts.DType.INT8:
            # int8 input requires quantization information
            input_qparams = get_input_qparams(node)
            input_zp = input_qparams[0].get_zp_per_tensor()

        weight_zp = 0
        if inputs[1].dtype == ts.DType.INT8:
            # int8 weights requires quantization information
            input_qparams = get_input_qparams(node)
            weight_zp = input_qparams[1].zp  # type: ignore[assignment]

        # The output type is int32 when input type is int8.
        conv2d_output_name = output.name
        if output.dtype == ts.DType.INT8:
            conv2d_res = tosa_graph.addIntermediate(
                tosa_shape(output.shape, output.dim_order), ts.DType.INT32
            )
            conv2d_output_name = conv2d_res.name
        acc_type = (
            inputs[0].dtype if inputs[0].dtype == ts.DType.FP32 else ts.DType.INT32
        )

        tosa_graph.addConst(
            [1], output.dtype, [input_zp], name=f"{conv2d_output_name}_input_zp"
        )
        tosa_graph.addConst(
            [1],
            output.dtype,
            weight_zp,
            name=f"{conv2d_output_name}_weight_zp",
        )

        # Given input.shape is (N, Ci, H, W), and weight.shape is (Co, Ci/G, H, W)
        in_channels = input.shape[1]
        out_channels = weight.shape[0]
        if (in_channels == group.number) and (out_channels % in_channels) == 0:
            """Depthwise convolution case"""
            # Reshape torch shape format of weight tensor to tosa required format.
            # https://www.mlplatform.org/tosa/tosa_spec.html#_depthwise_conv2d
            m_length = int(out_channels / in_channels)
            weight_post_shape = [
                weight.shape[2],
                weight.shape[3],
                in_channels,
                m_length,
            ]

            weight_reshaped = tosa_graph.addIntermediate(
                weight_post_shape,
                weight.dtype,
            )
            shape = tosa_graph.addConst(
                [len(weight_post_shape)],
                ts.DType.SHAPE,
                weight_post_shape,
                name=weight_reshaped.name + "_shape",
            )

            reshape_attr = ts.TosaSerializerAttribute()
            reshape_attr.ReshapeAttribute()
            tosa_graph.addOperator(
                ts.TosaOp.Op().RESHAPE,
                [weight.name, shape.name],
                [weight_reshaped.name],
                reshape_attr,
            )

            attr = ts.TosaSerializerAttribute()
            tosa_op = ts.TosaOp.Op().DEPTHWISE_CONV2D
            weight_name = weight_reshaped.name

            attr.DepthwiseConv2dAttribute(
                pad=pad_attr,
                stride=stride_attr,
                dilation=dilation_attr,
                local_bound=False,
                acc_type=acc_type,
            )
        else:
            """Regular convolution case"""
            tosa_op = ts.TosaOp.Op().CONV2D
            weight_name = weight.name

            attr.Conv2dAttribute(
                pad=pad_attr,
                stride=stride_attr,
                dilation=dilation_attr,
                local_bound=False,
                acc_type=acc_type,
            )

        tosa_graph.addOperator(
            tosa_op,
            [
                input.name,
                weight_name,
                bias.name,
                f"{conv2d_output_name}_input_zp",
                f"{conv2d_output_name}_weight_zp",
            ],
            [conv2d_output_name],
            attr,
        )

        # For quantized convolution, rescale the output value back to the same
        # integer value domain of the next op. Otherwise return float32 output.
        if inputs[0].dtype == ts.DType.INT8:
            # Get scale_factor from input, weight, and output.
            input_scale = input_qparams[0].get_scale_per_tensor()  # type: ignore[possibly-undefined]  # pyre-ignore [61]
            per_channel_quant = input_qparams[1].per_channel  # pyre-ignore [61]
            if per_channel_quant:
                weight_scale = input_qparams[1].get_scale_per_channel()
            else:
                weight_scale = [
                    input_qparams[1].get_scale_per_tensor()
                ]  # pyre-ignore [61]
            output_qargs = get_output_qparams(node)
            post_conv2d_scale = [
                (inp * w) / out
                for inp, w, out in zip(
                    itertools.cycle([input_scale]),
                    weight_scale,
                    itertools.cycle([output_qargs[0].get_scale_per_tensor()]),
                )
            ]
            build_rescale(
                tosa_fb=tosa_graph,
                scale=post_conv2d_scale,
                input_node=conv2d_res,  # type: ignore[possibly-undefined]
                output_name=output.name,
                output_type=output.dtype,
                input_zp=[0],
                output_zp=[output_qargs[0].get_zp_per_tensor()],
                per_channel=per_channel_quant,
                rounding_mode=RoundingMode.SINGLE_ROUND,
            )
