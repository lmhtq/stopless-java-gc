set pagination off
set confirm off
set height 0
file /home/bc/projs/stopless-java-gc/third_party/output/rootfs-morello-purecap/boot/kernel/kernel.full
target remote :1234
break sigexit if sig == 4
printf "=== armed ===\n"
continue
python
import gdb, re
def reg(name):
    out = gdb.execute("info registers %s" % name, to_string=True)
    m = re.search(r'0x[0-9a-fA-F]+', out)
    return int(m.group(0), 16) if m else None
def rd(addr, n=8):
    m = gdb.selected_inferior().read_memory(int(addr) & 0xffffffffffffffff, n).tobytes()
    return int.from_bytes(m, 'little')
print("\n========== HIT sigexit(sig=4) ==========")
td = reg("x0") & 0xffffffffffffffff
print("TD = 0x%x" % td)
tt = gdb.lookup_type("struct thread")
off_frame = [f.bitpos//8 for f in tt.fields() if f.name == "td_frame"][0]
tf = rd(td + off_frame) & 0xffffffffffffffff
print("TRAPFRAME = 0x%x  (offsetof td_frame=%d)" % (tf, off_frame))
ft = gdb.lookup_type("struct trapframe")
offs = {f.name: f.bitpos//8 for f in ft.fields()}
print("trapframe fields:", sorted(offs.items(), key=lambda x: x[1]))
for fld in ("tf_elr","tf_pc","tf_lr","tf_sp","tf_esr","tf_spsr","tf_far"):
    if fld in offs:
        print(">>> %-8s = 0x%x" % (fld, rd(tf + offs[fld]) & 0xffffffffffffffff))
for arrname in ("tf_x","tf_capregs","tf_c","tf_regs"):
    if arrname in offs:
        base = tf + offs[arrname]
        for i in range(0, 18):
            print("    %s[%d].addr = 0x%x" % (arrname, i, rd(base + i*16) & 0xffffffffffffffff))
        break
print("========== END ==========")
end
detach
quit
