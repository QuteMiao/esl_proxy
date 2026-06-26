#!/usr/bin/env python3
# Copyright (c) PyPTO Contributors.
"""Qwen3-14B single-layer decode — dynamic tensormap variant.

Auto-scope orchestration (PyPTO IR compiler output) that reads user_batch
dynamically from ``orch_args.tensor(0).shapes[0]``. Uses 18 incore kernels
(see CALLABLE below): SPMD projections, per-batch attention, a MixedKernels
out_proj_residual group {10, 11, 11}, MLP gate/up/silu/down, and a single
AIV down_proj_residual writeback.

Golden validation uses a relaxed comparator; on mismatch the golden/actual
tensors are dumped as ``.pt`` under ``outputs/golden_mismatch/`` for offline
inspection. No external hooks or caching are used.
"""
from __future__ import annotations

import importlib
import math
import re
from datetime import datetime
from pathlib import Path

import torch
from simpler.task_interface import ArgDirection as D

from simpler_setup import SceneTestCase, TaskArgsBuilder, Tensor, scene_test

_SCENE_TEST_MOD = importlib.import_module(SceneTestCase.__module__)
_ROOT = Path(__file__).resolve().parent


def _safe_dir_label(name: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9._-]+", "_", name).strip("_") or "case"
    return s[:120]


BATCH = 90
MAX_SEQ = 4096
NUM_HEADS = 40
NUM_KV_HEADS = 8
HEAD_DIM = 128
HIDDEN = NUM_HEADS * HEAD_DIM
INTERMEDIATE = 17408
KV_HIDDEN = NUM_KV_HEADS * HEAD_DIM
BATCH_TILE = 16
BLOCK_SIZE = 128
EPS = 1e-6
Q_HEAD_BATCH = 5
INPUT_PROJ_K_CHUNK = 128
KV_PROJ_K_CHUNK = 128
Q_OUT_CHUNK = 256
KV_OUT_CHUNK = 128
K_CHUNK = 128
OUT_PROJ_K_CHUNK = 128
OUT_PROJ_N_CHUNK = 128
MLP_OUT_CHUNK = 512
DOWN_MLP_CHUNK = 128
DOWN_OUT_CHUNK = 128
SYNTHETIC_PROJ_SCALE = 0.5

def _max_blocks_per_seq() -> int:
    return (MAX_SEQ + BLOCK_SIZE - 1) // BLOCK_SIZE


def _cache_rows(batch: int) -> int:
    num_blocks = batch * _max_blocks_per_seq()
    return num_blocks * NUM_KV_HEADS * BLOCK_SIZE


GOLDEN_RELAXED_SOFT_FRAC = 1e-2
GOLDEN_RELAXED_SOFT_ABS_CAP = 0.008
GOLDEN_RELAXED_MARGINAL_FRAC = 1e-3
GOLDEN_RELAXED_MARGINAL_ABS_CAP = 0.016


def _golden_allclose_relaxed(
    actual: torch.Tensor,
    expected: torch.Tensor,
    rtol: float,
    atol: float,
    *,
    soft_frac: float = GOLDEN_RELAXED_SOFT_FRAC,
    soft_abs_cap: float = GOLDEN_RELAXED_SOFT_ABS_CAP,
    marginal_frac: float = GOLDEN_RELAXED_MARGINAL_FRAC,
    marginal_abs_cap: float = GOLDEN_RELAXED_MARGINAL_ABS_CAP,
) -> tuple[bool, str | None]:
    a = actual.detach().float().reshape(-1)
    e = expected.detach().float().reshape(-1)
    diff = (a - e).abs()
    n = diff.numel()
    if n == 0:
        return True, None

    thr = atol + rtol * e.abs()
    baseline_miss = diff > thr
    if not baseline_miss.any():
        return True, None

    severe = baseline_miss & (diff > marginal_abs_cap)
    if severe.any():
        return False, (
            f"max_abs_diff={diff.max().item():.6g} > marginal_abs_cap={marginal_abs_cap} "
            f"(rtol={rtol}, atol={atol})"
        )

    soft = baseline_miss & (diff <= soft_abs_cap)
    marginal = baseline_miss & (diff > soft_abs_cap) & (diff <= marginal_abs_cap)

    n_soft = int(soft.sum().item())
    n_marginal = int(marginal.sum().item())
    max_soft = int(math.floor(n * soft_frac))
    max_marginal = int(math.floor(n * marginal_frac))

    if n_soft > max_soft:
        return False, (
            f"{n_soft} / {n} elems in (thr,{soft_abs_cap}] exceed soft budget {max_soft}=floor(n*{soft_frac}); "
            f"rtol={rtol}, atol={atol}"
        )
    if n_marginal > max_marginal:
        return False, (
            f"{n_marginal} / {n} elems in ({soft_abs_cap},{marginal_abs_cap}] exceed marginal budget "
            f"{max_marginal}=floor(n*{marginal_frac}); rtol={rtol}, atol={atol}"
        )
    return True, None


