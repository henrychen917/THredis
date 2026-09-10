"""Raw-counter fixtures for serverless ABBA controls; never used by the runner."""
from abba_saturation import LbSnapshot, SATURATION_FLOOR, bottleneck_saturation


def saturation_record(mode="1s", score=99.9, threads=32, window_seconds=20, inactive_roles=()):
    wall = round(window_seconds * 1e9)
    work = round(wall * score / 100)
    before, after = {}, {}
    for tid in range(threads):
        role = "fused" if mode == "1s" else "io" if tid < threads - threads // 2 else "ex"
        before[tid] = dict(role=role, clients=0 if role == "ex" else 1,
                           ops=0, busy=0, idle=0, cpu=0)
        active = role not in inactive_roles
        after[tid] = dict(before[tid], ops=1_000_000 if active else 0,
            busy=work if active else 0, idle=wall - work if active else wall, cpu=work if active else 0)
    return bottleneck_saturation(LbSnapshot(1_000_000_000, before),
        LbSnapshot(1_000_000_000 + wall, after), floor_pct=SATURATION_FLOOR)
