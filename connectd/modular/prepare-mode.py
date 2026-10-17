"""Emit read-only firmware hashes; never embed firmware bytes in the receiver."""
from pathlib import Path
import os
import struct

root=Path(__file__).resolve().parent
modules=Path(os.environ.get('PS5_FIRMWARE_MODULES',root.parents[2]/'spotifyPS5/research/sysmodules'))
committed=root/'ownership/firmware-1320.inc'
out=root/'ownership/build'
if not (modules/'SystemMusicCore.elf').is_file():
    # Without the console's modules (for example on GitHub), use the hashes recorded from them.
    out.mkdir(parents=True,exist_ok=True)
    (out/'mode-firmware.inc').write_bytes(committed.read_bytes())
    print('Using the recorded firmware 13.20 code hashes')
    raise SystemExit
rows=[]
for label,name,offsets in [('core','SystemMusicCore.elf',[0x9db0,0x12090,0x31f00]),
                           ('server','mediacoreserver.elf',[0x19d00,0x1a320,0x19b70])]:
    data=(modules/name).read_bytes()
    assert data[:6]==b'\x7fELF\x02\x01'
    entry,phoff=struct.unpack_from('<QQ',data,24)
    phsize,count=struct.unpack_from('<HH',data,54)
    segments=[struct.unpack_from('<IIQQQQQQ',data,phoff+i*phsize) for i in range(count)]
    checks=[]
    for address in offsets:
        blocks=[data[o+address-v:o+address-v+32] for t,f,o,v,_,n,_,_ in segments if t==1 and f&1 and v<=address and address+32<=v+n]
        assert len(blocks)==1 and len(blocks[0])==32
        h=14695981039346656037
        for b in blocks[0]:h=((h^b)*1099511628211)&((1<<64)-1)
        checks.append(f'{{0x{address:x},0x{h:016x}ULL}}')
    rows.append(f'constexpr std::uint64_t {label}_entry=0x{entry:x};\nconstexpr core_state::Fingerprint {label}_checks[]={{'+','.join(checks)+'};\n')
assert committed.read_text()==''.join(rows),'firmware-1320.inc no longer matches these modules'
out.mkdir(parents=True,exist_ok=True)
(out/'mode-firmware.inc').write_text(''.join(rows),newline='\n')
print('Prepared code hashes for guarded native AudioTag observations')
