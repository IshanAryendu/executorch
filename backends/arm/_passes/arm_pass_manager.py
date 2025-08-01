# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
# Copyright 2024-2025 Arm Limited and/or its affiliates.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import executorch.backends.arm.tosa.dialect  # noqa: unused
from executorch.backends.arm._passes import (
    AddBiasPass,
    AnnotateChannelsLastDimOrder,
    AnnotateDecomposedMatmulPass,
    BroadcastArgsPass,
    CastBoolToInt8Pass,
    CastInt64BuffersToInt32Pass,
    CastToInt32Pass,
    ComputeConstantOpsAOT,
    Conv1dUnsqueezePass,
    ConvertAnyDefaultDimDimsPass,
    ConvertExpandCopyToRepeatPass,
    ConvertFullLikeToFullPass,
    ConvertIntPowToMuls,
    ConvertMinMaxPass,
    ConvertMmToBmmPass,
    ConvertSplitToSlicePass,
    ConvertSqueezesToViewPass,
    ConvertToClampPass,
    DecomposeAcoshPass,
    DecomposeAdaptiveAvgPool2dPass,
    DecomposeAsinPass,
    DecomposeAtanhPass,
    DecomposeAtanPass,
    DecomposeAvgPool2d,
    DecomposeBatchNormNoStatsPass,
    DecomposeCosineSimilarityPass,
    DecomposeDivPass,
    DecomposeEmbeddingPass,
    DecomposeGeluPass,
    DecomposeGroupedConv,
    DecomposeGroupNormPass,
    DecomposeLayerNormPass,
    DecomposeLeakyReLUPass,
    DecomposeLinearPass,
    DecomposeLinearVectorNormPass,
    DecomposeMaxPool2DPass,
    DecomposeMeanDimPass,
    DecomposeNotEqualPass,
    DecomposeRoundPass,
    DecomposeSelectPass,
    DecomposeSignPass,
    DecomposeSiluPass,
    DecomposeSinhPass,
    DecomposeSoftmaxPass,
    DecomposeSoftmaxUnstablePass,
    DecomposeSqrtPass,
    DecomposeSumPass,
    DecomposeVarPass,
    DecorateFp32toInt32CastingPass,
    FoldAndAnnotateQParamsPass,
    FuseBatchnorm2DPass,
    FuseConstantArgsPass,
    FuseEqualPlaceholdersPass,
    FuseQuantizedActivationPass,
    InsertCastForOpsWithInt64InputPass,
    InsertRescalePass,
    InsertTableOpsPass,
    MatchArgDtypePass,
    MatchArgRanksPass,
    QuantizeOperatorArguments,
    RemoveClonePass,
    ReplaceInfValues,
    ReplaceScalarWithTensorArgPassTOSABI,
    ReplaceScalarWithTensorArgPassTOSAMI,
    RetraceFoldedDtypesPass,
    ScalarsToAttributePass,
    SizeAdjustInputPass,
    UnsqueezeBeforeRepeatPass,
    UnsqueezeScalarPlaceholdersPass,
)
from executorch.backends.arm.tosa_specification import (
    TosaLoweringContext,
    TosaSpecification,
)
from executorch.backends.transforms.decompose_sdpa import (
    DecomposeScaledDotProductAttention,
)
from executorch.backends.transforms.fuse_view_copy import FuseViewCopyTransform
from executorch.backends.transforms.remove_getitem_op import RemoveGetItemPass
from executorch.exir import ExportedProgram
from executorch.exir.pass_manager import PassManager
from torch.fx import GraphModule


