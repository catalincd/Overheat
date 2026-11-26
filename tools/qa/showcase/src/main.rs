//! Word frequencies, most common first.
use std::collections::HashMap;
use std::fmt;

#[derive(Debug, Clone, PartialEq)]
struct Entry<'a> {
    word: &'a str,
    count: usize,
}

impl fmt::Display for Entry<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:>12} {}", self.word, self.count)
    }
}

fn frequencies(text: &str) -> Vec<Entry<'_>> {
    let mut counts: HashMap<&str, usize> = HashMap::new();
    for word in text.split_whitespace() {
        *counts.entry(word).or_default() += 1;
    }
    let mut entries: Vec<Entry> = counts
        .into_iter()
        .map(|(word, count)| Entry { word, count })
        .collect();
    entries.sort_by(|a, b| b.count.cmp(&a.count).then(a.word.cmp(b.word)));
    entries
}

fn main() {
    let text = "the quick brown fox jumps over the lazy dog the end";
    for entry in frequencies(text).iter().take(5) {
        println!("{entry}");
    }
    if let Some(top) = frequencies(text).first() {
        assert_eq!(top.word, "the");
    }
}
