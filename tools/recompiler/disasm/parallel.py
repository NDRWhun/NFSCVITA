"""Per-function decisions and code emission of the bucket files in worker processes.

The main process runs discovery and the whole-program analyses. Each worker decodes the functions of a bucket
again from the discovery tables, repeats their per-function analysis (classify, record), takes the x87 depths and
liveness the main process computed, decides and writes the bucket file. Every file depends only on its own
functions and the shared state, so the output does not depend on the number of workers or their order.
"""

import copy
import multiprocessing
import os
import pickle
import sys
import tempfile

import capstone

from . import analysis, discover, writer

# Discovery state the workers need to decode the functions again.
DISCOVERY_STATE = ('entries', 'tables', 'index_tables', 'data_refs', 'map', 'no_return', 'no_return_slots')

_worker = None


class _Module:
    """The parts of the game module the code writers read."""
    def __init__(self, module):
        self.application_name = module.application_name
        self.image = module.image
        self.entry_point = module.entry_point


def _init(path):
    """Worker set-up from the shared state file; prints go nowhere (the main process reports)."""
    global _worker
    sys.stdout = open(os.devnull, 'w')
    with open(path, 'rb') as f:
        shared = pickle.load(f)
    cs = capstone.Cs(*shared['capstone_mode'])
    cs.detail = True
    found = discover.Discovery(shared['module'].image, cs)
    vars(found).update(shared['discovery'])
    shared['found'] = found
    _worker = shared


def _write_bucket(task):
    start, entries, results = task
    w = _worker
    program = w['program']
    functions = []
    for entry in entries:
        sub = w['found'].subroutine(entry)
        fn = analysis.Function(w['module'], sub, w['thread_segments'], w['skip_instructions'], w['function_names'])
        fn.program = program
        fn.classify()
        fn.record()
        fn.fpu, fn.live_in, fn.live_out = results[entry]
        fn.decide()
        functions.append(fn)
    out = writer.Writer(w['module'], program, w['out_dir'], w['function_names'], w['data_imports'])
    out.write_bucket(start, functions)
    return start


def write_buckets(out, discovery, program, thread_segments, skip_instructions, jobs):
    """Writes the bucket files of writer out with jobs worker processes (Writer.write's bucket_writer)."""
    def bucket_writer(buckets):
        shared = {'discovery': dict((k, getattr(discovery, k)) for k in DISCOVERY_STATE),
                  'capstone_mode': (discovery.cs.arch, discovery.cs.mode),
                  'module': _Module(out.module), 'thread_segments': thread_segments,
                  'skip_instructions': skip_instructions, 'function_names': out.function_names,
                  'data_imports': out.data_imports, 'out_dir': out.out_dir}
        slim = copy.copy(program)
        slim.functions = {}
        shared['program'] = slim
        # Largest buckets first, so no worker is left with a large one at the end.
        tasks = []
        for start, members in buckets:
            results = dict((fn.entry_address, (fn.fpu, fn.live_in, fn.live_out)) for fn in members)
            tasks.append((sum(fn.n for fn in members), start, [fn.entry_address for fn in members], results))
        tasks.sort(key=lambda t: (-t[0], t[1]))
        path = None
        try:
            with tempfile.NamedTemporaryFile('wb', prefix='nfscgen-', suffix='.pickle', delete=False) as f:
                path = f.name
                pickle.dump(shared, f, pickle.HIGHEST_PROTOCOL)
            with multiprocessing.get_context('spawn').Pool(jobs, _init, (path,)) as pool:
                for _ in pool.imap_unordered(_write_bucket, [t[1:] for t in tasks]):
                    pass
        finally:
            if path:
                os.remove(path)
    return bucket_writer