def _emit_golden_mismatch_dumps(
    *,
    work_dir: Path,
    comparisons: list[tuple[str, torch.Tensor, torch.Tensor]],
) -> None:
    """Write golden/actual ``.pt`` for every compared output (for offline inspection)."""
    out_dir = work_dir / "data" / "out"
    act_dir = work_dir / "data" / "actual"
    out_dir.mkdir(parents=True, exist_ok=True)
    act_dir.mkdir(parents=True, exist_ok=True)

    for name, actual_cpu, golden_cpu in comparisons:
        torch.save(golden_cpu, out_dir / f"{name}.pt")
        torch.save(actual_cpu, act_dir / f"{name}.pt")


def _compute_golden(tensors: dict, params: dict | None = None) -> None:
    hidden_states = tensors["hidden_states"]
    input_rms_weight = tensors["input_rms_weight"]
    wq = tensors["wq"]
    wk = tensors["wk"]
    wv = tensors["wv"]
    q_norm_weight = tensors["q_norm_weight"]
    k_norm_weight = tensors["k_norm_weight"]
    seq_lens = tensors["seq_lens"]
    block_table = tensors["block_table"]
    slot_mapping = tensors["slot_mapping"]
    rope_cos = tensors["rope_cos"]
    rope_sin = tensors["rope_sin"]
    k_cache = tensors["k_cache"]
    v_cache = tensors["v_cache"]
    wo = tensors["wo"]
    post_rms_weight = tensors["post_rms_weight"]
    w_gate = tensors["w_gate"]
    w_up = tensors["w_up"]
    w_down = tensors["w_down"]

    batch = hidden_states.shape[0]
    hidden_size = hidden_states.shape[1]
    kv_hidden = wk.shape[1]
    head_dim = rope_cos.shape[1]
    max_seq = rope_cos.shape[0]
    num_kv_heads = kv_hidden // head_dim
    num_heads = hidden_size // head_dim
    q_per_kv = num_heads // num_kv_heads
    q_groups = q_per_kv // Q_HEAD_BATCH
    half = head_dim // 2
    scale = 1.0 / math.sqrt(head_dim)
    eps = 1e-6
    max_ctx_blocks = (max_seq + BLOCK_SIZE - 1) // BLOCK_SIZE

    def tiled_matmul(lhs, rhs, k_chunk, n_chunk):
        out = torch.zeros(lhs.shape[0], rhs.shape[1], dtype=torch.float32)
        for n0 in range(0, rhs.shape[1], n_chunk):
            acc = torch.zeros(lhs.shape[0], n_chunk, dtype=torch.float32)
            for k0 in range(0, lhs.shape[1], k_chunk):
                acc = acc + lhs[:, k0 : k0 + k_chunk].float() @ rhs[
                    k0 : k0 + k_chunk,
                    n0 : n0 + n_chunk,
                ].float()
            out[:, n0 : n0 + n_chunk] = acc
        return out

    def chunked_row_sq_sum(x, k_chunk):
        acc = torch.zeros(x.shape[0], 1, dtype=torch.float32)
        for k0 in range(0, x.shape[1], k_chunk):
            x_chunk = x[:, k0 : k0 + k_chunk]
            acc = acc + (x_chunk * x_chunk).sum(dim=-1, keepdim=True)
        return acc

    q_proj = torch.zeros(batch, hidden_size, dtype=torch.float32)
    k_proj = torch.zeros(batch, kv_hidden, dtype=torch.float32)
    v_proj = torch.zeros(batch, kv_hidden, dtype=torch.float32)

    for b0 in range(0, batch, BATCH_TILE):
        b_end = min(b0 + BATCH_TILE, batch)
        x_tile = hidden_states[b0:b_end, :].float()

        sq_sum = torch.zeros(b_end - b0, 1, dtype=torch.float32)
        for k0 in range(0, hidden_size, INPUT_PROJ_K_CHUNK):
            x_chunk = x_tile[:, k0 : k0 + INPUT_PROJ_K_CHUNK]
            sq_sum = sq_sum + (x_chunk**2).sum(dim=-1, keepdim=True)
        variance = sq_sum / hidden_size + EPS
        rms = torch.sqrt(variance)
        normed = (x_tile / rms * input_rms_weight.float()).bfloat16()

        q_proj[b0:b_end, :] = tiled_matmul(normed, wq, INPUT_PROJ_K_CHUNK, Q_OUT_CHUNK)
        k_proj[b0:b_end, :] = tiled_matmul(normed, wk, KV_PROJ_K_CHUNK, KV_OUT_CHUNK)
        v_proj[b0:b_end, :] = tiled_matmul(normed, wv, KV_PROJ_K_CHUNK, KV_OUT_CHUNK)

    attn_out = torch.zeros(batch, hidden_size, dtype=torch.bfloat16)

    for b in range(batch):
        ctx_len = seq_lens[b].item()
        pos = ctx_len - 1
        ctx_blocks = (ctx_len + BLOCK_SIZE - 1) // BLOCK_SIZE

        cos_row = rope_cos[pos : pos + 1, :]
        sin_row = rope_sin[pos : pos + 1, :]
        cos_lo, cos_hi = cos_row[:, :half], cos_row[:, half:]
        sin_lo, sin_hi = sin_row[:, :half], sin_row[:, half:]

        k_heads = k_proj[b].view(num_kv_heads, head_dim)
        k_variance = k_heads.pow(2).mean(dim=-1, keepdim=True)
        k_heads = k_heads * torch.rsqrt(k_variance + eps) * k_norm_weight.float()
        k_lo_h, k_hi_h = k_heads[:, :half], k_heads[:, half:]
        k_rot = torch.cat(
            [k_lo_h * cos_lo - k_hi_h * sin_lo, k_hi_h * cos_hi + k_lo_h * sin_hi],
            dim=-1,
        )
        slot = int(slot_mapping[b].item())
        slot_block = slot // BLOCK_SIZE
        slot_offset = slot % BLOCK_SIZE

        for ki in range(num_kv_heads):
            cache_row = (slot_block * num_kv_heads + ki) * BLOCK_SIZE + slot_offset
            k_cache[cache_row, :] = k_rot[ki].to(torch.bfloat16)
            v_cache[cache_row, :] = v_proj[b, ki * head_dim : (ki + 1) * head_dim].to(torch.bfloat16)

        q_heads = q_proj[b].view(num_heads, head_dim)
        q_variance = q_heads.pow(2).mean(dim=-1, keepdim=True)
        q_heads = q_heads * torch.rsqrt(q_variance + eps) * q_norm_weight.float()
        q_lo_h, q_hi_h = q_heads[:, :half], q_heads[:, half:]
        q_rot = torch.cat(
            [q_lo_h * cos_lo - q_hi_h * sin_lo, q_hi_h * cos_hi + q_lo_h * sin_hi],
            dim=-1,
        )

        attn_row = torch.zeros(1, hidden_size, dtype=torch.bfloat16)
        for kvh in range(num_kv_heads):
            for qg in range(q_groups):
                q_base = kvh * q_per_kv + qg * Q_HEAD_BATCH
                q_grp_bf16 = q_rot[q_base : q_base + Q_HEAD_BATCH, :].to(torch.bfloat16)

                oi = torch.zeros(Q_HEAD_BATCH, head_dim, dtype=torch.float32)
                li = torch.zeros(Q_HEAD_BATCH, 1, dtype=torch.float32)
                mi = torch.zeros(Q_HEAD_BATCH, 1, dtype=torch.float32)

                for sb in range(ctx_blocks):
                    s0 = sb * BLOCK_SIZE
                    valid_len = min(BLOCK_SIZE, ctx_len - s0)
                    pbid = int(block_table[b * max_ctx_blocks + sb].item())
                    cache_row0 = (pbid * num_kv_heads + kvh) * BLOCK_SIZE
                    k_tile = k_cache[cache_row0 : cache_row0 + BLOCK_SIZE, :]
                    v_tile = v_cache[cache_row0 : cache_row0 + BLOCK_SIZE, :]

                    raw_scores = q_grp_bf16.float() @ k_tile.float().T
                    if valid_len < BLOCK_SIZE:
                        raw_scores[:, valid_len:] = torch.finfo(torch.float32).min
                    scores = raw_scores * scale
                    cur_mi = scores.max(dim=-1, keepdim=True).values
                    exp_scores = torch.exp(scores - cur_mi)
                    exp_scores_bf16 = exp_scores.to(torch.bfloat16)
                    cur_li = exp_scores_bf16.float().sum(dim=-1, keepdim=True)
                    oi_tmp = exp_scores_bf16.float() @ v_tile.float()

                    if sb == 0:
                        oi = oi_tmp
                        li = cur_li
                        mi = cur_mi
                    else:
                        mi_new = torch.maximum(mi, cur_mi)
                        alpha = torch.exp(mi - mi_new)
                        beta = torch.exp(cur_mi - mi_new)
                        li = alpha * li + beta * cur_li
                        oi = oi * alpha + oi_tmp * beta
                        mi = mi_new

                ctx = oi / li
                ctx_flat_bf16 = ctx.reshape(1, -1).to(torch.bfloat16)
                attn_row[
                    :,
                    q_base * head_dim : (q_base + Q_HEAD_BATCH) * head_dim,
                ] = ctx_flat_bf16

        attn_out[b : b + 1, :] = attn_row

    o_proj = tiled_matmul(attn_out, wo, OUT_PROJ_K_CHUNK, OUT_PROJ_N_CHUNK)
    resid1 = o_proj + hidden_states.float()

    variance = chunked_row_sq_sum(resid1, K_CHUNK) / hidden_size
    inv_rms = torch.rsqrt(variance + eps)
    normed_bf16 = (resid1 * inv_rms * post_rms_weight).bfloat16()

    gate = tiled_matmul(normed_bf16, w_gate, K_CHUNK, MLP_OUT_CHUNK)
    up = tiled_matmul(normed_bf16, w_up, K_CHUNK, MLP_OUT_CHUNK)
    mlp_bf16 = (gate * torch.sigmoid(gate) * up).bfloat16()
    down = tiled_matmul(mlp_bf16, w_down, DOWN_MLP_CHUNK, DOWN_OUT_CHUNK)

    tensors["out"][:] = (down + resid1).bfloat16()


