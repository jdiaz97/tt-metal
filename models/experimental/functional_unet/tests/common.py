# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

from dataclasses import dataclass

import ttnn
from loguru import logger

from tests.ttnn.utils_for_testing import assert_with_pcc

UNET_FULL_MODEL_PCC = 0.99840
UNET_TRACE_REGION_SIZE = 483328


@dataclass
class UNetPerformanceStatistics:
    groups: int
    batch: int
    num_devices: int
    inference_and_compile_time: float
    inference_time: float

    def get_fps(self) -> float:
        return round(self.batch * self.groups * self.num_devices / self.inference_time, 4)


def is_n300_with_eth_dispatch_cores(mesh_device) -> bool:
    all_devices_using_full_grid = all(
        [(8 == device.core_grid.x and 8 == device.core_grid.y) for device in mesh_device.get_devices()]
    )
    return all_devices_using_full_grid and (len(mesh_device.get_devices()) == 2)


def is_t3k_with_eth_dispatch_cores(mesh_device) -> bool:
    all_devices_using_full_grid = all(
        [(8 == device.core_grid.x and 8 == device.core_grid.y) for device in mesh_device.get_devices()]
    )
    return all_devices_using_full_grid and (len(mesh_device.get_devices()) == 8)


def verify_with_pcc(torch_tensor, ttnn_tensor, pcc):
    _, computed_pcc = assert_with_pcc(torch_tensor, ttnn_tensor, pcc)
    logger.info(f"PCC check was successful ({computed_pcc:.6f} > {pcc:.6f})")
    if (computed_pcc - pcc) / pcc > 0.0025:
        logger.warning(
            f"Computed PCC ({computed_pcc:.6f}) was higher than the expected PCC ({pcc:.6f}) - consider updating the expected PCC value"
        )


# TODO: This is the same as the function below, we should consolidate them
def check_pcc_conv(torch_tensor, ttnn_tensor, pcc=0.999, mesh_composer=None):
    B, C, H, W = torch_tensor.shape
    ttnn_tensor = (
        ttnn.to_torch(ttnn_tensor, mesh_composer=mesh_composer).reshape(B, H, W, -1).permute(0, 3, 1, 2)[:, :C, :, :]
    )
    verify_with_pcc(torch_tensor, ttnn_tensor, pcc)


def check_pcc_pool(torch_tensor, ttnn_tensor, pcc=0.999, mesh_composer=None):
    B, C, H, W = torch_tensor.shape
    ttnn_tensor = (
        ttnn.to_torch(ttnn_tensor, mesh_composer=mesh_composer).reshape(B, H, W, -1).permute(0, 3, 1, 2)[:, :C, :, :]
    )
    verify_with_pcc(torch_tensor, ttnn_tensor, pcc)
