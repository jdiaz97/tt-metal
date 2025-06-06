// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <boost/move/utility_core.hpp>
#include <fmt/base.h>
#include <gtest/gtest.h>
#include <stdint.h>
#include <tt-metalium/bfloat16.hpp>
#include <tt-metalium/constants.hpp>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <tt-metalium/assert.hpp>
#include <tt-metalium/buffer.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/logger.hpp>
#include <tt-metalium/shape.hpp>
#include "tests/tt_metal/tt_metal/common/dispatch_fixture.hpp"
#include "ttnn/cpp/ttnn/operations/creation.hpp"
#include "ttnn/cpp/ttnn/operations/experimental/reshape/view.hpp"
#include "ttnn/decorators.hpp"
#include "ttnn/operations/eltwise/binary/binary.hpp"
#include "ttnn/operations/eltwise/unary/unary.hpp"
#include "ttnn/tensor/enum_types.hpp"
#include "ttnn/tensor/host_buffer/owned_buffer.hpp"
#include "ttnn/tensor/shape/shape.hpp"
#include "ttnn/tensor/storage.hpp"
#include "ttnn/tensor/tensor.hpp"
#include "ttnn/tensor/types.hpp"

namespace tt::tt_metal {
namespace {

using ::tt::constants::TILE_HEIGHT;
using ::tt::constants::TILE_WIDTH;

uint32_t get_device_buffer_address(const Tensor& tensor) {
    TT_FATAL(std::holds_alternative<DeviceStorage>(tensor.get_storage()), "Tensor storage is not DeviceStorage");
    auto buffer = std::get<DeviceStorage>(tensor.get_storage()).buffer;
    uint32_t result = 0;
    buffer->device()->push_work([&]() { result = buffer->address(); }, true);
    return result;
}

TEST_F(DispatchFixture, TestTensorOwnershipSanity) {
    // Sanity test tensor read, write and update paths with synchronous
    // Ensure that tensor data is copied and owned as expected
    IDevice* device = this->devices_[0];
    Tensor host_tensor = ttnn::arange(/*start=*/0, /*stop=*/32 * 32 * 4, /*step=*/1, DataType::FLOAT32);
    Tensor readback_tensor(1);

    auto func = [device, host_tensor, readback_tensor]() mutable {
        // Ensure that both the lambda and global scope have ownership to this tensor
        EXPECT_EQ(host_tensor.tensor_attributes.use_count(), 2);
        std::visit(
            [](auto&& storage) {
                using T = std::decay_t<decltype(storage)>;
                if constexpr (std::is_same_v<T, OwnedStorage>) {
                    std::visit(
                        [](auto&& buf) {
                            using buf_type = std::decay_t<decltype(buf)>;
                            if constexpr (std::is_same_v<buf_type, owned_buffer::Buffer<float>>) {
                                EXPECT_EQ(buf.use_count(), 1);
                            }
                        },
                        storage.buffer);
                }
            },
            host_tensor.get_storage());
        // Send tensor to device, read it back and copy it to empty tensor initialized by main thread
        Tensor reshaped_tensor = ttnn::experimental::view(host_tensor, ttnn::Shape{1, 1, 32, 128});
        auto device_tensor = reshaped_tensor.to_layout(Layout::TILE).to_device(device);
        auto thread_local_tensor = device_tensor.cpu().to_layout(Layout::ROW_MAJOR);
        readback_tensor.set_storage(thread_local_tensor.get_storage());
        readback_tensor.set_tensor_spec(thread_local_tensor.get_tensor_spec());
        // Ensure that the readback buffer is owned inside and outside the lambda
        std::visit(
            [](auto&& storage) {
                using T = std::decay_t<decltype(storage)>;
                if constexpr (std::is_same_v<T, OwnedStorage>) {
                    std::visit(
                        [](auto&& buf) {
                            using buf_type = std::decay_t<decltype(buf)>;
                            if constexpr (std::is_same_v<buf_type, owned_buffer::Buffer<float>>) {
                                EXPECT_EQ(buf.use_count(), 2);
                            }
                        },
                        storage.buffer);
                }
            },
            readback_tensor.get_storage());
    };

    func();
    std::visit(
        [](auto&& storage) {
            using T = std::decay_t<decltype(storage)>;
            if constexpr (std::is_same_v<T, OwnedStorage>) {
                std::visit(
                    [](auto&& buf) {
                        using buf_type = std::decay_t<decltype(buf)>;
                        if constexpr (std::is_same_v<buf_type, owned_buffer::Buffer<float>>) {
                            EXPECT_EQ(buf.use_count(), 1);
                            for (int i = 0; i < 128 * 32; i++) {
                                EXPECT_EQ(buf[i], i);
                            }
                        }
                    },
                    storage.buffer);
            }
        },
        readback_tensor.get_storage());
    EXPECT_EQ(readback_tensor.get_dtype(), DataType::FLOAT32);
    EXPECT_EQ(readback_tensor.get_layout(), Layout::ROW_MAJOR);
    EXPECT_EQ(readback_tensor.get_logical_shape(), Shape({1, 1, 32, 128}));
}

TEST_F(DispatchFixture, TestAsyncEltwiseBinary) {
    IDevice* device = this->devices_[0];
    // Populate these in first loop and verify that deallocation worked - addresses should be identical across loops
    std::size_t input_a_addr = 0;
    std::size_t input_b_addr = 0;
    std::size_t input_c_addr = 0;
    std::size_t output_1_addr = 0;
    std::size_t output_2_addr = 0;

    for (int i = 0; i < 5; i++) {
        // Initialize tensors and move them to DRAM
        Tensor input_tensor_a = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}), static_cast<float>(i), DataType::BFLOAT16, Layout::TILE, *device);
        Tensor input_tensor_b = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}), static_cast<float>(i), DataType::BFLOAT16, Layout::TILE, *device);
        Tensor input_tensor_c = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}), static_cast<float>(i), DataType::BFLOAT16, Layout::TILE, *device);
        Tensor output_tensor_device = ttnn::multiply(ttnn::add(input_tensor_a, input_tensor_b), input_tensor_c);
        Tensor output_tensor_device_2 = ttnn::neg(ttnn::subtract(output_tensor_device, input_tensor_c));

        EXPECT_EQ(output_tensor_device.get_logical_shape(), Shape({1, 1, 1024, 1024}));
        EXPECT_EQ(output_tensor_device.get_dtype(), DataType::BFLOAT16);

        Tensor output_tensor_host = output_tensor_device_2.cpu();
        // Test tensor deallocation in async mode: deallocate tensors after using them
        if (i == 0) {
            input_a_addr = get_device_buffer_address(input_tensor_a);
            input_b_addr = get_device_buffer_address(input_tensor_b);
            input_c_addr = get_device_buffer_address(input_tensor_c);
            output_1_addr = get_device_buffer_address(output_tensor_device);
            output_2_addr = get_device_buffer_address(output_tensor_device_2);
        } else {
            EXPECT_EQ(get_device_buffer_address(input_tensor_a), input_a_addr);
            EXPECT_EQ(get_device_buffer_address(input_tensor_b), input_b_addr);
            EXPECT_EQ(get_device_buffer_address(input_tensor_c), input_c_addr);
            EXPECT_EQ(get_device_buffer_address(output_tensor_device), output_1_addr);
            EXPECT_EQ(get_device_buffer_address(output_tensor_device_2), output_2_addr);
        }
        input_tensor_a.deallocate();
        input_tensor_b.deallocate();
        input_tensor_c.deallocate();
        output_tensor_device.deallocate();
        output_tensor_device_2.deallocate();
        // Verify output data
        auto& buf =
            std::get<owned_buffer::Buffer<bfloat16>>(std::get<OwnedStorage>(output_tensor_host.get_storage()).buffer);
        EXPECT_EQ(buf.use_count(), 1);
        for (int j = 0; j < 1024 * 1024; j++) {
            EXPECT_EQ(bfloat16(buf[j]), bfloat16(static_cast<float>(i - 2 * i * i)));
        }
    }
}

