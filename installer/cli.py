"""Command line: python -m installer (--nup FILE | --content DIR) --dest DIR [options]

--nup names the player's .nup (the base game and the 1.1.1 update); --content a folder with the game's files instead: one
tools.nup_extract wrote, or one holding exefs/ and romfs/ of the 1.1.1 update. On a folder with mtf-install.json (resume
after a cancel or failure, or update with a newer release) only --dest is needed; the game is asked for only when the
extraction has to run again. Prints JSON events on stdout (events.py). Exit code: 0 done, 1 failed, 2 bad command line,
3 cancelled (Ctrl+C or Ctrl+Break).

--dlss downloads NVIDIA's DLSS library (nvngx_dlss.dll) from NVIDIA's DLSS SDK repository into the install folder on a
PC with an NVIDIA RTX GPU (steps.py, Dlss); --dlss-file copies one the player already has. --firmware and --keys are
optional and only serve the system fonts (steps.py, Fonts).
"""
import argparse
import hashlib
import os
import signal
import sys
import time
from pathlib import Path

from . import layout as layouts
from . import state as statefile
from .events import Reporter, render
from .runner import Context, run_steps
from .steps import STEP_IDS, all_steps

EXIT = {'ok': 0, 'failed': 1, 'cancelled': 3}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        Reporter().error('cli.usage', {'detail': message})
        sys.exit(2)


def temp_folder(base, dest):
    """One temp folder per install folder, so two installs never share generated files."""
    tag = hashlib.sha256(str(dest).lower().encode()).hexdigest()[:8]
    return base / f'{dest.name or "root"}-{tag}'


def cancel(signum, frame):
    raise KeyboardInterrupt


def main(argv=None):
    ap = Parser(prog='python -m installer', description=__doc__.splitlines()[0])
    ap.add_argument('--nup', type=Path, help='your Mario Tennis Fever .nup')
    ap.add_argument('--content', type=Path, help="a folder with the game's files instead of the .nup")
    ap.add_argument('--firmware', type=Path, help="optional: the firmware's Contents/registered folder, or a folder with the "
                                                  'decoded system fonts')
    ap.add_argument('--keys', type=Path, help='prod.keys, only to read the firmware; it is read in place and never stored')
    ap.add_argument('--dest', type=Path, required=True, help='install folder (about 10 GB)')
    ap.add_argument('--root', type=Path, help='release folder or repository (default: the folder above installer/)')
    ap.add_argument('--temp', type=Path, help='temp folder (default: temp/ of the release, build/generated/installer in a '
                                              'repository)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2), help='parallel jobs')
    ap.add_argument('--dlss', action='store_true', help="download NVIDIA's nvngx_dlss.dll (NVIDIA RTX SDK license) next "
                                                      'to the game, for DLAA and the DLSS modes')
    ap.add_argument('--dlss-file', type=Path, help='an nvngx_dlss.dll to copy next to the game instead')
    ap.add_argument('--shaders-from', type=Path, help='reuse a translated shader folder instead of translating again')
    ap.add_argument('--only', nargs='+', default=[], choices=STEP_IDS, help='run only these steps')
    ap.add_argument('--force', nargs='+', default=[], choices=STEP_IDS, help='run these steps even if done before')
    ap.add_argument('--keep-work', action='store_true', help='keep the generated code and the objects in the temp folder')
    ap.add_argument('--lang', choices=('en', 'pt-BR'), help='default F1 menu language written to mtf.ini')
    args = ap.parse_args(argv)

    # The window cancels with Ctrl+Break to the installer's process group (CREATE_NEW_PROCESS_GROUP), which also reaches
    # the tools it runs; Python's default for it ends the process without recording anything.
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    layout = layouts.find(args.root)
    dest = args.dest.resolve()
    args.nup = args.nup.resolve() if args.nup else None
    args.content = args.content.resolve() if args.content else None
    args.firmware = args.firmware.resolve() if args.firmware else None
    args.keys = args.keys.resolve() if args.keys else None
    temp = temp_folder((args.temp or layout.temp).resolve(), dest)
    log = temp / 'logs' / time.strftime('install-%Y%m%d-%H%M%S.log')
    rep = Reporter(log_path=log)
    t0 = time.monotonic()
    try:
        state = statefile.load(dest)
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)})
        rep.emit('end', result='failed', seconds=0)
        return 1
    ctx = Context(layout, dest, temp, rep, args, state=state, forced=args.force, only=args.only)
    steps = all_steps()
    rep.emit('start', release=layout.release, mode=ctx.mode, dev=layout.dev, dest=str(dest), log=str(log),
             steps=[s.id for s in steps])
    result = run_steps(steps, ctx)
    rep.emit('end', result=result, seconds=round(time.monotonic() - t0, 1))
    rep.close()
    return EXIT[result]


