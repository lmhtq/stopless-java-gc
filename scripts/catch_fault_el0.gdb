set pagination off
set confirm off
set height 0
file /home/bc/projs/stopless-java-gc/third_party/output/rootfs-morello-purecap/boot/kernel/kernel.full
target remote :1234
hbreak *0x4267c010
printf "=== armed HW bp at 0x4267c010 ; continuing ===\n"
continue
printf "\n========== HIT 0x4267c010 @ EL0 ==========\n"
python
import gdb, re
def reg(n):
    try:
        o = gdb.execute("info registers %s" % n, to_string=True)
        m = re.search(r'0x[0-9a-fA-F]+', o); return int(m.group(0),16) if m else None
    except Exception: return None
def rd(a, n=8):
    try: return int.from_bytes(gdb.selected_inferior().read_memory(int(a)&0xffffffffffffffff, n).tobytes(),'little')
    except Exception: return None
def h(v): return ('0x%x'%(v&0xffffffffffffffff)) if isinstance(v,int) else 'unmapped'
LIBJVM=0x41600000; CC0=0x58bc0000; CC1=0x5d3c0000
def tag(v):
    if not isinstance(v,int): return ''
    v&=0xffffffffffffffff
    if LIBJVM<=v<0x422a9000: return ' libjvm+0x%x'%(v-LIBJVM)
    if CC0<=v<CC1: return ' codecache+0x%x'%(v-CC0)
    return ''
fp=reg("x29"); sp=reg("sp"); lr=reg("x30"); pc=reg("pc")
print("PC=%s FP(x29)=%s SP=%s LR(x30)=%s" % (h(pc),h(fp),h(sp),h(lr)))
print("userspace readable @EL0? [sp]=%s [fp]=%s" % (h(rd(sp)), h(rd(fp))))
print("-- FP-chain walk ([fp]=savedFP, [fp+16]=savedLR cap-addr), 14 deep --")
f=fp
for d in range(14):
    if not isinstance(f,int): break
    sfp=rd(f); slr=rd(f+16)
    print("  [%2d] fp=%s savedFP=%s savedLR=%s%s" % (d, h(f), h(sfp), h(slr), tag(slr)))
    if not isinstance(sfp,int): break
    sfp&=0xffffffffffffffff
    if sfp<=(f&0xffffffffffffffff) or sfp==0: break
    f=sfp
print("-- stack near SP (addr | meta) --")
for off in range(-0x10,0x90,16):
    a=(sp+off) if isinstance(sp,int) else 0
    print("  [sp%+#06x]=%s | %s%s" % (off, h(rd(a)), h(rd(a+8)), tag(rd(a))))
print("========== END ==========")
end
detach
quit
