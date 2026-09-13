import socket, sys, random
PORT=int(sys.argv[1]); MODE=sys.argv[2]
def enc(a):
    o=b"*%d\r\n"%len(a)
    for x in a:
        x=x.encode() if isinstance(x,str) else x
        o+=b"$%d\r\n"%len(x)+x+b"\r\n"
    return o
class C:
    def __init__(s2, port):
        s2.s=socket.create_connection(("127.0.0.1",port),timeout=30); s2.f=s2.s.makefile("rb")
    def rr(s2):
        line=s2.f.readline(); t=line[:1]
        if t in b"+-:": return line.strip()
        if t==b"$":
            n=int(line[1:-2]); return None if n==-1 else s2.f.read(n+2)[:-2]
        n=int(line[1:-2]); return [s2.rr() for _ in range(n)]
    def cmd(s2,*a): s2.s.sendall(enc(list(a))); return s2.rr()
c=C(PORT)
rng=random.Random(31)
def build():
    # strings: int, raw, big, ttl
    c.cmd("SET","s:int","12345"); c.cmd("SET","s:raw","hello world")
    c.cmd("SET","s:big","B"*50000); c.cmd("SET","s:ttl","tv","EX","5000")
    c.cmd("SET","s:bin","\x00\x01\xff")
    # hash compact + expanded (force promote with 300 fields) + binary
    for i in range(5): c.cmd("HSET","h:c","f%d"%i,"v%d"%i)
    args=[]
    for i in range(300): args+=["pf%d"%i,"pv%d"%i]
    c.cmd(*(["HSET","h:x"]+args))
    c.cmd("HSET","h:bin","a\x00b","c\x00d")
    c.cmd("HSET","h:bigv","f","V"*40000)
    # list compact + deque
    c.cmd(*(["RPUSH","l:c"]+["e%d"%i for i in range(6)]))
    c.cmd(*(["RPUSH","l:x"]+["x%d-"%i+("P"*50) for i in range(400)]))
    c.cmd("LPUSH","l:x","front")
    # set int-compact, generic-compact, table
    c.cmd(*(["SADD","st:int"]+[str(i) for i in range(50)]))
    c.cmd(*(["SADD","st:gen"]+["m%d"%i for i in range(8)]))
    c.cmd(*(["SADD","st:tab"]+["t%d"%i for i in range(400)]))
    # zset compact + skiplist, negative/fractional scores
    for i in range(6): c.cmd("ZADD","z:c",str(i*0.5-1.25),"zm%d"%i)
    args=[]
    for i in range(300): args+=[str(rng.uniform(-1e6,1e6)),"zx%d"%i]
    c.cmd(*(["ZADD","z:x"]+args))
    c.cmd("ZADD","z:inf","inf","up","-inf","down")
    # stream: compact + expanded, tombstone, trimmed head, last_id and entries_added > length
    c.cmd("XADD","x:c","10-0","f","v0")
    c.cmd("XADD","x:c","10-1","f","v1")
    c.cmd("XDEL","x:c","10-1")
    for i in range(300):
        c.cmd("XADD","x:x","%d-0"%(i+1),"f","v%03d"%i+("Q"*256))
    c.cmd("XDEL","x:x","90-0")
    c.cmd("XTRIM","x:x","MINID","=","51-0")
KEYS_CHECK=[("TYPE","s:int"),("GET","s:int"),("GET","s:raw"),("GET","s:big"),("GET","s:bin"),
 ("TTL","s:ttl"),
 ("HGETALL","h:c"),("HGETALL","h:x"),("HGETALL","h:bin"),("HSTRLEN","h:bigv","f"),("HLEN","h:x"),
 ("OBJECT","ENCODING","h:c"),("OBJECT","ENCODING","h:x"),
 ("LRANGE","l:c","0","-1"),("LRANGE","l:x","0","-1"),("LLEN","l:x"),
 ("OBJECT","ENCODING","l:c"),("OBJECT","ENCODING","l:x"),
 ("SMEMBERS","st:int"),("SMEMBERS","st:gen"),("SMEMBERS","st:tab"),("SCARD","st:tab"),
 ("OBJECT","ENCODING","st:int"),("OBJECT","ENCODING","st:tab"),
 ("ZRANGE","z:c","0","-1","WITHSCORES"),("ZRANGE","z:x","0","-1","WITHSCORES"),
 ("ZSCORE","z:inf","up"),("ZSCORE","z:inf","down"),("ZCARD","z:x"),
 ("OBJECT","ENCODING","z:c"),("OBJECT","ENCODING","z:x"),
 ("TYPE","x:c"),("XRANGE","x:c","-","+"),("XLEN","x:c"),
 ("OBJECT","ENCODING","x:c"),
 ("TYPE","x:x"),("XRANGE","x:x","-","+"),("XLEN","x:x"),
 ("OBJECT","ENCODING","x:x"),
 ("DBSIZE",)]
def norm(op, r):
    if op[0]=="TTL": return ("ttl>4000", isinstance(r,bytes) and r!=b":-2" and int(r[1:])>4000)
    if op[0]=="HGETALL" and isinstance(r,list):
        return sorted((r[i],r[i+1]) for i in range(0,len(r),2))
    if op[0]=="SMEMBERS" and isinstance(r,list): return sorted(r)
    return r
def statefile():
    return [norm(op, c.cmd(*op)) for op in KEYS_CHECK]
if MODE=="build_save":
    build()
    state = statefile()
    open("/tmp/claude-1000/snap_state_a.txt","w").write(repr(state))
    save_reply = c.cmd("SAVE")
    if save_reply != b"+OK":
        raise RuntimeError("SAVE failed: %r" % (save_reply,))
    print("SAVE:", save_reply)
    print("state captured: %d checks" % len(state))
elif MODE=="verify":
    state = statefile()
    expect = eval(open("/tmp/claude-1000/snap_state_a.txt").read())
    bad = [ (KEYS_CHECK[i], expect[i], state[i]) for i in range(len(state)) if state[i]!=expect[i] ]
    for b in bad[:6]: print("  MISMATCH", b[0], "expect", str(b[1])[:60], "got", str(b[2])[:60])
    print("TYPED ROUNDTRIP", "PASS (%d/%d)"%(len(state)-len(bad),len(state)) if not bad else "FAIL")
    sys.exit(1 if bad else 0)
