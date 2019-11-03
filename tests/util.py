#!/usr/bin/python3

import os
import re
import subprocess
import tempfile
from pathlib import Path

DIR = Path('..')
BCH_PATH = DIR / 'bcachefs'

VPAT = re.compile(r'ERROR SUMMARY: (\d+) errors from (\d+) contexts')

class ValgrindFailedError(Exception):
    def __init__(self, log):
        self.log = log

def check_valgrind(logfile):
    log = logfile.read().decode('utf-8')
    m = VPAT.search(log)
    assert m is not None, 'Internal error: valgrind log did not match.'

    errors = int(m.group(1))
    if errors > 0:
        raise ValgrindFailedError(log)

def run(cmd, *args, valgrind=False, check=False):
    """Run an external program via subprocess, optionally with valgrind.

    This subprocess wrapper will capture the stdout and stderr. If valgrind is
    requested, it will be checked for errors and raise a
    ValgrindFailedError if there's a problem.
    """
    cmds = [cmd] + list(args)

    if valgrind:
        vout = tempfile.NamedTemporaryFile()
        vcmd = ['valgrind',
               '--leak-check=full',
               '--log-file={}'.format(vout.name)]
        cmds = vcmd + cmds

    print("Running '{}'".format(cmds))
    res = subprocess.run(cmds, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         encoding='utf-8', check=check)

    if valgrind:
        check_valgrind(vout)

    return res

def run_bch(*args, **kwargs):
    """Wrapper to run the bcachefs binary specifically."""
    cmds = [BCH_PATH] + list(args)
    return run(*cmds, **kwargs)

def sparse_file(lpath, size):
    """Construct a sparse file of the specified size.

    This is typically used to create device files for bcachefs.
    """
    path = Path(lpath)
    f = path.touch(mode = 0o600, exist_ok = False)
    os.truncate(path, size)

    return path

def device_1g(tmpdir):
    """Default 1g sparse file for use with bcachefs."""
    path = tmpdir / 'dev-1g'
    return sparse_file(path, 1024**3)
