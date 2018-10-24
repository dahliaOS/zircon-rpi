// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::monoid::*;

/// Newtype wrapper for a ZipVec, a Vec whose monoid zips items together rather
/// than concatenating If we know how to combine elements of a Vec, then we can
/// combine two ZipVecs element-wise, padding the shortest with mzero.
///
/// e.g. using the addition monoid on ints:
///
///   > Zip(vec![0,1]).mappend( Zip(vec![1,2,4]) )
///   Zip( vec![1,3,4] )
pub struct Zip<T>(pub Vec<T>);

#[rustfmt::skip]
impl<T: Monoid> Semigroup for Zip<T> {
    fn mappend(&self, b: &Zip<T>) -> Zip<T> {
        let mut v = Zip(vec![]);
        let mut a_ = self.0.iter();
        let mut b_ = b.0.iter();
        loop {
            match (a_.next(), b_.next()) {
                (Some(a), Some(b)) => v.0.push(a.mappend(b)),
                (Some(a), None   ) => v.0.push(a.mappend(&T::mzero())),
                (None,    Some(b)) => v.0.push(b.mappend(&T::mzero())),
                (None,    None   ) => return v,
            }
        }
    }
}

impl<T: Monoid> Monoid for Zip<T> {
    fn mzero() -> Zip<T> {
        Zip(vec![])
    }
}

impl<T> Zip<T> {
    pub fn map<U, F>(&self, f: F) -> Zip<U>
    where
        F: Fn(&T) -> U,
    {
        Zip(self.0.iter().map(f).collect())
    }
}
