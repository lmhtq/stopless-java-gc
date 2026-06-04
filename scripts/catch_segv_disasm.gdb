set pagination off
set confirm off
set height 0
set remotetimeout 60
set tcp connect-timeout 60
file /home/bc/projs/stopless-java-gc/third_party/output/rootfs-morello-purecap/boot/kernel/kernel.full
target remote :1234
break sigexit if sig == 11
printf "=== armed (sigexit sig==11) ===\n"
continue
printf "\n========== HIT sigexit(sig=11) ==========\n"
python
import gdb, re
def reg(name):
    out = gdb.execute("info registers %s" % name, to_string=True)
    m = re.search(r'0x[0-9a-fA-F]+', out)
    return int(m.group(0), 16) if m else None
def rd(addr, n=8):
    try:
        return int.from_bytes(gdb.selected_inferior().read_memory(int(addr)&0xffffffffffffffff, n).tobytes(),'little')
    except Exception as e:
        return None
td = reg("x0") & 0xffffffffffffffff
tt = gdb.lookup_type("struct thread")
off_frame = [f.bitpos//8 for f in tt.fields() if f.name == "td_frame"][0]
tf = rd(td + off_frame) & 0xffffffffffffffff
ft = gdb.lookup_type("struct trapframe")
offs = {f.name: f.bitpos//8 for f in ft.fields()}
elr=rd(tf+offs["tf_elr"]); sp=rd(tf+offs["tf_sp"]); lr=rd(tf+offs["tf_lr"])
print("tf_elr=0x%x  tf_sp=0x%x  tf_lr=0x%x" % (elr&0xffffffffffffffff, sp&0xffffffffffffffff, lr&0xffffffffffffffff))
# GPR array (addresses of caps). Find the array field.
arr=None
for n in ("tf_x","tf_capregs","tf_c","tf_regs"):
    if n in offs: arr=n; break
base=tf+offs[arr]
g=[rd(base+i*16)&0xffffffffffffffff for i in range(0,31)]
for i in range(0,31):
    print("  x%-2d=0x%x" % (i,g[i]), end=("\n" if i%4==3 else "  "))
print()
# Disassemble the generated code at rscratch1(x8) and rscratch2(x9) and lr — these point at interp/return-entry/dispatch.
def disasm(label, a, n=20):
    a &= 0xffffffffffffffff
    print("---- disasm %s @ 0x%x ----" % (label, a))
    try:
        print(gdb.execute("x/%di 0x%x" % (n, a), to_string=True))
    except Exception as e:
        print("  (disasm failed: %s)" % e)
# x8=rscratch1, x9=rscratch2(bad br target=elr), x21=rdispatch, x22=rbcp
disasm("x8/rscratch1", g[8])
disasm("lr/x30", lr)
print("---- rbcp(x22)=0x%x : bytes it points at ----" % (g[22]&0xffffffffffffffff))
for k in range(0,8):
    print("   [rbcp+%d]=0x%02x" % (k, (rd(g[22]+k,1) or 0)&0xff))
print("---- rdispatch(x21)=0x%x : first 8 table entries (16B stride, addr halves) ----" % (g[21]&0xffffffffffffffff))
for k in range(0,8):
    print("   [rdispatch+%d*16]=0x%x" % (k, (rd(g[21]+k*16) or 0)&0xffffffffffffffff))
print("========== END ==========")
end
detach
quit
