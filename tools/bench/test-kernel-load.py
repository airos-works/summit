#!/usr/bin/env python3
"""Contention attribution must not hide unrelated work or idle CPU time."""
import copy
import unittest
from unittest.mock import patch
import guest


class KernelLoadTest(unittest.TestCase):
    def report(self, owned=(42,), names=(), extra=None):
        data = dict(at=0, cpuCount=32, busyCores=1.2, usedBytes=0, cachedBytes=0,
                    teams=[dict(team=42, args='/boot/Summit', cores=.2, threads=1, areas=1)],
                    kernelWorkers=[dict(thread=80, name='amdgpu jobs', cores=.8),
                                   dict(thread=81, name='usb explore', cores=.05),
                                   dict(thread=2, name='idle thread 1', cores=30)])
        if extra:
            data['teams'].append(extra)
            data['busyCores'] += extra['cores']
        with patch.object(guest, 'sample', return_value=copy.deepcopy(data)):
            return guest.load_report('unused', owned, 1000, names)

    def test_default_does_not_exempt_kernel_work(self):
        self.assertTrue(self.report()['contended'])

    def test_isolated_gpu_worker_is_reported_separately(self):
        r = self.report(names=('amdgpu jobs',))
        self.assertFalse(r['contended'])
        self.assertEqual(r['ownedKernelCores'], .8)
        self.assertEqual(r['rawUnexplainedBusyCores'], 1)
        self.assertEqual(r['unexplainedBusyCores'], .2)

    def test_no_owned_team_no_attribution(self):
        r = self.report(owned=(), names=('amdgpu jobs',))
        self.assertTrue(r['contended'])
        self.assertEqual(r['ownedKernelCores'], 0)

    def test_foreign_compiler_remains_contended(self):
        r = self.report(names=('amdgpu jobs',), extra=dict(
            team=43, args='/boot/gcc', cores=.3, threads=1, areas=1))
        self.assertTrue(r['contended'])
        self.assertEqual(r['compilerTeams'], 1)

    def test_exact_names_and_idle_exclusion(self):
        self.assertTrue(self.report(names=('amdgpu',))['contended'])
        r = self.report(names=('idle thread 1',))
        self.assertTrue(r['contended'])
        self.assertEqual(r['ownedKernelCores'], 0)


if __name__ == '__main__':
    unittest.main()
