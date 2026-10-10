"""Test EOS bindings and whole-Program restarts using only full and VM-only L^."""
import argparse
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def executable(build, config):
    return next(x for x in [build / config / 'eos_lhat_host.exe', build / 'eos_lhat_host'] if x.exists())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', type=Path, help='Full tooling host (defaults to build/tools)')
    p.add_argument('--vm', type=Path, help='VM-only tooling host (built automatically if omitted)')
    p.add_argument('--lhat', type=Path, default=ROOT.parent / 'lhat')
    p.add_argument('--build', type=Path, default=ROOT / 'build')
    p.add_argument('--sdk', type=Path, default=ROOT / 'EOSSDK/SDK')
    p.add_argument('--config', default='Release')
    p.add_argument('--installed', type=Path, help='Test an installed real extension with its SDK placed beside it')
    a = p.parse_args()
    a.build = a.build.resolve()
    host = (a.host or executable(a.build / 'tools', a.config)).resolve()
    if a.vm is None:
        vm_build = a.build / 'vm-host'
        subprocess.run(['cmake', '-S', str(ROOT / 'tools'), '-B', str(vm_build),
                        '-DLHAT_ROOT=' + str(a.lhat.resolve()), '-DLHAT_WITH_FRONTEND=OFF',
                        '-DCMAKE_BUILD_TYPE=' + a.config], check=True)
        subprocess.run(['cmake', '--build', str(vm_build), '--config', a.config,
                        '--target', 'eos_lhat_host'], check=True)
        a.vm = executable(vm_build, a.config)
    vm = a.vm.resolve()
    suffix = '.dll' if sys.platform == 'win32' else '.dylib' if sys.platform == 'darwin' else '.so'
    with tempfile.TemporaryDirectory(prefix='eos-integration-') as tmp:
        root = Path(tmp)
        env = dict(os.environ)
        loader_path = 'PATH' if sys.platform == 'win32' else 'DYLD_LIBRARY_PATH' if sys.platform == 'darwin' else 'LD_LIBRARY_PATH'
        env[loader_path] = str((a.sdk / 'Bin').resolve()) + os.pathsep + env.get(loader_path, '')
        for library, source in [('eos_lhat', 'smoke.lh'), ('eos_lhat_mock', 'integration.lh')]:
            binary = next(x for x in [a.build / a.config / (library + suffix),
                                      a.build / (library + suffix)] if x.exists())
            work = root / library
            work.mkdir()
            if a.installed:
                # Relocate outside the build tree and remove SDK search overrides.
                source_binary = a.installed.resolve() if library == 'eos_lhat' else binary
                binary = work / source_binary.name
                shutil.copy2(source_binary, binary)
                for dep in (a.sdk / 'Bin').glob('*' + suffix):
                    shutil.copy2(dep, work / dep.name)
                env = dict(os.environ)
                env.pop('LD_LIBRARY_PATH', None)
                env.pop('DYLD_LIBRARY_PATH', None)
            lifetime = work / 'lifetime.log'
            env['EOS_LHAT_TEST_LIFETIME'] = str(lifetime)

            def run(exe, *args, env=env):
                result = subprocess.run([str(exe), *map(str, args)], cwd=work, env=env,
                                        capture_output=True, text=True, encoding='utf-8',
                                        errors='replace', timeout=60)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print(result.stdout.strip())

            for source_name, repetitions in [(source, 1), ('restart.lh', 3)]:
                text = ROOT / 'tests' / source_name
                compiled = work / (source_name + '.bin')
                run(host, 'run', binary, text, repetitions)
                run(host, 'compile', binary, text, compiled)
                run(vm, 'run', binary, compiled, repetitions)
            # A host without std.binary gets the string^ arms only.
            plain = dict(env, EOS_LHAT_NO_BINARY='1')
            plain.pop('EOS_LHAT_TEST_LIFETIME', None)
            compiled = work / 'plain.bin'
            run(host, 'run', binary, ROOT / 'tests' / 'smoke.lh', env=plain)
            run(host, 'compile', binary, ROOT / 'tests' / 'smoke.lh', compiled, env=plain)
            run(vm, 'run', binary, compiled, env=plain)
            if library.endswith('_mock'):
                # One SDK lifetime per process, zero live platforms at shutdown;
                # all three restarted Programs must have released their clients.
                assert lifetime.read_text().splitlines() == [
                    '1 1 0 3', '1 1 0 3', '1 1 0 3', '1 1 0 3'
                ], 'Incorrect SDK/platform lifecycle across Program restarts'
    print('PASS: full and VM-only L^, real SDK smoke, mocked multiplayer, Program restarts')


if __name__ == '__main__':
    main()
