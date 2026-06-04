set pagination off
set confirm off
set height 0
set remotetimeout 180
set tcp connect-timeout 180
file /home/bc/projs/stopless-java-gc/third_party/output/rootfs-morello-purecap/boot/kernel/kernel.full
target remote :1234
hbreak *0x426bd010
printf "=== armed hbreak at crash-landing 0x426bd010; continuing ===\n"
continue
printf "\n########## STOPPED at landing 0x426bd010 ##########\n"
python
import gdb
def regv(n):
    try: return int(gdb.parse_and_eval("(unsigned long)$%s"%n))&0xffffffffffffffff
    except Exception:
        try: return int(gdb.parse_and_eval("$%s"%n))&0xffffffffffffffff
        except Exception: return None
for r in ("pc","x30","sp","x29","x8","x9","x21","x22","x12","x13","x20"):
    print("  %-4s = 0x%x"%(r, regv(r) or 0))
# $lr is the key: if the faulting branch was BLR/cap_BLR, lr = instruction AFTER it
# (=the branch's location+4). If it was a ret/br, lr is unrelated. Disassemble around lr.
lr = regv("x30")
if lr:
    print("---- disasm around lr-0x10 (possible branch site) ----")
    try: print(gdb.execute("disassemble 0x%x, 0x%x"%((lr-0x10)&0xffffffffffffffff,(lr+0x8)&0xffffffffffffffff), to_string=True))
    except Exception as e: print("  disas failed: %s"%e)
print("########## END ##########")
end
detach
quit
