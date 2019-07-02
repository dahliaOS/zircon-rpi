// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    std::{
        fmt::{self, Debug, Display, Formatter},
        sync::Arc,
    },
    pretty::{BoxAllocator, DocAllocator},
};

/// Asynchronous extensions to Expectation Predicates
pub mod asynchronous;
/// Expectations for the host driver
pub mod host_driver;
/// Expectations for remote peers
pub mod peer;
/// Tests for the expectation module
#[cfg(test)]
pub mod test;

/// A String whose `Debug` implementation pretty-prints
#[derive(Clone)]
pub struct AssertionText(String);

impl fmt::Debug for AssertionText {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// Simplified `DocBuilder` alias used in this file
type DocBuilder<'a> = pretty::DocBuilder<'a, BoxAllocator>;

fn parens<'a>(alloc: &'a BoxAllocator, doc: DocBuilder<'a>) -> DocBuilder<'a> {
    alloc.text("(").append(doc).append(alloc.text(")"))
}

fn show_debug<T: Debug>(t: &T) -> String {
    format!("{:?}", t)
}

/// Function to compare two `T`s
type Comparison<T> = Arc<dyn Fn(&T, &T) -> bool + Send + Sync + 'static>;
/// Function to show a representation of a given `T`
// TODO(nickpollard) - use DocBuilder
type Show<T> = Arc<dyn Fn(&T) -> String + Send + Sync + 'static>;

/// A Boolean predicate on type `T`. Predicate functions are a boolean algebra
/// just as raw boolean values are; they an be ANDed, ORed, NOTed. This allows
/// a clear and concise language for declaring test expectations.
pub enum Predicate<T> {
    Equal(Arc<T>, Comparison<T>, Show<T>),
    And(Box<Predicate<T>>, Box<Predicate<T>>),
    Or(Box<Predicate<T>>, Box<Predicate<T>>),
    Not(Box<Predicate<T>>),
    Predicate(Arc<dyn Fn(&T) -> bool + Send + Sync + 'static>, String),
    // Since we can't use an existential:
    //  for<U> Over(Predicate<U>, Fn(&T) -> &U)
    // we use the trait `IsOver` to hide the type `U` of the intermediary
    Over(Arc<dyn IsOver<T> + Send + Sync>),
    // Since we can't use an existential:
    //  for<I> Any(Fn(&T) -> I, Predicate<I::Elem>)
    //  where I::Elem: Debug
    // we use the trait `IsAny` to hide the type `I` of the iterator
    Any(Arc<dyn IsAny<T> + Send + Sync + 'static>),
    // Since we can't use an existential:
    //  for<I> All(Fn(&T) -> I, Predicate<I::Elem>)
    //  where I::Elem: Debug
    // we use the trait `IsAll` to hide the type `I` of the iterator
    All(Arc<dyn IsAll<T> + Send + Sync + 'static>),
}

impl<T> Clone for Predicate<T> {
    fn clone(&self) -> Self {
        match self {
            Predicate::Equal(t, comp, repr) => Predicate::Equal(t.clone(), comp.clone(), repr.clone()),
            Predicate::And(l, r) => Predicate::And(l.clone(), r.clone()),
            Predicate::Or(l, r) => Predicate::Or(l.clone(), r.clone()),
            Predicate::Not(x) => Predicate::Not(x.clone()),
            Predicate::Predicate(p, msg) => Predicate::Predicate(p.clone(), msg.clone()),
            Predicate::Over(x) => Predicate::Over(x.clone()),
            Predicate::Any(x) => Predicate::Any(x.clone()),
            Predicate::All(x) => Predicate::All(x.clone()),
        }
    }
}

impl<T> Debug for Predicate<T> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.describe(&BoxAllocator).1.pretty(80).fmt(f)
    }
}

/// At most how many elements of an iterator to show in a falsification, when falsifying `any` or
/// `all`
const MAX_ITER_FALSIFICATIONS: usize = 5;

/// Trait representation of OverPred where `U` is existential
pub trait IsOver<T> {
    fn describe<'d>(&self, doc: &'d BoxAllocator) -> DocBuilder<'d>;
    fn falsify_over<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>>;
}

/// Trait representation of a Predicate on members of an existential iterator type
pub trait IsAny<T> {
    fn describe<'d>(&self, doc: &'d BoxAllocator) -> DocBuilder<'d>;
    fn falsify_any<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>>;
}

impl<T: 'static, Elem: 'static> IsAny<T> for Predicate<Elem>
where for<'a> &'a T: IntoIterator<Item = &'a Elem> {
    fn describe<'d>(&self, doc: &'d BoxAllocator) -> DocBuilder<'d> {
        doc.text("ANY")
            .append(doc.newline())
            .append(self.describe(doc).nest(2))
    }
    fn falsify_any<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>> {
        if t.into_iter().any(|el| self.satisfied(el)) {
            None
        } else {
            t.into_iter()
                .filter_map(|i| self.falsify(i, doc))
                .take(MAX_ITER_FALSIFICATIONS)
                .fold(None, |acc: Option<DocBuilder<'d>>, falsification| Some(acc.unwrap_or(doc.nil()).append(falsification)))
        }
    }
}

