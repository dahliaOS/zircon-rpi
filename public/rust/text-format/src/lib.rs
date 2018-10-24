// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use algebra::monoid::*;
use algebra::zip::*;
use std::fmt::Write;

/// Format a matrix of strings (plus an optional row of headers) into an aligned table
/// e.g.
///
///   > tabulate(vec![vec![("yellow", "green", "blue")],
///                   vec![("red", "orange", "fuchsia")]],
///              Some(vec!["First", "Second", "Third"]))
///
///   First  Second Third
///   ======================
///   yellow green  blue
///   red    orange fuchsia
pub fn tabulate<S: AsRef<str>, T: AsRef<str>>(rows: Vec<Vec<S>>, headers: Option<Vec<T>>) -> String {

    fn row_widths<S: AsRef<str>>(row: &Vec<S>) -> Zip<Max> {
        Zip(row.iter().map(|s| Max(s.as_ref().len())).collect())
    };

    let header_widths = msum(headers.iter().map(row_widths));
    let widths = msum(rows.iter().map(row_widths));
    let widths = header_widths.mappend(&widths).map(|n| n.0 + 1).0;

    let mut string = String::new();
    for hs in headers {
        write_row_padded(&mut string, &widths, &hs);
        write_underline(&mut string, widths.iter().sum());
    }
    for row in rows {
        write_row_padded(&mut string, &widths, &row);
    }
    string
}

/// Write an underline of a given length of '=' characters to a target string
fn write_underline(out: &mut String, length: usize) {
    writeln!(out, "{:=<width$}", "", width = length).unwrap();
}

/// Write a row of fields padded to given widths to a target string
fn write_row_padded<S: AsRef<str>>(out: &mut String, widths: &Vec<usize>, row: &Vec<S>) {
    for (text, cols) in row.iter().zip(widths.iter()) {
        write!(out, "{:width$}", text.as_ref(), width = cols).unwrap();
    }
    writeln!(out, "").unwrap();
}
