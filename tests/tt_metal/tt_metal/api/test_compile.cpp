// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <stdexcept>
#include <tt-metalium/core_coord.hpp>
#include <tt-metalium/kernel_types.hpp>
#include <tt-metalium/program.hpp>
#include <tt-metalium/tt_metal.hpp>
#include "device_fixture.hpp"

namespace tt::tt_metal {
    TEST_F(DeviceFixture, CreateKernelRejectsNullCharInDefineValue) {
        Program program = CreateProgram();
        DataMovementConfig config{.defines = {{
            {"UPSTREAM_NOC_X", std::to_string(phys_spoof_prefetch_core.x)},
            {"UPSTREAM_NOC_Y", std::to_string(phys_spoof_prefetch_core.y)},
            {"MY_NOC_X", std::to_string(phys_dispatch_core.x)},
            {"MY_NOC_Y", std::to_string(phys_dispatch_core.y)},
            {"DOWNSTREAM_NOC_X", {0}}, // oops, should be a string
            {"DOWNSTREAM_NOC_Y", {0}}, // oops, should be a string
        }}};
        
        EXPECT_THROW(
            CreateKernel(program, "dummy.cpp", CoreCoord{0, 0}, config),
            std::invalid_argument);
    }
}