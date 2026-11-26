// Intentional errors for Overheat's problem matchers (`cargo build --features broken`).

pub fn mismatched() -> u32 {
    let text: u32 = "forty-two";
    text + missing_value
}

pub fn borrowed() {
    let v = vec![1, 2, 3];
    let first = &v[0];
    drop(v);
    println!("{first}");
}