Tensor tensor_identity_copy_function(const Tensor& tensor) { return tensor; }

TEST_F(DispatchFixture, TestAsyncRefCountManager) {
    IDevice* device = this->devices_[0];

    log_info(LogTest, "Testing Device tensor copy assignment");
    for (int i = 0; i < 5; i++) {
        // Run for multiple loops to ensure deterministic behaviour with device addresses
        // Initialize 2 tensors on device
        Tensor tensor1 = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}),
            static_cast<float>(i),
            DataType::BFLOAT16,
            /*layout=*/std::nullopt,
            *device);
        Tensor tensor2 = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}),
            static_cast<float>(i),
            DataType::BFLOAT16,
            /*layout=*/std::nullopt,
            *device);
        uint32_t tensor2_device_buf_addr = get_device_buffer_address(tensor2);
        tensor2 = tensor1;
        // To check if tensor2 is deallocated, create a third tensor on device and ensure that its address matches the
        // prev addr for tensor2
        Tensor tensor3 = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}),
            static_cast<float>(i),
            DataType::BFLOAT16,
            /*layout=*/std::nullopt,
            *device);
        EXPECT_EQ(get_device_buffer_address(tensor3), tensor2_device_buf_addr);
        EXPECT_EQ(get_device_buffer_address(tensor1), get_device_buffer_address(tensor2));
    }
    log_info(LogTest, "Testing Device tensor self-assignment through function");
    for (int i = 0; i < 5; i++) {
        Tensor device_tensor = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}),
            static_cast<float>(i),
            DataType::BFLOAT16,
            /*layout=*/std::nullopt,
            *device);
        uint32_t device_tensor_address = get_device_buffer_address(device_tensor);
        // This step will copy the tensor to a temp rval and std::move it back to the caller's instance of device_tensor
        // Ensure ref count and address remain unchanged
        device_tensor = tensor_identity_copy_function(device_tensor);
        EXPECT_EQ(get_device_buffer_address(device_tensor), device_tensor_address);
    }

    log_info(LogTest, "Testing Device tensor move assignment");
    for (int i = 0; i < 5; i++) {
        Tensor tensor1 = ttnn::full(
            ttnn::Shape({1, 1, 1024, 1024}),
            static_cast<float>(i),
            DataType::BFLOAT16,
            /*layout=*/std::nullopt,
            *device);
        Tensor tensor2 = std::move(tensor1);
    }

    log_info(LogTest, "Testing Device tensor self-assignment");
    Tensor tensor_to_self_assign = ttnn::full(
        ttnn::Shape({1, 1, 1024, 1024}),
        static_cast<float>(0),
        DataType::BFLOAT16,
        /*layout=*/std::nullopt,
        *device);
    uint32_t tensor_to_self_assign_address = get_device_buffer_address(tensor_to_self_assign);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
