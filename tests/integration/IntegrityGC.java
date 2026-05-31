// IntegrityGC: minimal, unbuffered-stderr data-integrity probe for the C-9
// stop-the-world Stopless move. Starts from HelloGC's known-good regime (N=8,
// one gc) and adds exactly two things: small int[] arrays per node (exercise
// the array move/cap-copy path) and a read-back integrity check after the move.
//
// EVERY phase marker goes to System.err, which is unbuffered for the console
// AND line-flushed even when redirected — so if the JVM hangs mid-run, the last
// marker tells us exactly where (unlike System.out, which block-buffers to a
// file and loses everything on kill -9).
//
// args: [N] [rounds] [arrayLen]   defaults: 8 1 4
public class IntegrityGC {
    static final class Node {
        int id;
        int check;        // == id * 31 + 7, verified after move
        int[] payload;    // small array, exercises arrayOop move path
        Node next;        // keeps the chain reachable as a live root
        Node(int id, int alen) {
            this.id = id;
            this.check = id * 31 + 7;
            this.payload = new int[alen];
            for (int i = 0; i < alen; i++) this.payload[i] = id * 1000 + i;
        }
    }

    public static void main(String[] args) {
        int N      = args.length > 0 ? Integer.parseInt(args[0]) : 8;
        int rounds = args.length > 1 ? Integer.parseInt(args[1]) : 1;
        int alen   = args.length > 2 ? Integer.parseInt(args[2]) : 4;
        System.err.println("[IG] start N=" + N + " rounds=" + rounds + " alen=" + alen);

        for (int r = 0; r < rounds; r++) {
            System.err.println("[IG] round " + r + " phase=alloc");
            Node head = null;
            for (int i = 0; i < N; i++) {
                Node n = new Node(i, alen);
                n.next = head;
                head = n;
            }
            System.err.println("[IG] round " + r + " phase=pre-gc-verify");
            verify(head, N, alen, "pre-gc");

            System.err.println("[IG] round " + r + " phase=gc-call");
            System.gc();   // -> VM_StoplessCollect: STW root-move + revoke
            System.err.println("[IG] round " + r + " phase=post-gc-return");

            // Reads here go through capabilities that may have been revoked &
            // self-healed by the SIGPROT handler / forwarding table.
            verify(head, N, alen, "post-gc");
            System.err.println("[IG] round " + r + " phase=verified-OK");
        }
        System.err.println("[IG] ALL-OK");
    }

    static void verify(Node head, int N, int alen, String tag) {
        int count = 0;
        for (Node n = head; n != null; n = n.next) {
            int expId = N - 1 - count;
            if (n.id != expId)                       fail(tag, "id", expId, n.id);
            if (n.check != n.id * 31 + 7)            fail(tag, "check", n.id * 31 + 7, n.check);
            if (n.payload == null)                   fail(tag, "payload-null", 0, -1);
            if (n.payload.length != alen)            fail(tag, "payload-len", alen, n.payload.length);
            for (int i = 0; i < alen; i++)
                if (n.payload[i] != n.id * 1000 + i) fail(tag, "payload[" + i + "]", n.id * 1000 + i, n.payload[i]);
            count++;
        }
        if (count != N) fail(tag, "count", N, count);
        System.err.println("[IG]   verify(" + tag + ") OK count=" + count);
    }

    static void fail(String tag, String what, int exp, int got) {
        System.err.println("[IG] FAIL " + tag + " " + what + " expected=" + exp + " got=" + got);
        throw new RuntimeException("integrity FAIL " + tag + " " + what);
    }
}
