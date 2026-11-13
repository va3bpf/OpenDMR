#!/usr/bin/env python3
"""Makes an OpenDMR release: builds the firmware, packs what docs/installation.md lists into
dist/OpenDMR-<version>.zip (the folder layout the tools expect) with our CPS (opendmr-cps.html, the latest release of
va3bpf/opendmr-cps), puts the CPS and SHA256SUMS.txt next to it, and with --publish makes the release on GitHub (the gh
CLI, logged in: gh auth login) and uploads them. The OpenGD77 CPS is not in it: users download it from opengd77.com.

  py tools/release/make_release.py                      build and pack (version: date and commit, 2026.11.07-6584c27)
  py tools/release/make_release.py --version 0.1        a version of your own
  py tools/release/make_release.py --no-build           pack src/build/opendmr.bin as it is (built from HEAD)
  py tools/release/make_release.py --publish            also the GitHub release, tag v<version> on HEAD: on main only,
                                                        pushed first (develop is for merging, main for releases)
        --notes FILE  the release text (Markdown; default: the commits since the last v* tag)
        --draft / --prerelease   --repo va3bpf/OpenDMR
        --cps FILE    this opendmr-cps.html instead of the one of the latest CPS release

The working tree must be clean (the firmware shows the commit it was built from); --dirty packs it anyway, not with
--publish.
"""
import argparse
import datetime
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DIST = os.path.join(ROOT, 'dist')
DEFAULT_REPO = 'va3bpf/OpenDMR'
CPS_REPO = 'va3bpf/opendmr-cps'
CPS_FILE = 'opendmr-cps.html'
FONT = 'tools/fonts/build/cjk.bin'

# In the zip -> from the repository. Keep in step with the table of docs/installation.md
FILES = {
    'opendmr.bin': 'src/build/opendmr.bin',
    'opendmr_BFUV32.bin': 'src/build/opendmr_BFUV32.bin',
    'cjk.bin': FONT,
    'tools/backup/dm32_dump.py': 'tools/backup/dm32_dump.py',
    'tools/backup/dumper/dumper.bin': 'tools/backup/dumper/dumper.bin',
    'tools/fonts/ttf/RobotoCondensed-VF.ttf': 'tools/fonts/ttf/RobotoCondensed-VF.ttf',
    'tools/fonts/upload_cjk.py': 'tools/fonts/upload_cjk.py',
    'tools/debug/fw_update.py': 'tools/debug/fw_update.py',
    'tools/debug/updater/updater.bin': 'tools/debug/updater/updater.bin',
    'licences/OFL-RobotoCondensed.txt': 'tools/fonts/ttf/OFL-RobotoCondensed.txt',  # the fonts are OFL: their
    'licences/OFL-NotoSansSC.txt': 'tools/fonts/ttf/OFL-NotoSansSC.txt',            # licence goes with them
    'LICENSE': 'LICENSE',
    'README.md': 'README.md',
    'README.en.md': 'README.en.md',
    'docs/installation.md': 'docs/installation.md',
    'docs/installation.en.md': 'docs/installation.en.md',
    'docs/manual.md': 'docs/manual.md',
    'docs/manual.en.md': 'docs/manual.en.md',
}
FOLDERS = {'docs/manual': 'docs/manual'}  # the screenshots of the manual


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def build():
    tools = os.path.join(ROOT, 'toolchain', 'MinGW')
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join([os.path.join(tools, 'csky-elfabiv2-tools', 'bin'),
                                   os.path.join(tools, 'msys', '1.0', 'bin'), env.get('PATH', '')])
    make = shutil.which('make', path=env['PATH'])  # Windows looks a program up on the parent's PATH otherwise
    if make is None:
        sys.exit('make not found: the toolchain goes into toolchain/ (README, "Building")')
    print('building the firmware ...')
    r = subprocess.run([make], cwd=os.path.join(ROOT, 'src'), env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stdout[-3000:] + r.stderr[-3000:] + '\nthe build failed')
    print(r.stdout.strip().splitlines()[-1])


def font():
    if not os.path.exists(os.path.join(ROOT, FONT)):
        print('making the Chinese font ...')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'fonts', 'mkcjk.py')], cwd=ROOT, check=True)


def gh_path():
    gh = shutil.which('gh') or shutil.which('gh', path=r'C:\Program Files\GitHub CLI')
    if gh is None:
        sys.exit('the GitHub CLI (gh) is not installed: winget install GitHub.cli, then gh auth login')
    return gh


