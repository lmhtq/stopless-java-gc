import re
lines=open('/tmp/qemu_trap.log',errors='replace').read().splitlines()
i=0
events=[]
while i<len(lines):
    if 'Taking exception 1 [Undefined Instruction]' in lines[i]:
        blk=lines[i:i+7]
        elr=esr=froml=ret=None
        for b in blk:
            m=re.search(r'with ELR 0x([0-9a-f]+)',b);  elr=m.group(1) if m else elr
            m=re.search(r'with ESR (0x[0-9a-f/]+)',b);  esr=m.group(1) if m else esr
            if '...from EL' in b: froml=b.strip()
        # the matching 'Exception return ... to ... EL0 PC X'
        for j in range(i+1,min(i+10,len(lines))):
            m=re.search(r'Exception return.*to.*EL0 PC 0x([0-9a-f]+)',lines[j])
            if m: ret=m.group(1); break
        events.append((i+1,elr,esr,ret))
        i+=7
    else:
        i+=1
print("total undef:",len(events))
# fatal candidates: kernel did NOT return to ELR+4 (i.e., not emulated-and-advanced)
print("\n=== undefs where return PC != ELR+4 (NOT emulated -> handler/terminate) ===")
n=0
for (ln,elr,esr,ret) in events:
    if not elr: continue
    e=int(elr,16); r=int(ret,16) if ret else None
    if r is None or r!=(e+4):
        print(f"line {ln}: ESR {esr} ELR 0x{elr} -> returnEL0 {('0x'+ret) if ret else 'NONE'}")
        n+=1
        if n>20: break
print("count(non-emulated):",n)
