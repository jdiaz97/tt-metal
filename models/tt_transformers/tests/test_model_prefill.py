# SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0
import bz2
import torch
import pytest
from loguru import logger
import os
import ttnn
from models.tt_transformers.tt.common import (
    get_prefill_rot_mat,
    PagedAttentionConfig,
)
from models.tt_transformers.tt.model import Transformer
from models.tt_transformers.tt.model_config import ModelArgs, DecodersPrecision
from models.utility_functions import (
    comp_pcc,
    comp_allclose,
)
from models.utility_functions import skip_for_grayskull


@torch.no_grad()
@skip_for_grayskull("Requires wormhole_b0 to run")
@pytest.mark.timeout(900)
@pytest.mark.models_performance_bare_metal
@pytest.mark.parametrize(
    "mesh_device",
    [
        {"N150": (1, 1), "N300": (1, 2), "T3K": (1, 8), "TG": (8, 4)}.get(
            os.environ.get("MESH_DEVICE"), len(ttnn.get_device_ids())
        )
    ],
    indirect=True,
)
# Model and attention prefill tests should run both with and without paged attention to debug any issues that may occur with default attention
@pytest.mark.parametrize(
    "paged_attention",
    (
        True,
        # False,
    ),
    ids=(
        "paged_attention",
        # "default_attention",
    ),
)
@pytest.mark.parametrize(
    "page_params",
    [{"page_block_size": 32, "page_max_num_blocks": 1024}],
)
@pytest.mark.parametrize(
    "seq_len",
    (4096,),
)
@pytest.mark.parametrize(
    "optimizations",
    [
        lambda model_args: DecodersPrecision.performance(model_args.n_layers, model_args.model_name),
        lambda model_args: DecodersPrecision.accuracy(model_args.n_layers, model_args.model_name),
    ],
    ids=["performance", "accuracy"],
)
def test_model_inference(
    seq_len,
    paged_attention,
    page_params,
    optimizations,
    mesh_device,
    use_program_cache,
    reset_seeds,
    ensure_gc,
    is_ci_env,
    request,
):
    test_id = request.node.callspec.id
    if is_ci_env and "accuracy" in test_id:
        pytest.skip("CI test only runs performance mode to reduce CI pipeline load")

    run_ref_pt = True  # Flag to run reference PyTorch model and compare PCC
    cache_pcc = False  # Flag to measure KV cache PCC for all layers

    dtype = ttnn.bfloat8_b
    batch_size = 1  # For prefill we only support batch_size = 1

    # This sets the minimum PCC for each iteration based on optimization mode
    if "accuracy" in test_id:
        pcc = 0.91  # TODO Look on improving PCC
    else:  # performance mode
        assert "performance" in test_id
        pcc = 0.869  # TODO Look on improving PCC

    # Use instruct weights instead of general weights
    instruct = True

    model_args = ModelArgs(mesh_device, max_batch_size=batch_size, optimizations=optimizations, max_seq_len=seq_len)
    tokenizer = model_args.tokenizer

    logger.info("Loading weights...")
    state_dict_prefix = model_args.get_state_dict_prefix("", None)
    state_dict = model_args.load_state_dict()
    reference_state_dict = {
        k[len(state_dict_prefix) :]: v
        for k, v in state_dict.items()
        if (
            any([f"{state_dict_prefix}layers.{i}." in k for i in range(model_args.n_layers)])
            or any(
                [
                    f"{state_dict_prefix}{name}" in k
                    for name in ["tok_embeddings.weight", "norm.weight", "output.weight"]
                ]
            )
        )
    }
    logger.info("Finished loading weights...")

    current_file_path = os.path.abspath(__file__)
    current_file_dir = os.path.dirname(current_file_path)
    prompt_file = os.path.join(current_file_dir, "tale-of-two-cities.txt.bz2")

    with bz2.open(prompt_file, "rt", encoding="utf-8") as f:
        prompt = f.read()

    encoded_prompt = model_args.encode_prompt(prompt, instruct=instruct)[:seq_len]

    if run_ref_pt:
        reference_model = model_args.reference_transformer()
        reference_model.load_state_dict(reference_state_dict)

    # Embedding on host
    embd = model_args.reference_embedding()
    embd.load_state_dict({"emb.weight": state_dict[f"{state_dict_prefix}tok_embeddings.weight"]})

    # pre-compute the rotational embedding matrix and send to device
    rot_mats = get_prefill_rot_mat(
        model_args.head_dim,
        mesh_device,
        seq_len,
        model_args.rope_theta,
        model_args.rope_scaling_factor,
        model_args.orig_context_len,
    )
    # Setup page table
    page_table_tt = None
    paged_attention_config = None

    if paged_attention:
        paged_attention_config = PagedAttentionConfig(
            block_size=page_params["page_block_size"],
            max_num_blocks=page_params["page_max_num_blocks"],
        )
        # Implied shuffling of blocks
        permutation = torch.randperm(paged_attention_config.max_num_blocks)
        # Page table which maps virtual blocks to physical
        reverse_permutation = torch.argsort(permutation)
        page_table = reverse_permutation.reshape(
            model_args.max_batch_size, paged_attention_config.max_num_blocks // model_args.max_batch_size
        )
        page_table_tt = ttnn.from_torch(
            page_table,
            device=mesh_device,
            dtype=ttnn.int32,
            layout=ttnn.ROW_MAJOR_LAYOUT,
            mesh_mapper=ttnn.ReplicateTensorToMesh(mesh_device),
        )

    # Load TTNN model
    tt_model = Transformer(
        args=model_args,
        mesh_device=mesh_device,
        dtype=dtype,
        state_dict=state_dict,
        weight_cache_path=model_args.weight_cache_path(dtype),
        paged_attention_config=paged_attention_config,
    )

    logger.info("Model and caches loaded.")

    if run_ref_pt:
        all_tests_pass = True

    # Select the first token from the prompt for initial decoding
    encoded_prompt_tensor = torch.tensor(encoded_prompt)  # [:,0]
    pt_prefill_input = embd(encoded_prompt_tensor).view(batch_size, seq_len, -1)

    tt_prefill_input = pt_prefill_input

    tt_prefill_input = model_args.prepare_residual_tensor_prefill(
        pt_prefill_input,
    )
    for i in range(1):
        start_pos = 0
        # Run TT model
        tt_out = tt_model(
            tt_prefill_input,
            current_pos=None,
            rot_mats=rot_mats,
            user_id=i,
            mode="prefill",
            page_table=page_table_tt,
        )
        # Convert ttnn tensor to torch tensor
        tt_out = ttnn.to_torch(
            tt_out,
            mesh_composer=ttnn.ConcatMesh2dToTensor(
                mesh_device, dims=(1, 3) if model_args.is_galaxy else (1, 3), mesh_shape=model_args.cluster_shape
            ),
        )
        tt_output_torch = tt_out[:, 0:1, :, : model_args.dim].view(batch_size, seq_len, -1)  # [ batch, seq, hidden_dim]

        if run_ref_pt:  # Run reference model
            ref_output = reference_model(pt_prefill_input, start_pos, mode="prefill")

        # Measure PCC if also running reference model
        if run_ref_pt:
            passing, pcc_message = comp_pcc(ref_output, tt_output_torch, pcc)

            logger.info(comp_allclose(ref_output, tt_output_torch))
            logger.info(f"PCC: {pcc_message}")

            if passing:
                logger.info("Model Passed!")
            else:
                logger.warning("Model Failed!")
            if not passing:
                all_tests_pass = False

            # Compare KV caches
            if cache_pcc:
                for i in range(model_args.n_layers):
                    pytorch_layer_present = [
                        reference_model.layers[i]
                        .attention.cache_k.clone()
                        .permute(0, 2, 1, 3),  # [batch_size, n_kv_heads, seq, head_dim]
                        reference_model.layers[i]
                        .attention.cache_v.clone()
                        .permute(0, 2, 1, 3),  # [batch_size, n_kv_heads, seq, head_dim]
                    ]

                    tt_layer_present = []
                    if paged_attention:
                        for layer_past in tt_model.layers[l].attention.layer_past:
                            tt_layer_present.append(
                                ttnn.to_torch(
                                    layer_past,
                                    mesh_composer=ttnn.ConcatMesh2dToTensor(
                                        mesh_device,
                                        dims=(1, 3) if model_args.is_galaxy else (0, 1),
                                        mesh_shape=model_args.cluster_shape,
                                    ),
                                )[reverse_permutation][:, : model_args.n_kv_heads, :, : model_args.head_dim]
                                .reshape(
                                    model_args.max_batch_size,
                                    paged_attention_config.max_num_blocks // model_args.max_batch_size,
                                    model_args.n_kv_heads,
                                    paged_attention_config.block_size,
                                    model_args.head_dim,
                                )
                                .transpose(1, 2)
                                .reshape(model_args.max_batch_size, model_args.n_kv_heads, -1, model_args.head_dim)[
                                    :batch_size, ...
                                ]
                            )
                        tt_layer_present = [
                            (
                                ttnn.to_torch(
                                    cache,
                                    mesh_composer=ttnn.ConcatMesh2dToTensor(
                                        mesh_device,
                                        dims=(1, 0) if model_args.is_galaxy else (0, 1),
                                        mesh_shape=model_args.cluster_shape,
                                    ),
                                )[reverse_permutation]
                                .reshape(
                                    model_args.max_batch_size,
                                    paged_attention_config.max_num_blocks // model_args.max_batch_size,
                                    model_args.n_kv_heads,
                                    paged_attention_config.block_size,
                                    model_args.head_dim,
                                )
                                .transpose(1, 2)
                                .reshape(model_args.max_batch_size, model_args.n_kv_heads, -1, model_args.head_dim)[
                                    :batch_size, ...
                                ]
                            )
                            for cache in tt_model.layers[l].attention.layer_past
                        ]
                    else:
                        for layer_past in tt_model.layers[i].attention.layer_past_list[0]:
                            tt_layer_present.append(
                                ttnn.to_torch(
                                    layer_past,
                                    mesh_composer=ttnn.ConcatMesh2dToTensor(
                                        mesh_device,
                                        dims=(1, 0) if model_args.is_galaxy else (0, 1),
                                        mesh_shape=model_args.cluster_shape,
                                    ),
                                )
                            )

                    for i, (cache_pt, cache_tt) in enumerate(zip(pytorch_layer_present, tt_layer_present)):
                        cache_length_to_check = model_args.max_seq_len
                        cache_pt = cache_pt[:, :, 0:cache_length_to_check, :]
                        cache_tt = cache_tt[:, :, 0:cache_length_to_check, :]
                        does_pass, output_pcc = comp_pcc(cache_pt, cache_tt)
                        if i == 0:
                            logger.info(f"K cache output: {output_pcc}")
                        else:
                            logger.info(f"V cache output: {output_pcc}")

                        if does_pass:
                            logger.info(f"V Cache Passed!")
                        else:
                            logger.warning(f"V Cache Failed! PCC value is lower than {0.99}")
                        # if not does_pass:
                        # all_tests_pass = False

    if run_ref_pt:
        if all_tests_pass:
            logger.info(f"All prefill iterations Passed!")
        else:
            logger.warning("One or more iterations of prefill had bad PCC")
            assert all_tests_pass, f"PCC value is lower than {pcc} for some of the outputs. Check Warnings!"