#pragma clang diagnostic ignored "-Wself-move"
#endif
    tensor_to_self_assign = tensor_to_self_assign;
    tensor_to_self_assign = std::move(tensor_to_self_assign);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    EXPECT_EQ(get_device_buffer_address(tensor_to_self_assign), tensor_to_self_assign_address);
    auto barrier_tensor = tensor_to_self_assign.cpu();
}

TEST_F(DispatchFixture, TestTensorAsyncDataMovement) {
    // Test 2 data paths here (resembles async mode):
    // 1. Main -> Worker: Create a tensor in the main thread. Ensure that it is accessible in the worker thread even
    // after its destroyed
    //                    by the main thread. This resembles host -> device data movement
    // 2. Worker -> Main: Create an empty tensor in the mainb thread. Populate it in the worker thread. Ensure that the
    // tensor is correctly
    //                    populated in the main thread once the worker is done.
    IDevice* device = this->devices_[0];
    uint32_t tensor_start = 0;
    uint32_t num_tiles = 128;
    uint32_t tensor_stop = TILE_HEIGHT * TILE_WIDTH * num_tiles;
    Tensor readback_tensor(1);
    std::thread worker;

    {
        // host_tensor only lives in this scope
        Tensor host_tensor = ttnn::arange(tensor_start, tensor_stop, /*step=*/1, DataType::FLOAT32);
        log_info(LogTest, "Spawning worker thread");
        worker = std::thread([tensor_stop, host_tensor, readback_tensor, device]() mutable {
            // Sleep for 3 seconds to ensure that main thread deallocates host_tensor
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            log_info(LogTest, "Worker started");
            // Main thread should have deallocated host_tensor by this point
            EXPECT_EQ(host_tensor.tensor_attributes.use_count(), 1);
            // Ensure that the buffer inside host_buffer is owned by a single tensor_attr object
            // This buffer will not go out of scope until the last object owning it is destroyed (i.e. until the thread
            // is done)
            std::visit(
                [](auto&& storage) {
                    using T = std::decay_t<decltype(storage)>;
                    if constexpr (std::is_same_v<T, OwnedStorage>) {
                        std::visit(
                            [](auto&& buf) {
                                using buf_type = std::decay_t<decltype(buf)>;
                                if constexpr (std::is_same_v<buf_type, owned_buffer::Buffer<float>>) {
                                    EXPECT_EQ(buf.use_count(), 1);
                                }
                            },
                            storage.buffer);
                    }
                },
                host_tensor.get_storage());

            Tensor reshaped_tensor = ttnn::experimental::view(host_tensor, ttnn::Shape{1, 1, 32, tensor_stop / 32});
            auto device_tensor = reshaped_tensor.to_layout(Layout::TILE).to_device(device);
            auto thread_local_tensor = device_tensor.cpu().to_layout(Layout::ROW_MAJOR);
            log_info(LogTest, "Worker populating empty host readback_tensor");
            readback_tensor.set_storage(thread_local_tensor.get_storage());
            readback_tensor.set_tensor_spec(thread_local_tensor.get_tensor_spec());
            // Ensure that this buffer is currently owned by both the thread_local and read_back tensors
            // This is because we explictly pass in the buffer to a new tensor_attr object
            std::visit(
                [](auto&& storage) {
                    using T = std::decay_t<decltype(storage)>;
                    if constexpr (std::is_same_v<T, OwnedStorage>) {
                        std::visit(
                            [](auto&& buf) {
                                using buf_type = std::decay_t<decltype(buf)>;
                                if constexpr (std::is_same_v<buf_type, owned_buffer::Buffer<float>>) {
                                    EXPECT_EQ(buf.use_count(), 2);
                                }
                            },
                            storage.buffer);
                    }
                },
                readback_tensor.get_storage());
            log_info(LogTest, "Worker Done");
        });
        // Call deallocate on the tensor in the main thread to ensure that this call is safe
        // i.e.: the tensor should not be deallocated until the thread is done with it
        log_info(LogTest, "Main thread calling deallocate on tensor passed to worker");
        host_tensor.deallocate();
    }
    worker.join();
    log_info(LogTest, "Verifying populated tensor in main thread");
    std::visit(
        [tensor_start, tensor_stop](auto&& storage) {
            using T = std::decay_t<decltype(storage)>;
            if constexpr (std::is_same_v<T, OwnedStorage>) {
                std::visit(
                    [tensor_start, tensor_stop](auto&& buf) {
                        using buf_type = std::decay_t<decltype(buf)>;
                        if constexpr (std::is_same_v<buf_type, owned_buffer::Buffer<float>>) {
                            EXPECT_EQ(buf.use_count(), 1);
                            for (int i = tensor_start; i < tensor_stop; i++) {
                                EXPECT_EQ(buf[i], i);
                            }
                        }
                    },
                    storage.buffer);
            }
        },
        readback_tensor.get_storage());
    EXPECT_EQ(readback_tensor.get_dtype(), DataType::FLOAT32);
    EXPECT_EQ(readback_tensor.get_layout(), Layout::ROW_MAJOR);
    EXPECT_EQ(readback_tensor.get_logical_shape(), Shape({1, 1, 32, tensor_stop / 32}));
}
}  // namespace
}  // namespace tt::tt_metal
