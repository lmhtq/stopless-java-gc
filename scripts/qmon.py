import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon.sock"); s.settimeout(2.0)
time.sleep(0.3)
try: s.recv(65536)
except: pass
for c in sys.argv[1:]:
    s.send((c+"\n").encode()); time.sleep(0.4)
    try: sys.stdout.write(s.recv(65536).decode(errors='replace'))
    except: pass
s.close()
