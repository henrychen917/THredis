import socket, sys, time
PORT=int(sys.argv[1]); MODE=sys.argv[2]
def enc(a):
    o=b"*%d\r\n"%len(a)
    for x in a:
        x=x.encode() if isinstance(x,str) else x
        o+=b"$%d\r\n"%len(x)+x+b"\r\n"
    return o
class C:
    def __init__(s2,port):
        s2.s=socket.create_connection(("127.0.0.1",port),timeout=30); s2.f=s2.s.makefile("rb")
    def rr(s2):
        line=s2.f.readline(); t=line[:1]
        if t in b"+-:": return line.strip()
        if t==b"$":
            n=int(line[1:-2]); return None if n==-1 else s2.f.read(n+2)[:-2]
        n=int(line[1:-2]); return [s2.rr() for _ in range(n)]
    def cmd(s2,*a): s2.s.sendall(enc(list(a))); return s2.rr()
c=C(PORT)
N=4000
if MODE=="race":
    # typed dataset big enough to give capture a window
    for i in range(0,N,200):
        c.s.sendall(b"".join(enc(["HSET","H%d"%j,"f1","A"*300,"f2","B"*300]) for j in range(i,i+200)))
        for _ in range(200): c.rr()
        c.s.sendall(b"".join(enc(["ZADD","Z%d"%j,"1.5","m"+"C"*200,"2.5","n"+"D"*200]) for j in range(i,i+200)))
        for _ in range(200): c.rr()
        c.s.sendall(b"".join(enc(["RPUSH","L%d"%j]+["e"+"E"*100]*5) for j in range(i,i+200)))
        for _ in range(200): c.rr()
    print("BGSAVE:", c.cmd("BGSAVE"))
    m=C(PORT)
    # typed overwrite storm immediately: mutate every 2nd key of each type
    js=list(range(0,N,2))
    B=200
    for i in range(0,len(js),B):
        chunk=js[i:i+B]
        m.s.sendall(b"".join(enc(["HSET","H%d"%j,"f1","MUT"])+enc(["ZADD","Z%d"%j,"9.9","RACED"])+enc(["RPUSH","L%d"%j,"RACED"]) for j in chunk))
        for _ in range(3*len(chunk)): m.rr()
    def info(f):
        t=c.cmd("INFO")
        for ln in t.split(b"\r\n"):
            if ln.startswith(f.encode()+b":"): return ln.split(b":",1)[1]
    t0=time.time()
    while info("rdb_bgsave_in_progress")!=b"0" and time.time()-t0<120: time.sleep(0.1)
    pre=info("snapshot_preimages")
    print("capture done; snapshot_preimages=%s" % pre.decode())
    print("PREIMAGE-FIRED", "PASS" if int(pre)>0 else "FAIL (mechanism never exercised!)")
elif MODE=="verify":
    bad=0
    for j in range(0,N,97):   # sample across the keyspace
        h=c.cmd("HGET","H%d"%j,"f1"); z=c.cmd("ZSCORE","Z%d"%j,"RACED"); l=c.cmd("LLEN","L%d"%j)
        if h!=b"A"*300: bad+=1          # cut state: original value, never MUT
        if z is not None: bad+=1        # RACED member is post-cut
        if l!=b":5": bad+=1             # 5 elements at cut, RACED push is post-cut
    print("TYPED RACE VERIFY", "PASS" if bad==0 else "FAIL (%d)"%bad)
    sys.exit(1 if bad else 0)
