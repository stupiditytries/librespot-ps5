"""Install or inspect the separate PCM receiver. Never launches an ELF."""
from argparse import ArgumentParser
from ftplib import FTP,error_perm
from pathlib import Path
from io import BytesIO
import hashlib,json,re,struct,time
REMOTE='/data/spotifyps5/pcm-transport-v6'
NAME='librespot-ps5-pcm-test-v8.elf'
FOLDER='/Music/librespot-ps5 Service'
SERVICE='librespot-ps5 Service.flac'
def entries(f,path):
    rows=[];f.retrlines('LIST '+path,rows.append)
    return {p[8]:tuple(p[4:8]) for row in rows if len(p:=row.split(None,8))==9 and p[8] not in ('.','..')}
def read(f,path):
    folder,name=path.rsplit('/',1);size=int(entries(f,folder)[name][0]);data=bytearray()
    with f.transfercmd('RETR '+path) as s:
        while len(data)<size:
            b=s.recv(min(262144,size-len(data)))
            if not b:raise RuntimeError('Short read: '+path)
            data.extend(b)
    f.voidresp();return bytes(data)
def optional(f,path):
    try:return read(f,path)
    except (KeyError,error_perm):return b''
def mkdir(f,path):
    try:f.mkd(path)
    except error_perm as e:
        if not str(e).startswith('550'):raise
def upload(f,data,path,verify=False):
    expected=hashlib.sha256(data).hexdigest()
    print('Uploading',path,len(data),'bytes',flush=True)
    sent=reported=0;began=time.monotonic()
    def progress(block):
        nonlocal sent,reported
        sent+=len(block)
        if sent-reported>=8*1024*1024:
            print('Uploaded',sent,'bytes in',round(time.monotonic()-began,1),'seconds',flush=True);reported=sent
    f.storbinary('STOR '+path+'.partial',BytesIO(data),blocksize=65536,callback=progress)
    f.rename(path+'.partial',path)
    actual=hashlib.sha256();size=int(entries(f,path.rsplit('/',1)[0])[path.rsplit('/',1)[1]][0]);done=0;reported=0
    if size!=len(data):raise RuntimeError('Uploaded size mismatch')
    # Full read-back doubles the transfer and has never caught an error; opt-in only.
    if not verify:print('Size verified',size,flush=True);return expected
    with f.transfercmd('RETR '+path) as s:
        while done<size:
            b=s.recv(min(262144,size-done))
            if not b:raise RuntimeError('Short verify read')
            actual.update(b);done+=len(b)
            if done-reported>=8*1024*1024:print('Verified',done,'bytes',flush=True);reported=done
    f.voidresp()
    if actual.hexdigest()!=expected:raise RuntimeError('Read-back SHA256 mismatch')
    print('SHA256 verified',expected,flush=True);return expected