def cps(args):
    """Our CPS, one HTML file: the asset of the latest release of va3bpf/opendmr-cps (or --cps); (path, version)"""
    if args.cps:
        return args.cps, 'local file'
    folder = os.path.join(DIST, 'cps')
    os.makedirs(folder, exist_ok=True)
    gh = gh_path()
    tag = subprocess.run([gh, 'release', 'view', '--repo', CPS_REPO, '--json', 'tagName', '-q', '.tagName'], cwd=ROOT,
                         capture_output=True, text=True)
    if tag.returncode != 0:
        sys.exit('gh: %s' % tag.stderr.strip())
    r = subprocess.run([gh, 'release', 'download', tag.stdout.strip(), '--repo', CPS_REPO, '--pattern', CPS_FILE,
                        '--dir', folder, '--clobber'], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('the CPS could not be downloaded: %s' % r.stderr.strip())
    print('CPS %s' % tag.stdout.strip())
    return os.path.join(folder, CPS_FILE), tag.stdout.strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def pack(version, cps_path, cps_version):
    os.makedirs(DIST, exist_ok=True)
    name = 'OpenDMR-%s' % version
    zip_path = os.path.join(DIST, name + '.zip')
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        for inside, source in FILES.items():
            path = os.path.join(ROOT, source)
            if not os.path.exists(path):
                sys.exit('missing: %s' % source)
            z.write(path, '%s/%s' % (name, inside))
        for inside, source in FOLDERS.items():
            for folder, _, files in os.walk(os.path.join(ROOT, source)):
                for f in sorted(files):
                    path = os.path.join(folder, f)
                    rel = os.path.relpath(path, os.path.join(ROOT, source)).replace(os.sep, '/')
                    z.write(path, '%s/%s/%s' % (name, inside, rel))
        z.write(cps_path, '%s/%s' % (name, CPS_FILE))
        z.writestr('%s/VERSION.txt' % name, 'OpenDMR %s\ncommit %s\nbuilt %s\nCPS %s\n' % (
            version, git('rev-parse', 'HEAD'), datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), cps_version))
    cps_asset = os.path.join(DIST, CPS_FILE)
    if os.path.abspath(cps_path) != os.path.abspath(cps_asset):
        shutil.copyfile(cps_path, cps_asset)
    assets = [zip_path, cps_asset]
    sums = os.path.join(DIST, 'SHA256SUMS.txt')
    with open(sums, 'w', newline='\n') as f:
        for a in assets:
            f.write('%s  %s\n' % (sha256(a), os.path.basename(a)))
    assets.append(sums)
    for a in assets:
        print('%10d  %s' % (os.path.getsize(a), os.path.relpath(a, ROOT)))
    return assets


def notes_since_last_tag(version):
    tags = [t for t in git('tag', '--list', 'v*', '--sort=-creatordate').splitlines() if t != 'v' + version]
    span = ('%s..HEAD' % tags[0]) if tags else 'HEAD'
    log = git('log', '--no-merges', '--format=- %s', span).splitlines()
    head = 'Since %s:' % tags[0] if tags else 'Changes:'
    return '\n'.join(['OpenDMR %s (commit %s).' % (version, git('rev-parse', '--short', 'HEAD')), '',
                      'Installing: docs/installation.md (中文) / docs/installation.en.md (English), in the zip too.',
                      '', head] + log[:100])


def publish(args, version, assets):
    gh = gh_path()
    head = git('rev-parse', 'HEAD')
    # Releases come from main only (develop is where the work is merged): HEAD must be what github.com has as main
    # (whatever the local branch is called)
    pushed = subprocess.run(['git', 'ls-remote', 'https://github.com/%s.git' % args.repo, 'refs/heads/main'], cwd=ROOT,
                            capture_output=True, text=True).stdout.split()
    if not pushed or pushed[0] != head:
        sys.exit('HEAD is not main of github.com/%s: merge develop into main, push it, and run this on it' % args.repo)
    remote = subprocess.run([gh, 'repo', 'view', args.repo, '--json', 'defaultBranchRef', '-q', '.defaultBranchRef.name'],
                            cwd=ROOT, capture_output=True, text=True)
    if remote.returncode != 0:
        sys.exit('gh: %s' % remote.stderr.strip())
    notes = os.path.join(DIST, 'NOTES.md')
    with open(notes, 'w', encoding='utf-8') as f:
        f.write(open(args.notes, encoding='utf-8').read() if args.notes else notes_since_last_tag(version))
    cmd = [gh, 'release', 'create', 'v' + version, *assets, '--repo', args.repo, '--target', head,
           '--title', 'OpenDMR ' + version, '--notes-file', notes]
    if args.draft:
        cmd.append('--draft')
    if args.prerelease:
        cmd.append('--prerelease')
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        sys.exit('gh release create failed (is HEAD pushed to %s?)' % args.repo)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', help='default: the date and the commit')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--dirty', action='store_true', help='pack a working tree with changes (not with --publish)')
    ap.add_argument('--publish', action='store_true')
    ap.add_argument('--repo', default=DEFAULT_REPO)
    ap.add_argument('--notes')
    ap.add_argument('--cps', help='opendmr-cps.html to pack (default: the latest CPS release)')
    ap.add_argument('--draft', action='store_true')
    ap.add_argument('--prerelease', action='store_true')
    args = ap.parse_args()

    if git('status', '--porcelain', '--untracked-files=no', '--ignore-submodules=none'):
        if args.publish or not args.dirty:
            sys.exit('the working tree has changes: commit them first (or --dirty, without --publish)')
    version = args.version or '%s-%s' % (datetime.date.today().strftime('%Y.%m.%d'),
                                         git('rev-parse', '--short=7', 'HEAD'))
    if not args.no_build:
        build()
    font()
    cps_path, cps_version = cps(args)
    assets = pack(version, cps_path, cps_version)
    if args.publish:
        publish(args, version, assets)


if __name__ == '__main__':
    main()
