#!/usr/bin/python3
#
# Tests of the functions in util.py

import pytest
import signal
import subprocess

import util
from pathlib import Path

#helper = Path('.') / 'test_helper'
helper = './test_helper'

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

def test_check():
    with pytest.raises(subprocess.CalledProcessError):
        ret = util.run(helper, 'abort', check=True)

def test_leak():
    with pytest.raises(util.ValgrindFailedError):
        ret = util.run(helper, 'leak', valgrind=True)

def test_undefined():
    with pytest.raises(util.ValgrindFailedError):
        ret = util.run(helper, 'undefined', valgrind=True)

def test_undefined_branch():
    with pytest.raises(util.ValgrindFailedError):
        ret = util.run(helper, 'undefined_branch', valgrind=True)

def test_read_after_free():
    with pytest.raises(util.ValgrindFailedError):
        ret = util.run(helper, 'read_after_free', valgrind=True)

def test_write_after_free():
    with pytest.raises(util.ValgrindFailedError):
        ret = util.run(helper, 'write_after_free', valgrind=True)
