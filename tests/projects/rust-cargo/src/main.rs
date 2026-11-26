#[cfg(feature = "broken")]
mod broken;

/// Builds and compiles cleanly; `cargo clippy` flags a few lints on purpose.
fn describe(names: &Vec<String>) -> String {
    if names.len() == 0 {
        return String::from("nobody");
    }
    let mut joined = names.join(", ");
    joined.push('!');
    return joined;
}

fn main() {
    let mut count = 0;
    let names = vec![String::from("ferris"), String::from("corro")];
    println!("hello {}", describe(&names));
    println!("count = {}", count);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn describes_nobody() {
        assert_eq!(describe(&Vec::new()), "nobody");
    }
}
