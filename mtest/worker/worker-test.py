#!/usr/bin/env python
import unittest, yaml
import sys
from walblib import *
from walb_worker import *

VOL = 'vol0'

class ControllerMock:
    def __init__(self, path, layout, isDebug=False):
        '''
            mock of walb.Controller
        '''
        self.path = path
        self.layout = layout
        self.isDebug = isDebug
    def get_restorable(self, ax, vol, opt=''):
        '''
        Get restorable gid list.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        opt :: str       - you can specify 'all'.
        return :: [GidInfo] - gid info list.
        '''
        return []
    def get_total_diff_size(self, ax, vol, gid0=0, gid1=UINT64_MAX):
        '''
        Get total wdiff size.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        gid0 :: int      - gid range begin.
        gid1 :: int      - gid range end.
        return :: int    - total size in the gid range [byte].
        '''
        return 0
    def get_applicable_diff_list(self, ax, vol, gid=UINT64_MAX):
        '''
        Get wdiff to list to apply.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        gid :: u64       - target gid.
        return :: [Diff] - wdiff information list managed by the archive server.
        '''
        return []
    def get_state(self, s, vol):
        '''
        Get state of a volume.
        s :: ServerParams
        vol :: str    - volume name.
        return :: str - state.
        '''
        return ""
    def get_vol_list(self, s):
        '''
        Get volume list.
        s :: ServerParams
        return :: [str] - volume name list.
        '''
        return [VOL]
    def get_base(self, ax, vol):
        '''
        Get base meta state of a volume in an archive server.
        ax :: ServerParams    - archive server.
        vol :: str          - volume name.
        return :: MetaState - meta state.
        '''
        return ""
    def get_num_diff(self, ax, vol, gid0=0, gid1=UINT64_MAX):
        '''
        Get number of wdiff files for a volume.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        gid0 :: int      - gid range begin.
        gid1 :: int      - gid range end.
        return :: int    - number of wdiffs in the gid range.
        '''
        return 0

class TestParsePERIOD(unittest.TestCase):
    def test(self):
        d = {
            '123':123,
            '10m':10 * 60,
            '100d':100 * 86400,
        }
        for (s, expect) in d.items():
            v = parsePERIOD(s)
            self.assertEqual(v, datetime.timedelta(seconds=expect))

class TestParseSIZE_UNIT(unittest.TestCase):
    def test(self):
        d = {
            '123':123,
            '10K':10 * 1024,
            '7M':7 * 1024 * 1024,
            '8G':8 * 1024 * 1024 * 1024,
        }
        for (s, expect) in d.items():
            v = parseSIZE_UNIT(s)
            self.assertEqual(v, expect)

class TestParseCOMPRESS_OPT(unittest.TestCase):
    def test(self):
        d = {
            "none":('none', 0, 0),
            "snappy:3":('snappy', 3, 0),
            "gzip:9:4":('gzip', 9, 4),
            "lzma:0:123":('lzma', 0, 123),
        }
        for (s, expect) in d.items():
            v = parseCOMPRESS_OPT(s)
            self.assertEqual(v, expect)

configStr = """
general:
  addr: 192.168.0.1
  port: 10000
  walbc_path: binsrc/walbc
  max_concurrent_tasks: 10
apply:
  keep_period: 14d
merge:
  interval: 10
  max_nr: 10
  max_size: 1M
  threshold_nr: 5
repl_servers:
  repl0:
    addr: 192.168.0.2
    port: 10001
    interval: 3d
    compress: snappy:3:4
    max_merge_size: 5K
    bulk_size: 40
  repl1:
    addr: 192.168.0.3
    port: 10002
    interval: 2h
    compress: gzip
    max_merge_size: 2M
    bulk_size: 400
"""

class TestLoadConfigParam(unittest.TestCase):
    def test(self):
        d = yaml.load(configStr)
        cfg = Config()
        cfg.set(d)
        general = cfg.general
        self.assertEqual(general.addr, '192.168.0.1')
        self.assertEqual(general.port, 10000)
        self.assertEqual(general.max_concurrent_tasks, 10)
        apply_ = cfg.apply_
        self.assertEqual(apply_.keep_period, datetime.timedelta(days=14))
        merge = cfg.merge
        self.assertEqual(merge.interval, datetime.timedelta(seconds=10))
        self.assertEqual(merge.max_nr, 10)
        self.assertEqual(merge.max_size, 1024 * 1024)
        self.assertEqual(merge.threshold_nr, 5)
        repl_servers = cfg.repl_servers
        r = repl_servers['repl0']
        self.assertEqual(r.addr, '192.168.0.2')
        self.assertEqual(r.port, 10001)
        self.assertEqual(r.interval, datetime.timedelta(days=3))
        self.assertEqual(r.compress, ('snappy', 3, 4))
        self.assertEqual(r.max_merge_size, 5 * 1024)
        self.assertEqual(r.bulk_size, 40)

        r = repl_servers['repl1']
        self.assertEqual(r.addr, '192.168.0.3')
        self.assertEqual(r.port, 10002)
        self.assertEqual(r.interval, datetime.timedelta(hours=2))
        self.assertEqual(r.compress, ('gzip', 0, 0))
        self.assertEqual(r.max_merge_size, 2 * 1024 * 1024)
        self.assertEqual(r.bulk_size, 400)

class TestSnapshot(unittest.TestCase):
    def test(self):
        self.assertEqual(Snapshot(2, 3), Snapshot(2, 3))
        self.assertTrue(Snapshot(2, 3) != Snapshot(3, 3))
        self.assertTrue(Snapshot(2, 3) != Snapshot(2, 4))
        self.assertTrue(Snapshot(2, 3) != Snapshot(1, 3))

class TestMetaState(unittest.TestCase):
    def test(self):
        self.assertEqual(MetaState(Snapshot(2, 3)), MetaState(Snapshot(2, 3)))
        self.assertTrue(MetaState(Snapshot(2, 3), Snapshot(4, 5)) != MetaState(Snapshot(2, 3)))
        self.assertTrue(MetaState(Snapshot(2, 3)) != MetaState(Snapshot(2, 3), Snapshot(4, 5)))
        self.assertTrue(MetaState(Snapshot(2, 3)) != MetaState(Snapshot(2, 4)))
        self.assertTrue(MetaState(Snapshot(2, 3), Snapshot(3, 4)) != MetaState(Snapshot(2, 3), Snapshot(3, 5)))


class TestWoker(unittest.TestCase):
    def test(self):
        d = yaml.load(configStr)
        cfg = Config()
        cfg.set(d)
        w = Worker(cfg, ControllerMock)
#        task = w.selectTask()
        volL = w.walbc.get_vol_list(w.a0)

        def test_selectApplyTask1():
            keep = w.walbc.get_base
            i = 0
            tbl = [
                (MetaState(Snapshot()), None),
                (MetaState(Snapshot(2, 2)), None),
                (MetaState(Snapshot(2, 4)), None),
                (MetaState(Snapshot(2, 3), Snapshot(4, 5)), Task("apply", VOL, (w.a0, 2))),
            ]
            def get_base(a0, vol):
                return tbl[i][0]
            w.walbc.get_base = get_base
            for t in tbl:
                self.assertEqual(w._selectApplyTask1(volL), tbl[i][1])
                i = i + 1
            w.walbc.get_base = keep

        test_selectApplyTask1()


if __name__ == '__main__':
    unittest.main()