class ArmPassManager(PassManager):

    def __init__(self, tosa_spec: TosaSpecification) -> None:
        self.tosa_spec = tosa_spec
        super().__init__()

    def _transform(self, graph_module: GraphModule):
        with TosaLoweringContext(self.tosa_spec):
            return self(graph_module).graph_module

    def _tosa_080_BI_pipeline(self, exported_program: ExportedProgram) -> GraphModule:
        self.add_pass(FuseQuantizedActivationPass())
        self.add_pass(RemoveGetItemPass())
        self.add_pass(ConvertSplitToSlicePass())
        self.add_pass(ConvertMmToBmmPass())
        self.add_pass(DecomposeLinearVectorNormPass())
        self.add_pass(
            DecomposeMeanDimPass(exported_program.graph_module, self.tosa_spec)
        )
        self.add_pass(ConvertFullLikeToFullPass())
        self.add_pass(ConvertToClampPass())
        self.add_pass(ConvertMinMaxPass())
        self.add_pass(ConvertAnyDefaultDimDimsPass())
        self.add_pass(MatchArgDtypePass())
        if self.tosa_spec.is_U55_subset:
            self.add_pass(CastToInt32Pass())

        self.add_pass(CastBoolToInt8Pass())
        self.add_pass(ReplaceScalarWithTensorArgPassTOSABI())
        self.add_pass(AnnotateDecomposedMatmulPass())
        self.add_pass(QuantizeOperatorArguments())
        self.add_pass(FoldAndAnnotateQParamsPass(exported_program))  # type: ignore[call-arg]
        self.add_pass(RetraceFoldedDtypesPass())
        self.add_pass(UnsqueezeScalarPlaceholdersPass(exported_program))
        self.add_pass(MatchArgRanksPass(exported_program))
        if self.tosa_spec.is_U55_subset:
            self.add_pass(BroadcastArgsPass())
        self.add_pass(DecomposeLinearPass())
        self.add_pass(DecomposeAdaptiveAvgPool2dPass())
        self.add_pass(DecomposeAvgPool2d())
        self.add_pass(ComputeConstantOpsAOT(exported_program))

        self.add_pass(DecomposeGroupedConv())
        self.add_pass(RemoveClonePass())
        self.add_pass(ConvertExpandCopyToRepeatPass())
        self.add_pass(UnsqueezeBeforeRepeatPass())
        self.add_pass(CastInt64BuffersToInt32Pass(exported_program))
        self.add_pass(DecomposeSumPass())
        self.add_pass(Conv1dUnsqueezePass())
        self.add_pass(DecomposeMaxPool2DPass())
        self.add_pass(SizeAdjustInputPass())
        self.add_pass(DecomposeSelectPass())
        self.add_pass(ConvertSqueezesToViewPass())

        self.add_pass(FuseViewCopyTransform())
        self.add_pass(FuseConstantArgsPass(exported_program))
        self.add_pass(AddBiasPass(exported_program))

        self.add_pass(InsertTableOpsPass(exported_program))
        self.add_pass(FuseEqualPlaceholdersPass(exported_program))
        self.add_pass(AnnotateChannelsLastDimOrder())
        self.add_pass(InsertRescalePass())

        return self._transform(exported_program.graph_module)

    def _tosa_080_MI_pipeline(self, exported_program: ExportedProgram) -> GraphModule:
        self.add_pass(DecomposeRoundPass())
        self.add_pass(DecomposeAcoshPass())
        self.add_pass(DecomposeAsinPass())
        self.add_pass(DecomposeSqrtPass())
        self.add_pass(DecomposeAtanPass())
        self.add_pass(DecomposeAtanhPass())
        self.add_pass(ConvertIntPowToMuls())
        self.add_pass(CastBoolToInt8Pass())
        self.add_pass(DecomposeSinhPass())
        self.add_pass(DecomposeSignPass())
        self.add_pass(ReplaceScalarWithTensorArgPassTOSAMI())
        self.add_pass(DecomposeEmbeddingPass())
        self.add_pass(FuseQuantizedActivationPass())
        self.add_pass(RemoveGetItemPass())
        self.add_pass(ConvertSplitToSlicePass())
        self.add_pass(FuseBatchnorm2DPass(exported_program))
        self.add_pass(ConvertMmToBmmPass())
        self.add_pass(DecomposeLinearPass())
        self.add_pass(DecomposeLeakyReLUPass())
        self.add_pass(DecomposeGroupNormPass())
        self.add_pass(DecomposeLayerNormPass())
        self.add_pass(DecomposeBatchNormNoStatsPass())
        self.add_pass(DecomposeVarPass())
        self.add_pass(
            DecomposeMeanDimPass(exported_program.graph_module, self.tosa_spec)
        )
        self.add_pass(DecomposeNotEqualPass())
        self.add_pass(DecomposeDivPass())
        self.add_pass(DecomposeSoftmaxPass())
        self.add_pass(DecomposeGeluPass())
        self.add_pass(ConvertFullLikeToFullPass())
        self.add_pass(ConvertToClampPass())
        self.add_pass(ConvertMinMaxPass())
        self.add_pass(ConvertAnyDefaultDimDimsPass())
        self.add_pass(MatchArgDtypePass())
        self.add_pass(AnnotateDecomposedMatmulPass())
        self.add_pass(QuantizeOperatorArguments())
        self.add_pass(FoldAndAnnotateQParamsPass(exported_program))  # type: ignore[call-arg]
        self.add_pass(RetraceFoldedDtypesPass())
        self.add_pass(UnsqueezeScalarPlaceholdersPass(exported_program))
        self.add_pass(MatchArgRanksPass(exported_program))
        self.add_pass(DecomposeAdaptiveAvgPool2dPass())
        self.add_pass(DecomposeAvgPool2d())
        self.add_pass(
            DecorateFp32toInt32CastingPass()
        )  # Require that no new fp32->int32 is introduced after this pass
        self.add_pass(ComputeConstantOpsAOT(exported_program))

        self.add_pass(DecomposeGroupedConv())
        self.add_pass(RemoveClonePass())
        self.add_pass(ConvertExpandCopyToRepeatPass())
        self.add_pass(UnsqueezeBeforeRepeatPass())
        self.add_pass(CastInt64BuffersToInt32Pass(exported_program))
        self.add_pass(DecomposeSumPass())
        self.add_pass(Conv1dUnsqueezePass())
        self.add_pass(DecomposeMaxPool2DPass())
        self.add_pass(SizeAdjustInputPass())
        self.add_pass(DecomposeSelectPass())
        self.add_pass(ConvertSqueezesToViewPass())

        self.add_pass(FuseViewCopyTransform())
        self.add_pass(FuseConstantArgsPass(exported_program))
        self.add_pass(AddBiasPass(exported_program))
        self.add_pass(InsertTableOpsPass(exported_program))
        self.add_pass(FuseEqualPlaceholdersPass(exported_program))
        self.add_pass(AnnotateChannelsLastDimOrder())
        self.add_pass(InsertRescalePass())

        return self._transform(exported_program.graph_module)

    def _tosa_1_0_int_quantized_pipeline(self, exported_program: ExportedProgram):
        return self._tosa_080_BI_pipeline(exported_program)

    def _tosa_1_0_fp_pipeline(self, exported_program: ExportedProgram):
        return self._tosa_080_MI_pipeline(exported_program)

    def transform_to_backend_pipeline(self, exported_program: ExportedProgram):
        """Apply passes before transforming program to backend"""
        if self.tosa_spec == TosaSpecification.create_from_string("TOSA-0.80.0+BI"):
            return self._tosa_080_BI_pipeline(exported_program)
        elif self.tosa_spec == TosaSpecification.create_from_string("TOSA-0.80.0+MI"):
            return self._tosa_080_MI_pipeline(exported_program)
        elif self.tosa_spec == TosaSpecification.create_from_string("TOSA-1.0+FP"):
            return self._tosa_1_0_fp_pipeline(exported_program)
        elif self.tosa_spec == TosaSpecification.create_from_string("TOSA-1.0+INT"):
            return self._tosa_1_0_int_quantized_pipeline(exported_program)
        else:
            raise NotImplementedError(
                f"No pass pipeline implemented for {self.tosa_spec=}"
            )

    def transform_for_annotation_pipeline(self, graph_module: GraphModule):
        self.add_pass(InsertCastForOpsWithInt64InputPass())
        self.add_pass(DecomposeEmbeddingPass())
        self.add_pass(DecomposeScaledDotProductAttention())
        self.add_pass(DecomposeRoundPass())
        self.add_pass(CastBoolToInt8Pass())
        self.add_pass(DecomposeSignPass())
        self.add_pass(ReplaceScalarWithTensorArgPassTOSABI())
        self.add_pass(ScalarsToAttributePass())
        self.add_pass(DecomposeGroupNormPass())
        self.add_pass(DecomposeLayerNormPass())
        self.add_pass(DecomposeVarPass())
        self.add_pass(DecomposeMeanDimPass(graph_module, self.tosa_spec))
        self.add_pass(DecomposeNotEqualPass())
        self.add_pass(DecomposeCosineSimilarityPass())
        self.add_pass(DecomposeDivPass())
        self.add_pass(DecomposeLeakyReLUPass())
        self.add_pass(DecomposeLinearVectorNormPass())
        self.add_pass(DecomposeSqrtPass())
        self.add_pass(DecomposeSiluPass())
        self.add_pass(DecomposeAvgPool2d())

        if self.tosa_spec.is_U55_subset:
            # Numerically stable softmax uses amax which is not supported on Ethos-U55
            self.add_pass(DecomposeSoftmaxUnstablePass())
        else:
            self.add_pass(DecomposeSoftmaxPass())

        self.add_pass(ConvertMinMaxPass())
        self.add_pass(ReplaceInfValues())
        self.add_pass(DecomposeSumPass())

        return self._transform(graph_module)
