// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! # monoid
//!
//! This module introduces the notion of a _Monoid_. For a full description,
//! see [Monoid on Wikipedia](https://en.wikipedia.org/wiki/Monoid)
//!
//! Briefly, a Monoid generalizes over the notion of summing. We're comfortable
//! with summing integers - adding all the integers in a list to produce a
//! total. We're probably also comfortable adding real numbers too. What about
//! other types - Vectors? Matrices? _Strings_? It turns out that other things
//! can be summed as well, and in defining the commonalities between those types
//! of 'sum', we end up with only a few requirements:
//!
//! * A binary operation that combines two values into a resulting value of the
//!   same type
//! * That operation must be *associative*
//!   ([Associativity on Wikipedia](https://en.wikipedia.org/wiki/Associative_property))
//! * A *zero* or *identity* value, which when summed with another value leaves
//!   the other value unchanged
//!   ([Identity Element on Wikipedia](https://en.wikipedia.org/wiki/Identity_element))
//!
//! These few rules define a generalized notion of a 'sum' which covers addition
//! over a variety of types, but also certain types of operation - taking the
//! maximum or minimum, for example, or combining associative arrays - which we
//! don't traditionally think of as sums.
//!
//! This abstraction is useful because it allows us to write code that is
//! generic over any type of sum. Many algorithms can be thought of in terms of
//! Monoids. Famously, the 'reduce' in map-reduce can be thought of as a Monoid.
//!
//! We also include the notion of a semigroup, a more general structure which
//! lacks the identity element. There are times this is useful, but Monoids are
//! the more often used structure. As a generalization of Monoids, all Monoids
//! are Semigroups; not all Semigroups are Monoids.

/// Trait representing the abstract algebra concept of a Semigroup - an
/// associative binary operation `mappend`. Semigroup's do not have an identity
/// element - they are `Monoid`s minus identity. Common examples include integer
/// addition, multiplication, maximum, minimum.
pub trait Semigroup {
    /// Binary operation - this *must* be associative, i.e.
    ///
    ///   `(a op b) op c === a op (b op c)`
    fn mappend(&self, b: &Self) -> Self;
}

/// Trait representing the abstract algebra concept of a Monoid - an associative
/// binary operation `mappend` with an identity element `mzero`. This abstracts
/// over 'summing', including addition, multiplication, concatenation, minimum
/// and maximum.
///
/// Monoids are useful in defining standard ways to 'combine' certain types and
/// then easily combining collections of these, and also deriving ways to define
/// compound types.
pub trait Monoid: Semigroup {
    /// Identity element - an element that has no affect when combined via
    /// `mappend`. This *must* obey the following laws:
    ///
    /// `mzero().mappend(a) === a`
    ///   and
    /// `a.mappend(mzero()) === a`
    fn mzero() -> Self;
}

/// Newtype wrapper invoking the `Max` Monoid.
/// Any numeric with a MIN_VALUE forms a Monoid with:
///   `mappend() = max()`
///     and
///   `mzero() == MIN_VALUE`
/// In the case of usize below, 0 is the MIN_VALUE
#[repr(transparent)]
#[derive(Debug, PartialEq)]
pub struct Max(pub usize);

impl Semigroup for Max {
    fn mappend(&self, b: &Max) -> Max {
        Max(self.0.max(b.0))
    }
}

impl Monoid for Max {
    fn mzero() -> Max {
        Max(0)
    }
}

/// Sum all items in an iterator, using the provided Monoid
///
/// We can always sum an iterator if we have a Monoid - we can recursively
/// combine elements by `mappend`, and if the iterator is empty the result is
/// just `mzero`.
///
/// Fold can be viewed as a Monoid Homomorphism - it maps from one Monoid (list)
/// to another
pub fn msum<I: Iterator<Item = T>, T: Monoid>(iter: I) -> T {
    iter.fold(T::mzero(), |a, b| a.mappend(&b))
}

#[cfg(test)]
pub mod test {
    use proptest::arbitrary::*;
    use proptest::strategy::*;
    use proptest::test_runner::*;
    use proptest::{prop_assert, prop_assert_eq};
    use std::ops::Range;

    use crate::monoid::*;

    impl From<usize> for Max {
        fn from(n: usize) -> Max {
            Max(n)
        }
    }

    impl Arbitrary for Max {
        type Parameters = ();
        type Strategy = MapInto<Range<usize>, Max>;

        fn arbitrary_with(_: ()) -> Self::Strategy {
            (0..std::usize::MAX).prop_map_into()
        }
    }
    /// A valid Monoid implementation must satisfy three laws:
    ///   * associativity
    ///   * left identity
    ///   * right identity
    ///
    /// This function tests all three laws for a given type `T`, when provided
    /// with a valid `proptest::Arbitrary` to generate arbitrary values of type
    /// `T`. Random values are tested against each other to assure that the
    /// lawful properties always hold
    ///
    /// These can be called for a given type by implementing arbitrary and then
    /// invoking this function in a test:
    ///
    ///   #[test]
    ///   fn check_monoid_laws_mytype() {
    ///     monoid_laws::<MyType>();
    ///   }
    pub fn monoid_laws<T: Monoid + Arbitrary + PartialEq>() {
        associativity_law::<T>();
        left_identity_law::<T>();
        right_identity_law::<T>();
    }

    pub fn associativity_law<T: Monoid + Arbitrary + PartialEq>() {
        let mut runner = TestRunner::new(Config::default());
        let result = runner.run(&(T::arbitrary(), T::arbitrary(), T::arbitrary()), |(a, b, c)| {
            prop_assert_eq!((a.mappend(&b)).mappend(&c), a.mappend(&b.mappend(&c)));
            Ok(())
        });
        if let Err(e) = result {
            panic!("Monoid associativity law failed:\n{}\n{}", e, runner);
        }
    }

    pub fn left_identity_law<T: Monoid + Arbitrary + PartialEq>() {
        let mut runner = TestRunner::new(Config::default());
        let result = runner.run(&(T::arbitrary()), |a| {
            prop_assert_eq!(T::mzero().mappend(&a), a);
            Ok(())
        });
        if let Err(e) = result {
            panic!("Monoid left identity law failed:\n{}\n{}", e, runner);
        }
    }

    pub fn right_identity_law<T: Monoid + Arbitrary + PartialEq>() {
        let mut runner = TestRunner::new(Config::default());
        let result = runner.run(&(T::arbitrary()), |a| {
            prop_assert_eq!(a.mappend(&T::mzero()), a);
            Ok(())
        });
        if let Err(e) = result {
            panic!("Monoid left identity law failed:\n{}\n{}", e, runner);
        }
    }

    #[test]
    fn check_monoid_laws_max() {
        monoid_laws::<Max>();
    }
}
