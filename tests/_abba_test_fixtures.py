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


def quiet_record(*, cpus=tuple(range(128)), samples=2, started_at=0., finished_at=20.,
                 sample_artifact="/fake/quiet-samples.jsonl", ports=(8700,),
                 server_physical_cores=32, window_seconds=20):
    """Selected-core evidence for serverless validators; no process inventory."""
    return dict(complete=True, interference=None, started_at=started_at, finished_at=finished_at,
        samples=samples, cpu_samples=samples, sample_interval_seconds=1,
        cpus=list(cpus), requested_cpus=list(cpus), policy="selected-core-port-budget-v1",
        sample_artifact=str(sample_artifact), ports=list(ports), listener_checks=1,
        generic_cpu_screening=dict(scope="preflight", preflight_seconds=20., capacity_fraction=.0015,
            server_physical_cores=server_physical_cores, window_seconds=window_seconds,
            cpu_budget_seconds=.0015 * server_physical_cores * window_seconds,
            peak_rolling=dict(cpu_ticks=0, cpu_seconds=0.)))
