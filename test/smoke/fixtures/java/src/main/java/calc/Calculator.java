package calc;

// Host fixture for the topo-init scaffold smoke: Maven layout, one class
// with zero-arg void methods only — the L1 extractor does not yet capture
// parameter or return types, and the bytecode verifier compares declared
// parameter counts against the compiled class.
public class Calculator {
    private int total;

    public void reset() {
        total = 0;
    }

    public void increment() {
        total = total + 1;
    }
}
