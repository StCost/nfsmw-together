import struct,sys
M=0xffffffff
def mix(a,b,c):
    a=(a-b-c)&M; a^=c>>13
    b=(b-c-a)&M; b^=(a<<8)&M
    c=(c-a-b)&M; c^=b>>13
    a=(a-b-c)&M; a^=c>>12
    b=(b-c-a)&M; b^=(a<<16)&M
    c=(c-a-b)&M; c^=b>>5
    a=(a-b-c)&M; a^=c>>3
    b=(b-c-a)&M; b^=(a<<10)&M
    c=(c-a-b)&M; c^=b>>15
    return a,b,c
def h(s,init=0xABCDEF00):
    k=s.encode(); L=len(k); a=b=0x9e3779b9; c=init; i=0
    while L-i>=12:
        a=(a+struct.unpack('<I',k[i:i+4])[0])&M; b=(b+struct.unpack('<I',k[i+4:i+8])[0])&M; c=(c+struct.unpack('<I',k[i+8:i+12])[0])&M
        a,b,c=mix(a,b,c); i+=12
    r=k[i:]; c=(c+len(k))&M
    rr=r+b'\0'*12
    a=(a+struct.unpack('<I',rr[0:4])[0])&M
    b=(b+struct.unpack('<I',rr[4:8])[0])&M
    c=(c+((struct.unpack('<I',rr[8:12])[0])<<8))&M
    a,b,c=mix(a,b,c); return c
D=open(r'H:/Need for Speed Most Wanted — co-campaign/GLOBAL/gameplay.bin','rb').read()
def find(v):
    p=struct.pack('<I',v); r=[];i=0
    while True:
        i=D.find(p,i)
        if i<0: break
        r.append(i); i+=1
    return r
if __name__=='__main__':
    for s in sys.argv[1:]:
        print(s, hex(h(s)), find(h(s))[:5])