def main():
    p=ArgumentParser(description=__doc__);p.add_argument('host');p.add_argument('action',choices=['install','stage','status','logs']);p.add_argument('--port',type=int,default=2121);p.add_argument('--verify',action='store_true',help='read the upload back and compare SHA256')
    a=p.parse_args();root=Path(__file__).resolve().parents[2];build=root/'build'
    with FTP() as f:
        f.connect(a.host,a.port,timeout=30);f.login()
        if a.action in ('install','stage'):
            candidates=[]
            for mount in entries(f,'/mnt'):
                if not re.fullmatch('usb[0-7]',mount):continue
                try:files=entries(f,'/mnt/'+mount)
                except error_perm:continue
                if 'librespot-ps5.elf' in files:candidates.append(('/mnt/'+mount,files))
            if len(candidates)!=1:raise RuntimeError('Cannot identify one receiver USB by contents')
            usb,before=candidates[0];print('Discovered USB',usb,flush=True)
            if a.action=='stage':
                if NAME in before:raise RuntimeError('Stage requires a new ELF name; candidate already exists')
                elf=(build/NAME).read_bytes()
                if not elf.startswith(b'\x7fELF'):raise RuntimeError('Candidate is not an ELF')
                elf_hash=upload(f,elf,usb+'/'+NAME,a.verify)
                after=entries(f,usb)
                if any(after.get(name)!=metadata for name,metadata in before.items()):raise RuntimeError('Existing USB file metadata changed')
                record={'usb':usb,'elf':NAME,'elf_bytes':len(elf),'local_sha256':elf_hash,'verification':'sha256' if a.verify else 'size','launched':False,'service_modified':False}
                (build/'pcm-v8-deployment.json').write_text(json.dumps(record,indent=2),encoding='utf8')
                print(json.dumps(record),flush=True)
                return
            for runtime in ['/data/spotifyps5/pcm-transport','/data/spotifyps5/pcm-transport-v2','/data/spotifyps5/pcm-transport-v3','/data/spotifyps5/pcm-transport-v4','/data/spotifyps5/pcm-transport-v5',REMOTE]:
                status=optional(f,runtime+'/status.bin')
                if len(status)==96:
                    state,source,rc,detail,port=struct.unpack_from('<5i',status,32)
                    if state in (1,2,4,5) or source>0 or port>=0:raise RuntimeError('PCM cleanup unconfirmed: '+runtime)
            elf=(build/NAME).read_bytes();service=(build/SERVICE).read_bytes()
            assert elf.startswith(b'\x7fELF') and service.startswith(b'fLaC')
            mkdir(f,usb+'/Music');mkdir(f,usb+FOLDER)
            if set(entries(f,usb+FOLDER))-{SERVICE,SERVICE+'.partial'}:raise RuntimeError('Service folder contains other files')
            service_path=usb+FOLDER+'/'+SERVICE
            expected_service=hashlib.sha256(service).hexdigest()
            listed=entries(f,usb+FOLDER).get(SERVICE)
            if listed and int(listed[0])==len(service) and not a.verify:
                service_hash=expected_service;print('Existing Service file size matches; preserved',flush=True)
            elif a.verify and (present:=optional(f,service_path)) and hashlib.sha256(present).hexdigest()==expected_service:
                service_hash=expected_service;print('Existing Service file read-back verified; preserved',flush=True)
            else:service_hash=upload(f,service,service_path,a.verify)
            elf_hash=upload(f,elf,usb+'/'+NAME,a.verify)
            after=entries(f,usb)
            for name,metadata in before.items():
                if name in ('librespot-ps5.elf','librespot-ps5-autostart.elf','librespot-ps5-selection-test.elf','librespot-ps5-pcm-test.elf','librespot-ps5-pcm-test-v2.elf','librespot-ps5-pcm-test-v3.elf','librespot-ps5-pcm-test-v4.elf','librespot-ps5-pcm-test-v5.elf','librespot-ps5-pcm-test-v6.elf','librespot-ps5-pcm-test-v7.elf') and after.get(name)!=metadata:raise RuntimeError('Existing ELF metadata changed: '+name)
            record={'usb':usb,'elf':NAME,'elf_bytes':len(elf),'elf_sha256':elf_hash,'service':FOLDER+'/'+SERVICE,
                    'service_bytes':len(service),'service_sha256':service_hash,'launched':False}
            (build/'pcm-v8-deployment.json').write_text(json.dumps(record,indent=2),encoding='utf8')
            print(json.dumps(record),flush=True)
        else:
            status=optional(f,REMOTE+'/status.bin')
            if len(status)==96:
                state,source,rc,detail,port,queued=struct.unpack_from('<5iI',status,32)
                frames,underruns,generation,checked=struct.unpack_from('<4Q',status,56)
                output=struct.unpack_from('<H',status,88)[0]
                print(json.dumps(dict(state=state,source=source,port=port,rc=rc,detail=detail,queued_frames=queued,
                    audio_frames=frames,underruns=underruns,generation=generation,output=hex(output))))
            else:print('No PCM status yet; launch manually and play from the phone.')
            if a.action=='logs':
                for name,path in [('pcm-v6-parent.log',REMOTE+'/parent.log'),('pcm-v6-worker.log',REMOTE+'/worker.log'),('pcm-connect.log','/data/spotifyps5/connect.log')]:
                    data=optional(f,path);(build/name).write_bytes(data)
                    lines=data.decode('utf8',errors='replace').splitlines()
                    if name=='pcm-connect.log':lines=[s for s in lines if any(v in s for v in ['PCM','client library','library:','Web API','audio output','Sink']) and not any(v in s.lower() for v in ['authorization','bearer','access_token','auth_data'])]
                    print(name+':');print('\n'.join(lines[-70:]))
if __name__=='__main__':main()