# Orchestration argument signature — shared by all SPMD tiers.
_ORCH_SIGNATURE = [
    D.IN, D.IN, D.IN, D.IN, D.IN, D.IN, D.IN, D.IN, D.IN, D.IN,
    D.IN, D.IN, D.INOUT, D.INOUT, D.IN, D.IN, D.IN, D.IN, D.IN, D.OUT,
]

# Kernel set and func-id assignments — identical across all SPMD tiers; the
# tier is selected purely by which orchestration source is compiled in.
_INCORES = [
            {
                "func_id": 0,
                "name": "rmsnorm",
                "source": "kernels/aiv/rmsnorm.cpp",
                "core_type": "aiv",
                "signature": [D.IN, D.OUT, D.IN],
            },
            {
                "func_id": 1,
                "name": "q_proj",
                "source": "kernels/aic/q_proj.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 2,
                "name": "k_proj",
                "source": "kernels/aic/k_proj.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 3,
                "name": "v_proj",
                "source": "kernels/aic/v_proj.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 4,
                "name": "qk_norm",
                "source": "kernels/aiv/qk_norm.cpp",
                "core_type": "aiv",
                "signature": [D.OUT, D.OUT, D.IN, D.IN, D.IN, D.IN],
            },
            {
                "func_id": 5,
                "name": "rope_kv_cache",
                "source": "kernels/aiv/rope_kv_cache.cpp",
                "core_type": "aiv",
                "signature": [D.OUT, D.OUT, D.OUT, D.IN, D.IN, D.IN, D.IN, D.IN, D.IN, D.IN],
            },
            {
                "func_id": 6,
                "name": "qk_matmul",
                "source": "kernels/aic/qk_matmul.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.OUT, D.IN, D.IN],
            },
            {
                "func_id": 7,
                "name": "softmax",
                "source": "kernels/aiv/softmax.cpp",
                "core_type": "aiv",
                "signature": [D.OUT, D.OUT, D.OUT, D.IN],
            },
            {
                "func_id": 8,
                "name": "sv_matmul",
                "source": "kernels/aic/sv_matmul.cpp",
                "core_type": "aic",
                "signature": [D.OUT, D.IN, D.IN, D.IN],
            },
            {
                "func_id": 9,
                "name": "online_softmax",
                "source": "kernels/aiv/online_softmax.cpp",
                "core_type": "aiv",
                "signature": [D.IN, D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 10,
                "name": "out_proj_residual_aic",
                "source": "kernels/aic/out_proj_residual_aic.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.IN, D.INOUT, D.OUT],
            },
            {
                "func_id": 11,
                "name": "out_proj_residual_aiv",
                "source": "kernels/aiv/out_proj_residual_aiv.cpp",
                "core_type": "aiv",
                "signature": [D.IN, D.IN, D.IN, D.INOUT, D.OUT],
            },
            {
                "func_id": 12,
                "name": "post_rmsnorm",
                "source": "kernels/aiv/post_rmsnorm.cpp",
                "core_type": "aiv",
                "signature": [D.IN, D.OUT, D.IN],
            },
            {
                "func_id": 13,
                "name": "gate_proj",
                "source": "kernels/aic/gate_proj.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 14,
                "name": "up_proj",
                "source": "kernels/aic/up_proj.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 15,
                "name": "silu",
                "source": "kernels/aiv/silu.cpp",
                "core_type": "aiv",
                "signature": [D.IN, D.IN, D.OUT],
            },
            {
                "func_id": 16,
                "name": "down_proj",
                "source": "kernels/aic/down_proj.cpp",
                "core_type": "aic",
                "signature": [D.IN, D.IN, D.INOUT],
            },
            {
                "func_id": 17,
                "name": "down_proj_residual",
                "source": "kernels/aiv/down_proj_residual.cpp",
                "core_type": "aiv",
                "signature": [D.IN, D.IN, D.OUT],
            },
]


class _Qwen3DecodeMixin:
    """Shared golden/args/runner for the SPMD tiers.

    Not a ``SceneTestCase`` itself (so it is not collected as a test); each tier
    subclass below mixes this in and supplies a ``CALLABLE`` that differs only in
    the orchestration source compiled for that tier.
    """

    RTOL = 3e-3
    ATOL = 3e-3

    CASES = [
        {
            "name": "Case1",
            "platforms": ["a2a3", "a2a3sim"],
            "config": {"aicpu_thread_num": 4, "block_dim": 24},
            "params": {"dtype": "bfloat16"},
        },
        {
            "name": "Case2",
            "platforms": ["1c1vsim"],
            "config": {"aicpu_thread_num": 7, "block_dim": 120},
            "params": {"dtype": "bfloat16"},
        },
    ]

    def generate_args(self, params):
        def _build(_p):
            batch = BATCH
            max_blocks = _max_blocks_per_seq()
            num_blocks = batch * max_blocks
            cache_rows = _cache_rows(batch)

            hidden_states = ((torch.rand(batch, HIDDEN, dtype=torch.float32) - 0.5)).to(torch.bfloat16)
            input_rms_weight = torch.rand(1, HIDDEN, dtype=torch.float32) - 0.5
            wq = torch.rand(HIDDEN, HIDDEN, dtype=torch.float32) / HIDDEN**0.5
            wq = wq.to(torch.bfloat16)
            wk = torch.rand(HIDDEN, KV_HIDDEN, dtype=torch.float32) / HIDDEN**0.5
            wk = wk.to(torch.bfloat16)
            wv = SYNTHETIC_PROJ_SCALE * (torch.rand(HIDDEN, KV_HIDDEN, dtype=torch.float32) / HIDDEN**0.5)
            wv = wv.to(torch.bfloat16)

            q_norm_weight = torch.ones(1, HEAD_DIM, dtype=torch.float32)
            k_norm_weight = torch.ones(1, HEAD_DIM, dtype=torch.float32)

            seq_lens = torch.randint(1, MAX_SEQ + 1, (batch,), dtype=torch.int32)
            block_table = torch.arange(num_blocks, dtype=torch.int32)

            slot_mapping = torch.empty(batch, dtype=torch.int32)
            for b in range(batch):
                pos = int(seq_lens[b].item()) - 1
                logical_block = pos // BLOCK_SIZE
                page_offset = pos % BLOCK_SIZE
                phys_block = b * max_blocks + logical_block
                slot_mapping[b] = phys_block * BLOCK_SIZE + page_offset

            rope_cos = torch.rand(MAX_SEQ, HEAD_DIM, dtype=torch.float32) - 0.5
            rope_sin = torch.rand(MAX_SEQ, HEAD_DIM, dtype=torch.float32) - 0.5

            k_cache = ((torch.rand(cache_rows, HEAD_DIM, dtype=torch.float32) - 0.5)).to(torch.bfloat16)
            v_cache = (SYNTHETIC_PROJ_SCALE * (torch.rand(cache_rows, HEAD_DIM, dtype=torch.float32) - 0.5)).to(
                torch.bfloat16
            )

            wo = (SYNTHETIC_PROJ_SCALE * (torch.rand(HIDDEN, HIDDEN, dtype=torch.float32) - 0.5) / HIDDEN**0.5).to(
                torch.bfloat16
            )
            post_rms_weight = torch.ones(1, HIDDEN, dtype=torch.float32)
            w_gate = (SYNTHETIC_PROJ_SCALE * (torch.rand(HIDDEN, INTERMEDIATE, dtype=torch.float32) - 0.5) / HIDDEN**0.5).to(
                torch.bfloat16
            )
            w_up = (SYNTHETIC_PROJ_SCALE * (torch.rand(HIDDEN, INTERMEDIATE, dtype=torch.float32) - 0.5) / HIDDEN**0.5).to(
                torch.bfloat16
            )
            w_down = (SYNTHETIC_PROJ_SCALE * (torch.rand(INTERMEDIATE, HIDDEN, dtype=torch.float32) - 0.5) / INTERMEDIATE**0.5).to(
                torch.bfloat16
            )
            out = torch.zeros(batch, HIDDEN, dtype=torch.bfloat16)

            return TaskArgsBuilder(
                Tensor("hidden_states", hidden_states),
                Tensor("input_rms_weight", input_rms_weight),
                Tensor("wq", wq),
                Tensor("wk", wk),
                Tensor("wv", wv),
                Tensor("q_norm_weight", q_norm_weight),
                Tensor("k_norm_weight", k_norm_weight),
                Tensor("seq_lens", seq_lens),
                Tensor("block_table", block_table),
                Tensor("slot_mapping", slot_mapping),
                Tensor("rope_cos", rope_cos),
                Tensor("rope_sin", rope_sin),
                Tensor("k_cache", k_cache),
                Tensor("v_cache", v_cache),
                Tensor("wo", wo),
                Tensor("post_rms_weight", post_rms_weight),
                Tensor("w_gate", w_gate),
                Tensor("w_up", w_up),
                Tensor("w_down", w_down),
                Tensor("out", out),
            )

        return _build(params)

    def compute_golden(self, args, params):
        tensors = {s.name: s.value for s in args.specs if isinstance(s, Tensor)}
        _compute_golden(tensors, params)
        for s in args.specs:
            if isinstance(s, Tensor) and s.name in tensors:
                getattr(args, s.name)[:] = tensors[s.name]

    def _run_and_validate_l2(
        self,
        worker,
        callable_obj,
        case,
        rounds=1,
        skip_golden=False,
        enable_l2_swimlane=False,
        enable_dump_tensor=False,
        enable_pmu=0,
        enable_dep_gen=False,
        enable_scope_stats=False,
        output_prefix="",
    ):
        case_name = case.get("name", "case")
        orig_compare = _SCENE_TEST_MOD._compare_outputs

        def _compare_outputs(test_args, golden_args, output_names, r, a):
            # golden 校验不包含 kv cache（仅校验 out）
            output_names = [n for n in output_names if n not in ("k_cache", "v_cache")]
            mismatches: list[tuple[str, float, str | None]] = []
            for name in output_names:
                act = getattr(test_args, name)
                exp = getattr(golden_args, name)
                ok, reason = _golden_allclose_relaxed(act, exp, r, a)
                if not ok:
                    diff = (act - exp).abs().max().item()
                    mismatches.append((name, diff, reason))
            if mismatches:
                ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                work_dir = _ROOT / "outputs" / "golden_mismatch" / f"{_safe_dir_label(case_name)}_{ts}"
                comparisons = [
                    (n, getattr(test_args, n).detach().cpu(), getattr(golden_args, n).detach().cpu())
                    for n in output_names
                ]
                _emit_golden_mismatch_dumps(work_dir=work_dir, comparisons=comparisons)
                detail_lines: list[str] = []
                for n, d, rs in mismatches:
                    line = f"  - '{n}': max_diff={d}, rtol={r}, atol={a}"
                    if rs:
                        line += f" — {rs}"
                    detail_lines.append(line)
                raise AssertionError(
                    "Golden mismatch: every output tensor was checked; "
                    f"{len(mismatches)} failed:\n"
                    + "\n".join(detail_lines)
                    + f"\nSee .pt files under {work_dir.resolve()}"
                )

        _SCENE_TEST_MOD._compare_outputs = _compare_outputs
        try:
            super()._run_and_validate_l2(
                worker,
                callable_obj,
                case,
                rounds=rounds,
                skip_golden=skip_golden,
                enable_l2_swimlane=enable_l2_swimlane,
                enable_dump_tensor=enable_dump_tensor,
                enable_pmu=enable_pmu,
                enable_dep_gen=enable_dep_gen,
                enable_scope_stats=enable_scope_stats,
                output_prefix=output_prefix,
            )
        finally:
            _SCENE_TEST_MOD._compare_outputs = orig_compare


# --- SPMD tier as a compile-time -D, single shared orchestration source ---------
# All tiers compile the SAME orchestration/qwen3_decode.cpp; the tier is
# passed to the compiler as -DQWEN3_SPMD_TIER=<tier> (no per-tier source files).
# Each test class declares its tier via CALLABLE["orchestration"]["spmd_tier"];
# the hooks below thread it into the orchestration compile flags. The orchestration
# toolchains (host g++ for *sim, aarch64 g++ for hardware) are the only ones
# patched, so the macro never reaches the (tier-agnostic) kernel compiles.
from simpler_setup import toolchain as _toolchain  # noqa: E402

_ACTIVE_SPMD_TIER = {"value": None}

_orig_compile_spec = _SCENE_TEST_MOD._compile_chip_callable_from_spec


def _compile_chip_callable_with_tier(spec, platform, runtime, cache_key):
    prev = _ACTIVE_SPMD_TIER["value"]
    _ACTIVE_SPMD_TIER["value"] = spec.get("orchestration", {}).get("spmd_tier")
    try:
        return _orig_compile_spec(spec, platform, runtime, cache_key)
    finally:
        _ACTIVE_SPMD_TIER["value"] = prev


_SCENE_TEST_MOD._compile_chip_callable_from_spec = _compile_chip_callable_with_tier


def _wrap_orch_compile_flags(toolchain_cls):
    orig = toolchain_cls.get_compile_flags

    def _patched(self, **kwargs):
        flags = list(orig(self, **kwargs))
        tier = _ACTIVE_SPMD_TIER["value"]
        if tier is not None:
            flags.append(f"-DQWEN3_SPMD_TIER={int(tier)}")
        return flags

    toolchain_cls.get_compile_flags = _patched


for _tc_cls in (_toolchain.GxxToolchain, _toolchain.Aarch64GxxToolchain):
    _wrap_orch_compile_flags(_tc_cls)


def _callable(spmd_tier):
    return {
        "orchestration": {
            "source": "orchestration/qwen3_decode.cpp",
            "function_name": "aicpu_orchestration_entry",
            "signature": _ORCH_SIGNATURE,
            "spmd_tier": spmd_tier,
        },
        "incores": _INCORES,
    }


@scene_test(level=2, runtime="tensormap_and_ringbuffer")
class TestQwen314bDynamicTensormapNonSpmdDecode(_Qwen3DecodeMixin, SceneTestCase):
    """--non-spmd (tier 0) — m=1: every chunk is its own single-block task."""

    CALLABLE = _callable(0)


@scene_test(level=2, runtime="tensormap_and_ringbuffer")
class TestQwen314bDynamicTensormapSpmd2Decode(_Qwen3DecodeMixin, SceneTestCase):
    """--spmd-2 (tier 1) — m=2: every chunkable op uses blocks_per_task=min(2, total_chunks)."""

    CALLABLE = _callable(1)


@scene_test(level=2, runtime="tensormap_and_ringbuffer")
class TestQwen314bDynamicTensormapSpmd4Decode(_Qwen3DecodeMixin, SceneTestCase):
    """--spmd-4 (tier 2) — m=4: every chunkable op uses blocks_per_task=min(4, total_chunks)."""

    CALLABLE = _callable(2)


@scene_test(level=2, runtime="tensormap_and_ringbuffer")
class TestQwen314bDynamicTensormapSpmd8Decode(_Qwen3DecodeMixin, SceneTestCase):
    """--spmd-8 (tier 3) — m=8: every chunkable op uses blocks_per_task=min(8, total_chunks)."""

    CALLABLE = _callable(3)


@scene_test(level=2, runtime="tensormap_and_ringbuffer")
class TestQwen314bDynamicTensormapAllSpmdDecode(_Qwen3DecodeMixin, SceneTestCase):
    """--all-spmd (tier 4) — m=total_chunks: one SPMD task per chunkable op."""

    CALLABLE = _callable(4)


# Map each SPMD CLI flag to its test class. Passing one or more of these flags on
# the standalone runner selects exactly those configs; with none given, every
# config runs (matching pytest collection of all five classes).
_SPMD_FLAG_TO_CASE = {
    "--non-spmd": "TestQwen314bDynamicTensormapNonSpmdDecode",
    "--spmd-2": "TestQwen314bDynamicTensormapSpmd2Decode",
    "--spmd-4": "TestQwen314bDynamicTensormapSpmd4Decode",
    "--spmd-8": "TestQwen314bDynamicTensormapSpmd8Decode",
    "--all-spmd": "TestQwen314bDynamicTensormapAllSpmdDecode",
}


def _apply_spmd_flags(argv):
    """Translate --non-spmd/--spmd-2/--spmd-4/--spmd-8/--all-spmd into --case
    selectors so the framework's fixed argparse never sees the custom flags."""
    selected = [case for flag, case in _SPMD_FLAG_TO_CASE.items() if flag in argv]
    rest = [a for a in argv if a not in _SPMD_FLAG_TO_CASE]
    for case in selected:
        rest += ["--case", case]
    return rest


if __name__ == "__main__":
    import sys

    sys.argv = [sys.argv[0]] + _apply_spmd_flags(sys.argv[1:])
    SceneTestCase.run_module(__name__)
