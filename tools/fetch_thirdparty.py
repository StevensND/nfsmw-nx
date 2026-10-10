"""Fetches the third-party sources of the SDK into sdk/thirdparty.

sdk/thirdparty only carries the files this port changed (see THIRD_PARTY_NOTICES.md). Everything else comes from the
ReXGlue SDK release this port is based on and its submodules. This script clones that release with its submodules
into a temporary folder and copies every file that sdk/thirdparty does not have yet; files already there are kept.

The deko3d renderer (docs/HowToDeko3d.md) needs two more, fetched the same way at fixed commits:
  - SPIRV-Cross, which turns the deko3d shader library's SPIR-V into GLSL (sdk/thirdparty/SPIRV-Cross);
  - uam, devkitPro's GLSL to DKSH compiler, as SwitchWakerHD vendors it: built with CMake as a library, with the
    seven patches of its PATCHES.md (sdk/thirdparty/uam).

Usage: python tools/fetch_thirdparty.py [--deko3d]
  --deko3d   only the two deko3d sources, not the SDK's
Needs git.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

UPSTREAM = 'https://github.com/rexglue/rexglue-sdk.git'
COMMIT = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'  # v0.10.0

# (repository, commit, folder inside it or '' for all of it, folder under sdk/thirdparty)
DEKO3D = [
    ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7',  # vulkan-sdk-1.4.363.0
     '', 'SPIRV-Cross'),
    ('https://github.com/centollOS/SwitchWakerHD.git', '2d147aaaa27b1ad0f9b390eff515609af9fe24a3',  # uam 1.1.0 + 7 patches
     'runtime/third_party/uam', 'uam'),
]


def run(*args, cwd=None):
    print('>', ' '.join(args))
    subprocess.run(args, cwd=cwd, check=True)


def remove_readonly(func, path, _):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def copy_missing(source, target):
    """Copies every file of source that target does not have yet. Returns (copied, kept)."""
    copied = kept = 0
    for dirpath, dirnames, filenames in os.walk(source):
        dirnames[:] = [d for d in dirnames if d != '.git']
        for name in filenames:
            if name == '.git':
                continue
            src = os.path.join(dirpath, name)
            dst = os.path.join(target, os.path.relpath(src, source))
            if os.path.lexists(dst):
                kept += 1
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # Links stay links: some submodules (moltenvk) have links that point nowhere on Linux and macOS,
            # where git checks them out as real links.
            shutil.copy2(src, dst, follow_symlinks=False)
            copied += 1
    return copied, kept


def checkout(work, url, commit, folder=''):
    run('git', 'init', '-q', work)
    run('git', 'remote', 'add', 'origin', url, cwd=work)
    if folder:
        # Only that folder: a partial clone without the other blobs, and a sparse checkout.
        run('git', 'sparse-checkout', 'set', folder, cwd=work)
        run('git', 'fetch', '-q', '--depth', '1', '--filter=blob:none', 'origin', commit, cwd=work)
    else:
        run('git', 'fetch', '-q', '--depth', '1', 'origin', commit, cwd=work)
    run('git', 'checkout', '-q', 'FETCH_HEAD', cwd=work)


def remove_tree(path):
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=remove_readonly)
    else:
        shutil.rmtree(path, onerror=remove_readonly)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, 'sdk', 'thirdparty')
    only_deko3d = '--deko3d' in sys.argv[1:]
    if not only_deko3d:
        work = tempfile.mkdtemp(prefix='rexglue-sdk-')
        try:
            checkout(work, UPSTREAM, COMMIT)
            run('git', 'submodule', 'update', '--init', '--recursive', cwd=work)
            copied, kept = copy_missing(os.path.join(work, 'thirdparty'), target)
            print(f'{copied} files copied into sdk/thirdparty, {kept} files of this port kept')
        finally:
            remove_tree(work)
    for url, commit, folder, name in DEKO3D:
        work = tempfile.mkdtemp(prefix='deko3d-')
        try:
            checkout(work, url, commit, folder)
            copied, kept = copy_missing(os.path.join(work, folder), os.path.join(target, name))
            print(f'{copied} files copied into sdk/thirdparty/{name}, {kept} kept')
        finally:
            remove_tree(work)


if __name__ == '__main__':
    main()
