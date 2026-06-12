Full raw outputs (gzipped) of the sound-concurrent-mode trials cited in §5.5:
- trial_ig{1,2,3}.out.gz — IntegrityGC 16 16 4 with STOPLESS_REVOKE_CONCURRENT=1
  STOPLESS_CONCURRENT_MS=50 STOPLESS_MOVE_LIMIT=8 (mixed-trigger: each round's
  System.gc races the background collector). All 16 rounds verified-OK each;
  exit code 0.
- trial_ig_failsafe.out.gz — same scenario on the fail-safe build (open rc
  checked, close retry, Atomic flag); 16/16 verified, 16 coalesced cycles.
- trial_ct{1,2}.out.gz — ConcatTest under the same env; exit code 0.
Command template:
  java -Xint -XX:-UsePerfData -XX:+UnlockExperimentalVMOptions \
       -XX:+UseStoplessGC -XX:-UseCompressedOops -XX:-UseCompressedClassPointers <Test>
- trial_fault_open.out.gz — STOPLESS_FAULT_OPEN=2 (first two epoch-opens
  forced to fail): 2 FAULT-INJECT lines, 2 synchronous-fallback engagements,
  16/16 rounds verified, exit 0.
- trial_fault_close.out.gz — STOPLESS_FAULT_CLOSE=2: 2 FAULT-INJECT lines,
  2 close-retries (pending preserved, relocation coalesced meanwhile),
  16/16 rounds verified, exit 0.
