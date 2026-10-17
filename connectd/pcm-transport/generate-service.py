"""Make a complete, seekable 12-hour silent FLAC; never a sparse/rewritten tape."""
from pathlib import Path
import hashlib,struct

def table(poly,bits):
    out=[]
    for v in range(256):
        crc=v<<(bits-8)
        for _ in range(8):crc=((crc<<1)^poly if crc&(1<<(bits-1)) else crc<<1)&((1<<bits)-1)
        out.append(crc)
    return out
C8=table(7,8);C16=table(0x8005,16)
def crc(data,t,bits):
    v=0
    for b in data:v=((v<<8)^t[((v>>(bits-8))^b)&255])&((1<<bits)-1)
    return v
def utf8(value):
    if value<128:return bytes([value])
    n=2
    while value>=(1<<(5*n+1)):n+=1
    tail=[]
    for _ in range(n-1):tail.append(0x80|(value&63));value>>=6
    return bytes([(0xff<<(8-n))&255|value]+tail[::-1])
def metadata(kind,data,last=False):return bytes([kind|(128 if last else 0)])+len(data).to_bytes(3,'big')+data
def main():
    frames=48000*12*3600//4096
    audio=bytearray();seek=bytearray()
    for i in range(frames):
        if i%750==0:seek.extend(struct.pack('>QQH',i*4096,len(audio),4096))
        header=b'\xff\xf8\xc0\x18'+utf8(i)
        frame=header+bytes([crc(header,C8,8)])+b'\0\0\0'*2
        audio.extend(frame+crc(frame,C16,16).to_bytes(2,'big'))
    stream=struct.pack('>HH',4096,4096)+(14).to_bytes(3,'big')+(18).to_bytes(3,'big')
    stream+=((48000<<44)|(1<<41)|(15<<36)|(frames*4096)).to_bytes(8,'big')+bytes(16)
    vendor=b'librespot-ps5';comments=[b'TITLE=librespot-ps5 Service',b'ARTIST=librespot-ps5']
    tags=struct.pack('<I',len(vendor))+vendor+struct.pack('<I',len(comments))
    for c in comments:tags+=struct.pack('<I',len(c))+c
    data=b'fLaC'+metadata(0,stream)+metadata(3,seek)+metadata(4,tags,True)+audio
    target=Path(__file__).resolve().parents[2]/'build'/'librespot-ps5 Service.flac'
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(data)
    print('12-hour service FLAC',len(data),'bytes',hashlib.sha256(data).hexdigest())
if __name__=='__main__':main()
