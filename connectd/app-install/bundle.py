#!/usr/bin/env python3
"""Embed the completed folder app; only the app manifest is used by the installer."""
import hashlib
import sys
from pathlib import Path
app=Path(sys.argv[1]).resolve()
out=Path(sys.argv[2]).resolve()
files=sorted(p for p in app.rglob('*') if p.is_file() and p.name!='.player-lock')
assert (app/'eboot.bin').is_file() and (app/'sce_sys/param.json').is_file()
digest=hashlib.sha256()
out.mkdir(parents=True,exist_ok=True)
with (out/'app_bundle.c').open('w',newline='\n') as source:
    source.write('#include "app_bundle.h"\n')
    for n,p in enumerate(files):
        name=p.relative_to(app).as_posix();assert all(c.isalnum() or c in '/._-' for c in name)
        data=p.read_bytes();digest.update(name.encode()+b'\0'+data)
        source.write(f'static const unsigned char file_{n}[]={{\n')
        for start in range(0,len(data),24):source.write(','.join(str(b) for b in data[start:start+24])+',\n')
        source.write('};\n')
    source.write('const struct app_file librespot_app_files[]={\n')
    for n,p in enumerate(files):source.write(f'{{"{p.relative_to(app).as_posix()}",file_{n},sizeof file_{n}}},\n')
    source.write('};\nconst unsigned librespot_app_file_count=sizeof librespot_app_files/sizeof *librespot_app_files;\n')
    source.write(f'const char librespot_app_version[]="{digest.hexdigest()}";\n')
(out/'app_bundle.h').write_text('#pragma once\n#include <stddef.h>\nstruct app_file{const char* name;const unsigned char* data;size_t size;};\nextern const struct app_file librespot_app_files[];\nextern const unsigned librespot_app_file_count;\nextern const char librespot_app_version[];\n',newline='\n')
print('Bundled',len(files),'app files;',sum(p.stat().st_size for p in files),'bytes')
