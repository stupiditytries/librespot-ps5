#!/usr/bin/env python3
"""Exercise closed-app updates, lock exclusion and missing-file repair."""
from pathlib import Path
import ctypes,fcntl,subprocess,tempfile
repo=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='player-install-') as directory:
    work=Path(directory);root=work/'app'
    (work/'app_bundle.h').write_text('#include <stddef.h>\nstruct app_file{const char* name;const unsigned char* data;size_t size;};\nextern const struct app_file librespot_app_files[];extern const unsigned librespot_app_file_count;extern const char librespot_app_version[];\n')
    (work/'bundle.c').write_text('#include "app_bundle.h"\nstatic const unsigned char data[]={1,2,3,4};const struct app_file librespot_app_files[]={{"eboot.bin",data,4},{"sce_sys/param.json",data,4},{"sce_module/libc.prx",data,4}};const unsigned librespot_app_file_count=3;const char librespot_app_version[]="test-version";\n')
    library=work/'installer.so'
    subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE','-shared','-fPIC',f'-DAPP_ROOT="{root}"','-I',str(work),str(repo/'install.c'),str(work/'bundle.c'),'-o',str(library)],check=True)
    install=ctypes.CDLL(str(library)).librespot_install_app
    assert install()==0
    assert (root/'eboot.bin').read_bytes()==bytes([1,2,3,4])
    assert install()==0
    assert (root/'eboot.bin').stat().st_mode & 0o777 == 0o755
    assert (root/'sce_module/libc.prx').stat().st_mode & 0o777 == 0o755
    assert (root/'sce_sys/param.json').stat().st_mode & 0o777 == 0o644
    (root/'eboot.bin').chmod(0o644)
    (root/'sce_module/libc.prx').chmod(0o644)
    assert install()==0
    assert (root/'eboot.bin').stat().st_mode & 0o777 == 0o755
    assert (root/'sce_module/libc.prx').stat().st_mode & 0o777 == 0o755
    with (root/'.player-lock').open('rb') as lock:
        fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
        (root/'.librespot-version').write_text('old-version')
        old_eboot=(root/'eboot.bin').open('rb')
        assert install()==1
        assert (root/'.librespot-version').read_text()=='test-version'
        assert old_eboot.read()==bytes([1,2,3,4])
        old_eboot.close()
        assert install()==0 # an already-current bundle needs no reopen warning
    (root/'sce_sys/param.json').unlink()
    assert install()==0 and (root/'sce_sys/param.json').stat().st_size==4
    (root/'eboot.bin').write_bytes(b'bad')
    assert install()==0 and (root/'eboot.bin').read_bytes()==bytes([1,2,3,4])
    print('App installer permissions, atomic open-app update and repair checks passed.')
