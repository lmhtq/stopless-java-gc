// Pinpoint probe: is the C-9 root-fixup corruption specific to fixing an
// INTERPRETER-STACK root slot, or does it happen for ANY root fixup?
// Here the only reference to the Node is a STATIC FIELD (reached by the root
// scan via CLDToOopClosure), with NO live interpreter-stack local holding it
// across System.gc(). Run with STOPLESS_MOVE_KLASS=StaticRootGC$Node so only
// this Node moves (skipping system objects).
//   - If this completes OK  -> the bug is interpreter-stack slot fixup.
//   - If this also hangs     -> the bug is general oop_store-to-root / revoke.
// No string concat / invokedynamic (uses p/pi/nl like StoplessBench).
public class StaticRootGC {
    static final class Node {
        int id;
        int check;
        Node(int id) { this.id = id; this.check = id * 31 + 7; }
    }

    static Node held;   // the ONLY root to the Node across the gc

    static void p(String s) { System.err.print(s); }
    static void pi(long v)  { System.err.print(v); }
    static void nl()        { System.err.println(); }

    public static void main(String[] a) {
        p("[SR] start"); nl();
        held = new Node(42);                 // result -> putstatic; no local keeps it
        p("[SR] pre id="); pi(held.id); p(" check="); pi(held.check); nl();

        p("[SR] gc"); nl();
        System.gc();                         // moves held's Node, fixes static slot, revokes old
        p("[SR] post-gc"); nl();

        Node h = held;                       // re-read static (fixed to new cap)
        p("[SR] post id="); pi(h.id); p(" check="); pi(h.check); nl();
        if (h.id != 42 || h.check != 42 * 31 + 7) { p("[SR] FAIL"); nl(); throw new RuntimeException("fail"); }
        p("[SR] OK"); nl();
    }
}
