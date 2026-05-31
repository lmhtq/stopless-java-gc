public class MinMove {
    static final class N { int id; N(int x){ id = x; } }
    static void p(String s){ System.err.print(s); }
    static void pi(long v){ System.err.print(v); }
    static void nl(){ System.err.println(); }
    public static void main(String[] a) {
        N n = new N(77);
        p("[MM] pre id="); pi(n.id); nl();
        System.gc();              // moves n (direct local root), revokes old
        p("[MM] post-gc reached"); nl();
        int v = n.id;             // deref moved+revoked n -> fault -> V2 self-heal
        p("[MM] post id="); pi(v); nl();
        if (v != 77) { p("[MM] FAIL"); nl(); } else { p("[MM] OK"); nl(); }
    }
}
