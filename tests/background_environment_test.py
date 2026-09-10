#!/usr/bin/env python3
"""Serverless controls drive the actual normal observer and canonical contract."""
import contextlib
import copy
from dataclasses import replace
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import background_environment as env
import gate_quiet as quiet


def document(server=False, incomplete=False):
    comm = "memcached" if server else "frontend"
    identity = {"pid": 20, "start_ticks": 3, "uid": 1000,
        "exe": {"path": "/test/" + comm, "device": 1, "inode": 2, "size": 3,
                "mtime_ns": 1, "ctime_ns": 2, "sha256": "a" * 64},
        "script": None, "argv_sha256": "b" * 64}
    if incomplete:
        identity = {"provenance": "incomplete-executable", "pid":20, "start_ticks":3, "comm":comm,
            "proc_directory_uid":124,"status_uids":[124]*4,"exe":None,
            "executable_observation":env.denied(PermissionError(13,"denied")),
            "argv_observation":{"status":"readable","bytes":20,"sha256":"b"*64},
            "limitation":"executable provenance unknown"}
    row = {"pid":20,"start_ticks":3,"comm":comm,"parent_pid":1,"affinity":[0,1],"cpu_ticks":100,
           "reviewed":True,"classification":"idle-server" if server else "interactive-frontend","identity":identity}
    if server:
        row["listener_ports"] = [11211]
    if incomplete:
        row["accept_incomplete_executable"] = True
    return {"schema":1,"captured_at":10,"processes":[row],"review":{"scope":"explicit idle review"}}


