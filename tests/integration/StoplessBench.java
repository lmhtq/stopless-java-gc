// StoplessBench — C-11 microbenchmark for the CHERI-native moving GC, written
// to AVOID invokedynamic (broken on purecap Zero): no string `+`, no lambdas,
// no method refs. Output uses System.err.print(literal) + print(int/long)
// overloads (String.valueOf path, no indy). Timing via System.nanoTime().
//
// What it measures for paper §5:
//   - alloc_ns : allocation cost for N linked Nodes (each with an int[] payload)
//   - gc_pause_ns : wall time of System.gc() == the C-9 stop-the-world move+revoke
//   - integrity : every field/array element read back correctly AFTER the move,
//     i.e. mutator reads through revoked caps self-heal via SIGPROT->forward-table
//
// args: [N] [rounds] [alen]   defaults: 256 3 8
public class StoplessBench {
    static final class Node {
        int id;
        int check;
        int[] payload;
        Node next;
        Node(int id, int alen) {
            this.id = id;
            this.check = id * 31 + 7;
            this.payload = new int[alen];
            for (int i = 0; i < alen; i++) this.payload[i] = id * 1000 + i;
        }
    }

    // Live roots held in a static array so the collector MUST move them and
    // fix the static-field root slots (exercises C-8 root scan + C-9 move).
    static Node[] roots;

    static void p(String s) { System.err.print(s); }
    static void pi(long v)  { System.err.print(v); }   // print long: no concat
    static void nl()        { System.err.println(); }

    public static void main(String[] args) {
        int N      = args.length > 0 ? Integer.parseInt(args[0]) : 256;
        int rounds = args.length > 1 ? Integer.parseInt(args[1]) : 3;
        int alen   = args.length > 2 ? Integer.parseInt(args[2]) : 8;

        p("[BENCH] start N="); pi(N); p(" rounds="); pi(rounds); p(" alen="); pi(alen); nl();
        roots = new Node[N];

        for (int r = 0; r < rounds; r++) {
            p("[BENCH] round "); pi(r); p(" alloc"); nl();
            long t0 = System.nanoTime();
            Node head = null;
            for (int i = 0; i < N; i++) {
                Node n = new Node(i, alen);
                n.next = head;
                head = n;
                roots[i] = n;
            }
            long t1 = System.nanoTime();
            p("[BENCH]   alloc_ns="); pi(t1 - t0); p(" objs="); pi(N); nl();

            verify(head, N, alen, 0);          // pre-gc

            p("[BENCH] round "); pi(r); p(" gc"); nl();
            long g0 = System.nanoTime();
            System.gc();                       // C-9 STW move + revoke
            long g1 = System.nanoTime();
            p("[BENCH]   gc_pause_ns="); pi(g1 - g0); nl();

            verify(head, N, alen, 1);          // post-gc: reads self-heal via SIGPROT
            p("[BENCH] round "); pi(r); p(" OK"); nl();
        }
        p("[BENCH] ALL-OK"); nl();
    }

    static void verify(Node head, int N, int alen, int tag) {
        int count = 0;
        for (Node n = head; n != null; n = n.next) {
            int expId = N - 1 - count;
            if (n.id != expId)                       fail(tag, 1, expId, n.id);
            if (n.check != n.id * 31 + 7)            fail(tag, 2, n.id * 31 + 7, n.check);
            if (n.payload == null)                   fail(tag, 3, 0, -1);
            if (n.payload.length != alen)            fail(tag, 4, alen, n.payload.length);
            for (int i = 0; i < alen; i++)
                if (n.payload[i] != n.id * 1000 + i) fail(tag, 5, n.id * 1000 + i, n.payload[i]);
            count++;
        }
        if (count != N) fail(tag, 6, N, count);
        p("[BENCH]   verify tag="); pi(tag); p(" OK count="); pi(count); nl();
    }

    static void fail(int tag, int what, int exp, int got) {
        p("[BENCH] FAIL tag="); pi(tag); p(" what="); pi(what);
        p(" exp="); pi(exp); p(" got="); pi(got); nl();
        throw new RuntimeException("integrity FAIL");   // literal: no concat
    }
}
