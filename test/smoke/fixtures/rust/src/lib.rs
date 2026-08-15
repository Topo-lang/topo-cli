// Host fixture for the topo-init scaffold smoke: a library crate with
// free functions AND a struct with impl methods — the backend compiles
// via `cargo rustc --lib`, and IR-level symbol mapping for rust impl
// methods matches `ns::Type::method` declarations since the rust-v0
// angle-bracket demangle fix (topo-llvm SymbolMapper), so the generated
// scaffold must declare the struct + its methods and verify cleanly.

pub fn add(a: i32, b: i32) -> i32 {
    a + b
}

pub fn twice(x: i32) -> i32 {
    x * 2
}

pub struct Counter {
    value: i32,
}

impl Counter {
    pub fn new(value: i32) -> Self {
        Counter { value }
    }

    pub fn add(&mut self, delta: i32) -> i32 {
        self.value += delta;
        self.value
    }
}
