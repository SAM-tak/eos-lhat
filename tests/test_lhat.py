"""Exercise real SDK loading and deterministic two-client bindings in full and VM-only LÔVE."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--lovec',type=Path,default=ROOT.parent/'lhat-love/build/love/Release/lovec.exe')
p.add_argument('--vm',type=Path,default=ROOT.parent/'lhat-love/build-vmonly-shipping/love/Release/lovec.exe')
p.add_argument('--build',type=Path,default=ROOT/'build')
p.add_argument('--sdk',type=Path,default=ROOT/'EOSSDK/SDK')
p.add_argument('--config',default='Release')
a=p.parse_args()
suffix='.dll' if sys.platform=='win32' else '.dylib' if sys.platform=='darwin' else '.so'
with tempfile.TemporaryDirectory(prefix='eos-integration-') as tmp:
    root=Path(tmp)
    for library,source in [('eos_lhat','smoke.lh'),('eos_lhat_mock','integration.lh')]:
        game=root/library;game.mkdir()
        binary=next(x for x in [a.build/a.config/(library+suffix),a.build/(library+suffix)] if x.exists())
        shutil.copy2(binary,game/binary.name)
        for dep in (a.sdk/'Bin').glob('*'+suffix):shutil.copy2(dep,game/dep.name)
        shutil.copy2(ROOT/'tests'/source,game/'main.lh')
        (game/'extensions.txt').write_text(library+'\n',encoding='utf-8')
        (game/'conf.lton').write_text('window = false^, modules = {audio = false^, graphics = false^}',encoding='utf-8')
        env=dict(os.environ);env['PATH']=str(game)+os.pathsep+env.get('PATH','')
        lifetime=root/(library+'-lifetime.log')
        env['EOS_LHAT_TEST_LIFETIME']=str(lifetime)
        def run(exe,*args):
            result=subprocess.run([str(exe.resolve()),'--no-error-screen',*map(str,args)],cwd=game,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
            if result.returncode:raise RuntimeError(result.stdout+result.stderr)
            print(result.stdout.strip())
        run(a.lovec,game)
        compiled=root/(library+'-compiled')
        run(a.lovec,'--compile-game',compiled,game)
        run(a.vm,compiled)
        if library.endswith('_mock'):
            assert lifetime.read_text().splitlines() == ['1 1 0', '1 1 0'], 'SDK shutdown must run exactly once after all platforms'
        shutil.copy2(ROOT/'tests/restart.lh',game/'main.lh')
        run(a.lovec,game)
        restarted=root/(library+'-restart')
        run(a.lovec,'--compile-game',restarted,game)
        run(a.vm,restarted)
        if library.endswith('_mock'):
            assert lifetime.read_text().splitlines() == ['1 1 0'] * 4, 'Restart must preserve the initialized SDK'
    game=root/'example';shutil.copytree(ROOT/'examples/love-p2p',game,ignore=shutil.ignore_patterns('native','config.lton','steam-ticket.txt'))
    (game/'native').mkdir(exist_ok=True)
    shutil.copy2(binary,game/'native'/('eos_lhat'+suffix))
    shutil.copy2(game/'config.example.lton',game/'config.lton')
    run(a.lovec,'--compile-game',root/'example-compiled',game)
print('PASS: full and VM-only, real SDK smoke, mocked multiplayer, example compilation')