/// Trait representation of a Predicate on members of an existential iterator type
pub trait IsAll<T> {
    fn describe<'d>(&self, doc: &'d BoxAllocator) -> DocBuilder<'d>;
    fn falsify_all<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>>;
}

impl<T: 'static, Elem: Debug + 'static> IsAll<T> for Predicate<Elem>
where for<'a> &'a T: IntoIterator<Item = &'a Elem> {
    fn describe<'d>(&self, doc: &'d BoxAllocator) -> DocBuilder<'d> {
        doc.text("ALL")
            .append(doc.newline())
            .append(self.describe(doc).nest(2))
    }
    fn falsify_all<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>> {
        let failures = t.into_iter()
            .filter_map(|i| self.falsify(i, doc).map(|falsification| {
                doc.text("ELEM").append(doc.space())
                    .append(doc.text(show_debug(i)).nest(2)).append(doc.space())
                    .append(doc.text("FAILS:")).append(doc.space())
                    .append(falsification.nest(2))
            }))
            .take(MAX_ITER_FALSIFICATIONS)
            .fold(None, |acc: Option<DocBuilder<'d>>, falsification| Some(acc.unwrap_or(doc.nil()).append(falsification)));

        let desc = self.describe(&doc);
        // TODO(nickpollard) - Is this explanation message correct?
        failures.map(|msg| {
            doc.text("FAILED").append(desc).append(doc.text("DUE TO")).append(msg)
        })
    }
}

enum OverPred<T, U> {
    ByRef ( Predicate<U>, Arc<dyn Fn(&T) -> &U + Send + Sync + 'static>, String ),
    ByValue ( Predicate<U>, Arc<dyn Fn(&T) -> U + Send + Sync + 'static>, String ),
}

impl<T, U> IsOver<T> for OverPred<T, U> {
    fn describe<'a>(&self, doc: &'a BoxAllocator) -> DocBuilder<'a> {
        match self {
            OverPred::ByRef(pred, _, path)  => doc.text(path.clone()).append(doc.space()).append(pred.describe(doc)),
            OverPred::ByValue(pred, _, path) => doc.text(path.clone()).append(doc.space()).append(pred.describe(doc)),
        }
    }
    fn falsify_over<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>> {
        match self {
            OverPred::ByRef(pred, project, path) => {
                pred.falsify((project)(t), doc)
                .map(|falsification| doc.text(path.to_string()).append(doc.space()).append(falsification))
            },
            OverPred::ByValue(pred, project, path) => {
                pred.falsify(&(project)(t), doc)
                .map(|falsification| doc.text(path.to_string()).append(doc.space()).append(falsification))
            },
        }
    }
}

impl<T: PartialEq + Debug + 'static> Predicate<T> {
    pub fn equal(t: T) -> Predicate<T> {
        Predicate::Equal(Arc::new(t), Arc::new(T::eq), Arc::new(show_debug))
    }
    pub fn not_equal(t: T) -> Predicate<T> {
        Predicate::Equal(Arc::new(t), Arc::new(T::eq), Arc::new(show_debug))
    }
}

