// Minimal isolation probe for the invokedynamic/Access<> blocker.
// Line 1 is a pure string LITERAL to System.err (no concat, no indy).
// Line 2 forces `invokedynamic makeConcatWithConstants` (javac 17 lowers
// string `+` to it) -> StringConcatFactory -> MethodHandle/MemberName.
// If marker 1 prints and marker 2 does NOT, the concat/indy path is the hang.
// Run under EpsilonGC to fully isolate from StoplessGC.
public class ConcatTest {
    public static void main(String[] a) {
        System.err.println("[CT] 1 before-concat (literal only)");
        System.err.flush();
        int n = a.length;
        String s = "n=" + n + " ok";   // <-- invokedynamic makeConcatWithConstants
        System.err.println("[CT] 2 after-concat s=" + s);
        System.err.flush();
        System.err.println("[CT] 3 DONE");
        System.err.flush();
    }
}
