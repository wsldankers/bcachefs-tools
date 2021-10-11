#!/usr/bin/python3
#
# Tests of the functions in util.py

import signal
import subprocess
import time
import os
import pytest

from tests import util

helper = os.path.abspath(os.path.join(util.BASE_PATH, 'test_helper'))

def test_sparse_file(tmpdir):
    dev = util.sparse_file(tmpdir / '1k', 1024)
    assert dev.stat().st_size == 1024

def test_device_1g(tmpdir):
    dev = util.device_1g(tmpdir)
    assert dev.stat().st_size == 1024**3

def test_abort():
    ret = util.run(helper, 'abort')
    assert ret.returncode == -signal.SIGABRT

def test_segfault():
    ret = util.run(helper, 'segfault')
    assert ret.returncode == -signal.SIGSEGV

@pytest.mark.skipif(not util.ENABLE_VALGRIND, reason="no valgrind")
def test_check():
    with pytest.raises(subprocess.CalledProcessError):
        util.run(helper, 'abort', check=True)

@pytest.mark.skipif(not util.ENABLE_VALGRIND, reason="no valgrind")
def test_leak():
    with pytest.raises(util.ValgrindFailedError):
        util.run(helper, 'leak', valgrind=True)

@pytest.mark.skipif(not util.ENABLE_VALGRIND, reason="no valgrind")
def test_undefined():
    with pytest.raises(util.ValgrindFailedError):
        util.run(helper, 'undefined', valgrind=True)

@pytest.mark.skipif(not util.ENABLE_VALGRIND, reason="no valgrind")
def test_undefined_branch():
    with pytest.raises(util.ValgrindFailedError):
        util.run(helper, 'undefined_branch', valgrind=True)

@pytest.mark.skipif(not util.ENABLE_VALGRIND, reason="no valgrind")
def test_read_after_free():
    with pytest.raises(util.ValgrindFailedError):
        util.run(helper, 'read_after_free', valgrind=True)

@pytest.mark.skipif(not util.ENABLE_VALGRIND, reason="no valgrind")
def test_write_after_free():
    with pytest.raises(util.ValgrindFailedError):
        util.run(helper, 'write_after_free', valgrind=True)

def test_mountpoint(tmpdir):
    path = util.mountpoint(tmpdir)
    assert str(path)[-4:] == '/mnt'
    assert path.is_dir()

def test_timestamp():
    t1 = time.clock_gettime(time.CLOCK_REALTIME)
    with util.Timestamp() as ts:
        t2 = time.clock_gettime(time.CLOCK_REALTIME)
    t3 = time.clock_gettime(time.CLOCK_REALTIME)

    assert not ts.contains(t1)
    assert ts.contains(t2)
    assert not ts.contains(t3)
