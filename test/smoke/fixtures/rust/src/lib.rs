// Host fixture for the topo-init scaffold smoke: a library crate with
// free functions only — the backend compiles via `cargo rustc --lib`,
// and IR-level symbol mapping for struct impl methods is not wired up
// yet.

pub fn add(a: i32, b: i32) -> i32 {
    a + b
}

pub fn twice(x: i32) -> i32 {
    x * 2
}
