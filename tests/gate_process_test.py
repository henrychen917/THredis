#!/usr/bin/env python3
"""Serverless controls for the owned server's connection/shutdown fence."""
import contextlib
import json
from pathlib import Path
import socket
import tempfile
import unittest
from unittest.mock import patch

import _gate_process as gate


class Clock:
    now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Connection:
    def __init__(self, counts=(65, 4, 1), quit_reply=b'OK', eof=True):
        self.counts = iter(counts)
        self.count = None
        self.quit_reply = quit_reply
        self.eof = eof
        self.events = []
        self.timeout = 5
        self.sock = self

    def gettimeout(self):
        return self.timeout

    def settimeout(self, timeout):
        self.timeout = timeout

    def must(self, *argv):
        self.events.append(argv)
        if argv == ('INFO', 'SERVER'):
            return b'process_id:41001\r\n'
        if argv == ('INFO', 'CLIENTS'):
            self.count = next(self.counts, self.count)
            return (b'' if self.count is None else
                    f'connected_clients:{self.count}\r\n'.encode())
        if argv == ('QUIT',):
            return self.quit_reply
        raise AssertionError(f'unexpected command {argv!r}')

    def read(self):
        self.events.append('peer-eof' if self.eof is True else 'read')
        if self.eof is True:
            raise EOFError('server closed the connection')
        if isinstance(self.eof, Exception):
            raise self.eof
        return self.eof

    def close(self):
        self.events.append('local-close')


class Process:
    pid = 41001
    returncode = None

    def __init__(self, conn, log=None, dirty=False):
        self.conn = conn
        self.log = log
        self.dirty = dirty

    def poll(self):
        return self.returncode

    def terminate(self):
        self.conn.events.append('terminate')
        self.returncode = 0
        if self.log is not None:
            # The fixture reports its inventory at the signal, like the real server. Bypassing
            # the fence therefore makes the actual server() verdict red, not merely a mock count.
            live = int(self.dirty or self.conn.count != 1 or 'peer-eof' not in self.conn.events)
            self.log.write('shutdown_report ' + json.dumps(dict(schema=1, stuck=dict(
                live_conns=live, rob_not_quiesced=0, unsent_bytes_pending=0))) + '\n')
            self.log.flush()

    def wait(self, timeout):
        self.conn.events.append('wait')
        return self.returncode