impl<T> Predicate<T> {
    pub fn describe<'a>(&self, doc: &'a BoxAllocator) -> DocBuilder<'a> {
        match self {
            Predicate::Equal(expected, _, repr) =>
                doc.text("EQUAL").append(doc.space()).append(doc.text(repr(expected))),
            Predicate::And(left, right) =>
                parens(doc, left.describe(doc)).nest(2)
                .append(doc.newline())
                .append(doc.text("AND"))
                .append(doc.newline())
                .append(parens(doc, right.describe(doc)).nest(2)),
            Predicate::Or(left, right) =>
                parens(doc, left.describe(doc)).nest(2)
                .append(doc.newline())
                .append(doc.text("OR"))
                .append(doc.newline())
                .append(parens(doc, right.describe(doc)).nest(2)),
            Predicate::Not(inner) => doc.text("NOT").append(inner.describe(doc).nest(2)),
            Predicate::Predicate(_, desc) => doc.text(desc.clone()),
            Predicate::Over(over) => over.describe(doc),
            Predicate::Any(any) => any.describe(doc),
            Predicate::All(all) => all.describe(doc),
        }
    }
    /// Provide a minimized falsification of the predicate, if possible
    pub fn falsify<'d>(&self, t: &T, doc: &'d BoxAllocator) -> Option<DocBuilder<'d>> {
        match self {
            Predicate::Equal(expected, are_eq, repr) => {
                if are_eq(expected, t) {
                    None
                } else {
                    Some(doc.text(repr(t)).append(doc.space()).append(doc.text("!=")).append(doc.space()).append(doc.text(repr(expected))))
                }
            },
            Predicate::And(left, right) => {
                match (left.falsify(t, doc), right.falsify(t, doc)) {
                    (Some(l), Some(r)) => Some(l.append(r)),
                    (Some(l), None) => Some(l),
                    (None, Some(r)) => Some(r),
                    (None, None) => None
                }
            }
            Predicate::Or(left, right) => {
                left.falsify(t, doc).and_then(|l| right.falsify(t, doc).map(|r| parens(doc, l).append(parens(doc, r))))
            },
            Predicate::Not(inner) => {
                match inner.falsify(t, doc) {
                    Some(_) => None,
                    None => Some(doc.text("NOT").append(doc.space()).append(inner.describe(doc)))
                }
            },
            Predicate::Predicate(pred, desc) => if pred(t) { None } else { Some(doc.text(desc.to_string())) },
            Predicate::Over(over) => over.falsify_over(t, &BoxAllocator),
            Predicate::Any(any) => any.falsify_any(t, &BoxAllocator),
            Predicate::All(all) => all.falsify_all(t, &BoxAllocator),
        }
    }
    pub fn satisfied(&self, t: &T) -> bool {
        match self {
            Predicate::Equal(expected, are_eq, _) => are_eq(t, expected),
            Predicate::And(left, right) => left.satisfied(t) && right.satisfied(t),
            Predicate::Or(left, right) => left.satisfied(t) || right.satisfied(t),
            Predicate::Not(inner) => !inner.satisfied(t),
            Predicate::Predicate(pred, _) => pred(t),
            Predicate::Over(over) => over.falsify_over(t, &BoxAllocator).is_none(),
            Predicate::Any(any) => any.falsify_any(t, &BoxAllocator).is_none(),
            Predicate::All(all) => all.falsify_all(t, &BoxAllocator).is_none(),
        }
    }
    pub fn assert_satisfied(&self, t: &T) -> Result<(),AssertionText> {
        let doc = BoxAllocator;
        match self.falsify(t, &doc) {
            Some(falsification) => {
                let d = doc.text("FAILED EXPECTATION").append(doc.space())
                            .append(self.describe(&doc).nest(2)).append(doc.space())
                            .append(doc.text("FALSIFIED BY")).append(doc.space())
                            .append(falsification.nest(2));
                Err(AssertionText(d.1.pretty(80).to_string()))
            },
            None => Ok(()),
        }
    }
    pub fn and(self, rhs: Predicate<T>) -> Predicate<T> {
        Predicate::And(Box::new(self), Box::new(rhs))
    }
    pub fn or(self, rhs: Predicate<T>) -> Predicate<T> {
        Predicate::Or(Box::new(self), Box::new(rhs))
    }
    pub fn not(self) -> Predicate<T> {
        Predicate::Not(Box::new(self))
    }
    pub fn new<F, S>(f: F, label: S) -> Predicate<T>
    where
        F: Fn(&T) -> bool + Send + Sync + 'static,
        S: Into<String>
    {
        Predicate::Predicate(Arc::new(f), label.into())
    }
}

/// Methods to work with `T`s that are some collection of elements `Elem`.
impl<Elem, T: Send + Sync + 'static> Predicate<T>
where for<'a> &'a T: IntoIterator<Item = &'a Elem>,
    Elem: Debug + Send + Sync + 'static {

    pub fn all(pred: Predicate<Elem>) -> Predicate<T> {
        Predicate::All(Arc::new(pred))
    }

    pub fn any(pred: Predicate<Elem>) -> Predicate<T> {
        Predicate::Any(Arc::new(pred))
    }
}

impl<U: Send + Sync + 'static> Predicate<U> {
    pub fn over<F, T, P>(self, project: F, path: P) -> Predicate<T>
    where
        F: Fn(&T) -> &U + Send + Sync + 'static,
        P: Into<String>,
        T: 'static {
        Predicate::Over(Arc::new(OverPred::ByRef(self, Arc::new(project), path.into())))
    }

    pub fn over_value<F, T, P>(self, project: F, path: P) -> Predicate<T>
    where
        F: Fn(&T) -> U + Send + Sync + 'static,
        P: Into<String>,
        T: 'static {
        Predicate::Over(Arc::new(OverPred::ByValue(self, Arc::new(project), path.into())))
    }
    pub fn over_<F, T, P>(self, params: (F,P)) -> Predicate<T>
    where
        F: Fn(&T) -> &U + Send + Sync + 'static,
        P: Into<String>,
        T: 'static {
        Predicate::Over(Arc::new(OverPred::ByRef(self, Arc::new(params.0), params.1.into())))
    }
}

#[macro_export]
macro_rules! focus {
    ($type:ty : $selector:tt) => {
        (|var: &$type| &var.$selector, stringify!($selector).to_string())
    }
}

#[macro_export]
macro_rules! over {
    ($type:ty : $selector:tt, $pred:expr) => {
        $pred.over(|var: &$type| &var.$selector, stringify!($selector).to_string())
    }
}