class PrepareReporter(Reporter):
    """Translates installer JSON events into RecompLauncher's `progress <0..1> <text>` protocol."""

    def __init__(self, out=None, log_path=None):
        super().__init__(out=out, log_path=log_path)
        self._steps = list(STEP_IDS)
        self._step_idx = 0

    def emit(self, type, **fields):
        event = {'type': type, 't': round(self.elapsed(), 2), **fields}
        line = None
        if type == 'plan':
            self._steps = list(fields.get('run') or STEP_IDS)
            self._step_idx = 0
        elif type == 'step':
            st = fields.get('state')
            step_id = fields.get('step', '')
            if step_id in self._steps:
                self._step_idx = self._steps.index(step_id)
            n = max(1, len(self._steps))
            if st == 'started':
                line = f'progress {self._step_idx / n:.3f} {render(self.catalog, "step." + step_id)}'
            elif st in ('done', 'skipped'):
                line = f'progress {min(1.0, (self._step_idx + 1) / n):.3f} {render(self.catalog, "step." + step_id)}'
        elif type == 'progress':
            step_id = fields.get('step', '')
            phase = fields.get('phase', '')
            n = max(1, len(self._steps))
            base = self._step_idx / n
            frac = 0.0
            if fields.get('total'):
                frac = min(1.0, max(0.0, float(fields.get('done', 0)) / float(fields['total'])))
            pct = min(1.0, base + frac / n)
            text = render(self.catalog, f'phase.{step_id}.{phase}')
            line = f'progress {pct:.3f} {text}'
        elif type == 'error':
            msg = render(self.catalog, fields.get('id', ''), fields.get('params'))
            sys.stderr.write(f'{msg}\n')
            sys.stderr.flush()
        if line is not None:
            with self.lock:
                self.out.write(line + '\n')
                self.out.flush()
        self.log(self.describe(event))
        return event


def prepare_main(argv=None):
    ap = argparse.ArgumentParser(prog='MTF-Recomp-Setup --prepare')
    # Without --game the steps run from the files the last install kept in the install folder (game/updates/...), so a
    # new release can be built again after the player deleted their .nup; the check step asks for it when it's needed.
    ap.add_argument('--game', type=Path, help='path to the extracted game folder or .nup')
    ap.add_argument('--dest', type=Path, help='install folder (default: executable folder)')
    ap.add_argument('--root', type=Path, help='release folder or repository')
    args = ap.parse_args(argv)

    default_dest = Path(sys.executable).resolve().parent if getattr(sys, 'frozen', False) else Path.cwd()
    dest = (args.dest or default_dest).resolve()
    game = args.game.resolve() if args.game else None

    layout = layouts.find(args.root)
    temp = temp_folder(layout.temp.resolve(), dest)
    log = temp / 'logs' / time.strftime('prepare-%Y%m%d-%H%M%S.log')
    rep = PrepareReporter(log_path=log)
    t0 = time.monotonic()

    cli_ns = argparse.Namespace(
        nup=game if game and game.is_file() else None,
        content=game if game and game.is_dir() else None,
        firmware=None,
        keys=None,
        dest=dest,
        root=args.root,
        temp=None,
        jobs=max(1, (os.cpu_count() or 2) - 2),
        dlss=False,
        dlss_file=None,
        shaders_from=None,
        only=[],
        force=[],
        keep_work=False,
        lang=None,
    )
    try:
        state = statefile.load(dest)
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)})
        rep.close()
        return 1

    ctx = Context(layout, dest, temp, rep, cli_ns, state=state, forced=[], only=[])
    result = run_steps(all_steps(), ctx)
    if result == 'ok':
        rep.out.write('progress 1.000 Ready to play\n')
        rep.out.flush()
    rep.log(f'prepare finished: {result} ({time.monotonic() - t0:.1f}s)')
    rep.close()
    return EXIT[result]