class Controls(unittest.TestCase):
    @contextlib.contextmanager
    def observer(self, declaration=None):
        rows = {10:quiet.Process(10,1,1,"python3",0,frozenset([0,1])),
                20:quiet.Process(20,3,1,declaration["processes"][0]["comm"] if declaration else "frontend",100,frozenset([0,1]))}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"review.json"
            if declaration:
                path.write_text(json.dumps(declaration))
            with mock.patch.object(quiet,"snapshot",side_effect=lambda **kwargs:dict(rows)), \
                 mock.patch.object(quiet,"read_topology",side_effect=lambda cpus:{cpu:frozenset([cpu]) for cpu in cpus}), \
                 mock.patch.object(env,"identity",side_effect=lambda pid,start:declaration["processes"][0]["identity"]), \
                 mock.patch.object(env,"incomplete_identity",side_effect=lambda pid,start:declaration["processes"][0]["identity"]), \
                 mock.patch.object(env,"tcp_snapshot",return_value={"ports":[11211],"rows":[{"local_port":11211,"remote_port":0,"state":10}]}) as tcp:
                observer = quiet.QuietMonitor([0],[1],own_root_pid=10,
                    background_environment=path if declaration else None,sample_artifact=Path(tmp)/"samples.jsonl")
                yield observer,rows,tcp

    def test_default_one_tick_refuses_and_remedy_is_explicit_review(self):
        with self.observer() as (observer,rows,tcp):
            rows[20]=replace(rows[20],ticks=101)
            observer.sample()
            with self.assertRaisesRegex(quiet.QuietViolation,"background-environment"):
                observer.check()
            self.assertIsNone(observer.cpu_budget_seconds)
            self.assertEqual(observer.evidence()["foreign_cpu_activity"][0]["cpu_ticks"],1)

    def test_reviewed_ticks_are_recorded_without_translating_them_to_rate_error(self):
        with self.observer(document()) as (observer,rows,tcp):
            rows[20]=replace(rows[20],ticks=201)
            observer.sample();observer.check();observer.close();observer.check()
            evidence=observer.evidence()
            self.assertTrue(evidence["complete"])
            self.assertIsNone(evidence["generic_cpu_screening"]["cpu_budget_seconds"])
            self.assertEqual(evidence["foreign_cpu_activity"][0]["cpu_ticks"],101)
            self.assertEqual(evidence["background_environment"]["sample_count"],2)
            samples=[json.loads(s) for s in observer.sample_artifact.read_text().splitlines()]
            self.assertEqual(samples[0]["user_cpu_activity"][0]["cpu_ticks"],101)
            self.assertEqual(samples[0]["environment"]["identities_checked"],1)

    def test_children_and_known_work_never_inherit_frontend_review(self):
        for name,ticks,driver in (("worker",1,False),("cc1plus",0,False),("python3",0,True)):
            with self.subTest(name=name),self.observer(document()) as (observer,rows,tcp):
                rows[30]=quiet.Process(30,4,20,name,ticks,frozenset([0]),experiment_driver=driver)
                observer.sample()
                with self.assertRaises(quiet.QuietViolation):observer.check()

    def test_identity_parent_affinity_exit_and_counter_changes_latch(self):
        for fields in ({"start":99},{"parent":99},{"name":"changed"},{"affinity":frozenset([0])},{"ticks":99}):
            with self.subTest(fields=fields),self.observer(document()) as (observer,rows,tcp):
                before=rows[20];rows[20]=replace(before,**fields);observer.sample();rows[20]=before;observer.sample()
                with self.assertRaises(quiet.QuietViolation):observer.check()
        with self.observer(document()) as (observer,rows,tcp):
            rows.pop(20);observer.sample()
            with self.assertRaisesRegex(quiet.QuietViolation,"exited or changed"):observer.check()

    def test_exec_change_and_observation_errors_fail(self):
        with self.observer(document()) as (observer,rows,tcp),mock.patch.object(env,"identity",return_value={}):
            observer.sample()
            with self.assertRaisesRegex(quiet.QuietViolation,"identity changed"):observer.check()
        with self.observer(document()) as (observer,rows,tcp),mock.patch.object(env,"identity",side_effect=PermissionError("unobserved")):
            observer.sample()
            with self.assertRaisesRegex(quiet.QuietViolation,"unobserved"):observer.check()

    def test_full_and_incomplete_idle_servers_keep_every_tcp_sample_and_tick(self):
        for incomplete in (False,True):
            with self.subTest(incomplete=incomplete),self.observer(document(True,incomplete)) as (observer,rows,tcp):
                rows[20]=replace(rows[20],ticks=101);observer.sample();observer.check();observer.close()
                evidence=observer.evidence()["background_environment"]
                self.assertEqual((evidence["sample_count"],evidence["listener_snapshots"]),(2,2))
                self.assertEqual(tcp.call_count,2)

    def test_foreign_server_live_tcp_state_remains_loud_and_retained(self):
        with self.observer(document(True,True)) as (observer,rows,tcp):
            tcp.return_value={"ports":[11211],"rows":[{"local_port":11211,"remote_port":1234,"state":1}]}
            observer.sample()
            with self.assertRaisesRegex(quiet.QuietViolation,"live TCP"):observer.check()
            sample=json.loads(observer.sample_artifact.read_text())
            self.assertEqual(sample["environment"]["tcp_snapshot"]["rows"][0]["state"],1)
        with self.observer(document(True)) as (observer,rows,tcp):
            rows[30]=quiet.Process(30,4,20,"sleep",0,frozenset([0]));observer.sample()
            with self.assertRaisesRegex(quiet.QuietViolation,"descendants"):observer.check()

    def test_contract_excludes_capture_ticks_but_binds_all_reviewed_provenance(self):
        value=document();contract=env.canonical_contract(value)
        other=copy.deepcopy(value);other["captured_at"]=999;other["processes"][0]["cpu_ticks"]+=500
        self.assertEqual(env.canonical_contract(other),contract)
        for key,replacement in (("parent_pid",2),("affinity",[0]),("classification","desktop")):
            changed=copy.deepcopy(value);changed["processes"][0][key]=replacement
            self.assertNotEqual(env.canonical_contract(changed),contract)
        self.assertEqual(env.validate_contract(contract),contract)
        for key,replacement in (("sha256","0"*64),("policy",env.STRICT),("reviewed_identities",[])):
            with self.subTest(key=key),self.assertRaises(ValueError):env.validate_contract({**contract,key:replacement})


if __name__=="__main__":
    unittest.main()
