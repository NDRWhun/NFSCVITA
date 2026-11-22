"""The discovery result on disk, keyed by the executable and everything discovery depends on.

The key covers the executable's SHA-256, the discovery parameters, the capstone and Python versions and the
sources below; a file with another key, or one that does not load, is ignored and replaced.
"""

import hashlib
import os
import pickle
import sys

import capstone

FORMAT = 1
# Sources in this directory that discovery runs (or pickles objects of).
SOURCES = ('cache.py', 'discover.py', 'image.py', 'instruction.py', 'pefile.py', 'subroutine.py')
FILE = 'discovery.pickle'
# Discovery attributes not stored: the inputs (given again on load) and the decoded instructions (decoded again).
TRANSIENT = ('image', 'cs', 'log', 'decoded', 'journal')


def key(image, parameters):
    """SHA-256 over everything the discovery result depends on; parameters must have a stable repr."""
    h = hashlib.sha256()
    h.update(repr((FORMAT, image.sha256, capstone.__version__, tuple(sys.version_info[:2]), parameters)).encode())
    here = os.path.dirname(os.path.abspath(__file__))
    for name in SOURCES:
        with open(os.path.join(here, name), 'rb') as f:
            h.update(b'%s %s\n' % (name.encode(), hashlib.sha256(f.read()).hexdigest().encode()))
    return h.hexdigest()


def load(directory, key):
    """The stored state for key, or None."""
    try:
        with open(os.path.join(directory, FILE), 'rb') as f:
            if pickle.load(f) != key:
                return None
            return pickle.load(f)
    except FileNotFoundError:
        return None
    except Exception as e:      # a truncated or unreadable file counts as missing
        print('discovery cache not readable (%s): ignored' % e.__class__.__name__)
        return None


def save(directory, key, state):
    """Replaces the stored state; written to a temporary file first, so a stopped run leaves no partial file."""
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, FILE)
    temporary = '%s.%d.tmp' % (path, os.getpid())
    try:
        with open(temporary, 'wb') as f:
            pickle.dump(key, f, pickle.HIGHEST_PROTOCOL)
            pickle.dump(state, f, pickle.HIGHEST_PROTOCOL)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def discovery_state(discovery):
    return dict((k, v) for k, v in vars(discovery).items() if k not in TRANSIENT)


def restore(discovery, state):
    vars(discovery).update(state)