class ConnectionQuiescence(unittest.TestCase):
    @contextlib.contextmanager
    def clock(self):
        clock = Clock()
        with patch.object(gate.time, 'monotonic', clock.monotonic), \
                patch.object(gate.time, 'sleep', clock.sleep):
            yield clock

    def test_observed_releases_and_peer_eof_precede_stop(self):
        conn = Connection()
        with self.clock():
            report = gate.quiesce_connections(conn, Process(conn))
        self.assertEqual([row['connected_clients'] for row in report['samples']], [65, 4, 1])
        self.assertTrue(report['observer_eof'])
        self.assertEqual(conn.events[-2:], [('QUIT',), 'peer-eof'])
        self.assertEqual(conn.timeout, 5)

    def test_retained_client_is_bounded_failure(self):
        conn = Connection(counts=(2,))
        with self.clock() as clock, self.assertRaisesRegex(AssertionError, 'quiescence timed out'):
            gate.quiesce_connections(conn, Process(conn), timeout=.025)
        self.assertEqual(clock.now, .025)
        self.assertNotIn(('QUIT',), conn.events)

    def test_observation_cannot_be_missing_or_omit_observer(self):
        for count, error in ((None, 'missing mandatory counter'), (0, 'omitted its live observer')):
            with self.subTest(count=count), self.clock(), self.assertRaisesRegex(AssertionError, error):
                conn = Connection(counts=(count,))
                gate.quiesce_connections(conn, Process(conn))

    def test_dead_server_is_not_quiescence(self):
        conn = Connection(counts=(1,))
        process = Process(conn)
        process.returncode = 0
        with self.clock(), self.assertRaisesRegex(AssertionError, 'server exited'):
            gate.quiesce_connections(conn, process)

    def test_quit_reply_must_be_ok(self):
        conn = Connection(counts=(1,), quit_reply=b'not OK')
        with self.clock(), self.assertRaisesRegex(AssertionError, 'QUIT did not reply OK'):
            gate.quiesce_connections(conn, Process(conn))
        self.assertNotIn('peer-eof', conn.events)

    def test_quit_reply_without_close_fails(self):
        conn = Connection(counts=(1,), eof=socket.timeout('peer never closed'))
        with self.clock(), self.assertRaisesRegex(socket.timeout, 'peer never closed'):
            gate.quiesce_connections(conn, Process(conn))
        self.assertEqual(conn.timeout, 5)

    def test_extra_reply_cannot_stand_in_for_eof(self):
        conn = Connection(counts=(1,), eof=b'unexpected')
        with self.clock(), self.assertRaisesRegex(AssertionError, 'expected peer EOF'):
            gate.quiesce_connections(conn, Process(conn))

    @contextlib.contextmanager
    def fixture(self, conn, dirty=False):
        with tempfile.TemporaryDirectory(prefix='gate-process-control-') as temporary:
            directory = Path(temporary) / 'server'

            def launch(*args, **kwargs):
                return Process(conn, kwargs['stdout'], dirty)

            with self.clock(), patch.object(gate.subprocess, 'check_output', return_value=''), \
                    patch.object(gate.subprocess, 'Popen', side_effect=launch), \
                    patch.object(gate, 'Conn', return_value=conn):
                yield directory

    def test_context_records_fence_then_reaps_child(self):
        conn = Connection()
        with self.fixture(conn) as directory:
            with gate.server('/unused/binary', '0-7', 8990, directory, []):
                pass
            self.assertTrue(json.loads((directory / 'quiescence.json').read_text())['observer_eof'])
            self.assertEqual(json.loads((directory / 'exit.json').read_text())['returncode'], 0)
        self.assertEqual(conn.events[-4:], ['peer-eof', 'local-close', 'terminate', 'wait'])

    def test_drain_failure_still_reaps_owned_child(self):
        conn = Connection(counts=(2,))
        with self.fixture(conn) as directory:
            with self.assertRaisesRegex(AssertionError, 'quiescence timed out'):
                with gate.server('/unused/binary', '0-7', 8990, directory, []):
                    pass
            self.assertTrue((directory / 'exit.json').is_file())
        self.assertEqual(conn.events[-3:], ['local-close', 'terminate', 'wait'])

    def test_original_row_failure_survives_cleanup(self):
        conn = Connection()
        with self.fixture(conn) as directory:
            with self.assertRaisesRegex(AssertionError, 'original witness failed'):
                with gate.server('/unused/binary', '0-7', 8990, directory, []):
                    raise AssertionError('original witness failed')
            self.assertFalse((directory / 'quiescence.json').exists())
        self.assertNotIn(('QUIT',), conn.events)
        self.assertEqual(conn.events[-3:], ['local-close', 'terminate', 'wait'])

    def test_deleted_fence_mutation_keeps_shutdown_red(self):
        conn = Connection()
        with self.fixture(conn) as directory, patch.object(gate, 'quiesce_connections', return_value={}):
            with self.assertRaisesRegex(AssertionError, 'unclean shutdown report'):
                with gate.server('/unused/binary', '0-7', 8990, directory, []):
                    pass

    def test_dirty_shutdown_still_fails_after_successful_fence(self):
        conn = Connection()
        with self.fixture(conn, dirty=True) as directory:
            with self.assertRaisesRegex(AssertionError, 'unclean shutdown report'):
                with gate.server('/unused/binary', '0-7', 8990, directory, []):
                    pass


if __name__ == '__main__':
    unittest.main()
