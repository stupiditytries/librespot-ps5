"""Stage the USB-free PCM receiver or inspect its logs; payload launch is manual."""
from argparse import ArgumentParser
from ftplib import FTP, error_perm
from pathlib import Path
from io import BytesIO
import importlib.util
import hashlib
import json
import re
import struct
import time

ROOT=Path(__file__).resolve().parent
REMOTE='/data/spotifyps5/pcm-transport-v12'
NAME='librespot-ps5-modular.elf'
spec=importlib.util.spec_from_file_location('pcm_previous_console',ROOT.parent/'pcm-transport/console.py')
helpers=importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

def optional(f,path):
    try:return helpers.read(f,path)
    except KeyError:return b''
    except error_perm as e:
        if str(e).startswith('550'):return b''
        raise

def stage(f):
    candidates=[]
    for mount in helpers.entries(f,'/mnt'):
        if not re.fullmatch('usb[0-7]',mount):continue
        try:files=helpers.entries(f,'/mnt/'+mount)
        except error_perm:continue
        if 'librespot-ps5.elf' in files:candidates.append(('/mnt/'+mount,files))
    if len(candidates)!=1:raise RuntimeError('Expected one USB containing librespot-ps5.elf')
    usb,before=candidates[0]
    if NAME in before or NAME+'.partial' in before:raise RuntimeError('Candidate already exists; inspect before replacing')
    data=(ROOT.parents[1]/'build'/NAME).read_bytes()
    if not data.startswith(b'\x7fELF'):raise RuntimeError('Candidate is not ELF')
    digest=helpers.upload(f,data,usb+'/'+NAME,False)
    after=helpers.entries(f,usb)
    for name,metadata in before.items():
        if after.get(name)!=metadata:raise RuntimeError('Existing USB metadata changed: '+name)
    record=dict(target=usb+'/'+NAME,size=len(data),local_sha256=digest,
        verification='LIST size only; no upload read-back',existing_usb_metadata_unchanged=True,
        service_file_required=False,launched=False)
    (ROOT/'build/deployment.json').write_text(json.dumps(record,indent=2))
    print(json.dumps(record,indent=2))

def state(f):
    s=optional(f,REMOTE+'/status.bin')
    if len(s)==96:
        values=struct.unpack_from('<5iI',s,32)
        frames,underruns,generation,checked=struct.unpack_from('<4Q',s,56)
        print(json.dumps(dict(state=values[0],source=values[1],rc=values[2],detail=values[3],port=values[4],
            queued_frames=values[5],audio_frames=frames,underruns=underruns,generation=generation)))
    else:print('No integrated PCM run yet; launch manually and play from Spotify.')
    boot=optional(f,REMOTE+'/bootstrap-status.bin')
    if len(boot)==40:
        signature,token,at,phase,source,rc,clean=struct.unpack('<QQQiiii',boot)
        print(json.dumps(dict(bootstrap_phase=phase,bootstrap_source=source,bootstrap_rc=rc,bootstrap_cleanup=clean)))
    feature=optional(f,REMOTE+'/feature-status.bin')
    if len(feature)==40:
        signature,token,at,phase,set_rc,restore_rc,verified=struct.unpack('<QQQIiiI',feature)
        print(json.dumps(dict(feature_phase=phase,set_rc=set_rc,restore_rc=restore_rc,restoration_verified=verified)))

def main():
    parser=ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('action',choices=['stage','status','logs','stop'])
    parser.add_argument('--port',type=int,default=2121)
    a=parser.parse_args()
    (ROOT/'build').mkdir(exist_ok=True)
    with FTP() as f:
        f.connect(a.host,a.port,timeout=30);f.login('anonymous','')
        if a.action=='stage':stage(f)
        elif a.action=='stop':
            text=f'usbfree-pcm-quit {time.time_ns()}\n'.encode()
            f.storbinary('STOR /data/spotifyps5/quit.txt',BytesIO(text))
            print('Requested receiver shutdown through quit.txt; check logs for SHUTDOWN CONFIRMED.')
        else:
            state(f)
            if a.action=='logs':
                for name,path in [('parent.log',REMOTE+'/parent.log'),('worker.log',REMOTE+'/worker.log'),('connect.log','/data/spotifyps5/connect.log')]:
                    data=optional(f,path)
                    lines=data.decode(errors='replace').splitlines()
                    if name=='connect.log':
                        lines=[line for line in lines if any(word in line for word in ['PCM','transport','library:','audio output','Sink','quit requested','receiver returned','starting','previous native worker','another copy'])
                            and not any(secret in line.lower() for secret in ['authorization','bearer','access_token','auth_data'])]
                        data=('\n'.join(lines)+'\n').encode()
                    (ROOT/'build'/name).write_bytes(data)
                    print(name+':\n'+'\n'.join(lines[-70:]))
                for name in ['resident.bin','request.bin','status.bin','heartbeat.bin','feature-plan.bin','feature-status.bin','feature-ack.bin','retired.bin','prior.bin','inactive-sources.bin','bootstrap.bin','bootstrap-status.bin','endpoint.bin']:
                    data=optional(f,REMOTE+'/'+name)
                    if data:(ROOT/'build'/name).write_bytes(data)

if __name__=='__main__':main()
