use failure::{Error, err_msg};
use futures::{
    channel::{oneshot, mpsc},
    Future,
    Stream,
    task::{LocalWaker, Poll}
};
use core::pin::Pin;
use std::sync::Arc;
use parking_lot::RwLock;
use fuchsia_async as fasync;

/// A trait for types that are actors who can receive and respond to messages of type `Message`
pub trait Actor {
    type Message: Send + Sync;

/*
   Could have this be Envelope<Message>, to allow some standard headers.

   Could do it via message traits ala Actix What about types responders?
   What do I send as the message target? Perhaps just route via oneshot channels?
   */
    fn update(&mut self, msg: Self::Message, context: ActorContext<Self>);
    // TODO - handle errors by returning a result?
    // Then can terminate actors if they return an error?
    // Or terminate the whole system?
    // fn update(&mut self, msg: Message, system: System) -> Result<(),Error>;
}

/// Context required to run an Actors `update()` fn, including access to the Actor System for
/// spawning new actors, and a reference to the inbox for the actor
pub struct ActorContext<A: Actor + ?Sized> {
    pub system: System,
    pub handle: ActorHandle<A::Message>,
}

impl<A: Actor + ?Sized> Clone for ActorContext<A> {
    fn clone(&self) -> Self {
        ActorContext{ system: self.system.clone(), handle: self.handle.clone() }
    }
}

struct ActorCell<A: Actor> {
    actor: A,
    inbox: mpsc::UnboundedReceiver<A::Message>,
    cx: ActorContext<A>
}

trait ActorProc {
    fn run_next(&mut self, lw: &LocalWaker) -> Poll<Option<()>>;
}

struct ActorStream {
    inner: Box<dyn ActorProc + Send + Sync + 'static>
}

impl Stream for ActorStream {
  type Item = ();
  fn poll_next(self: Pin<&mut Self>, lw: &LocalWaker) -> Poll<Option<Self::Item>> {
      Pin::get_mut(self).inner.run_next(lw)
  }
}

impl<A: Actor> ActorProc for ActorCell<A> {
    fn run_next(&mut self, lw: &LocalWaker) -> Poll<Option<()>> {
        let inbox = Pin::new(&mut self.inbox);
        match inbox.poll_next(lw) {
            Poll::Ready(Some(msg)) => Poll::Ready(Some(self.actor.update(msg, self.cx.clone()))),
            Poll::Ready(None) => Poll::Ready(None),
            Poll::Pending => Poll::Pending
        }
    }
}

/*
   An Actor System, which contains multiple actors which can all be
   executed via a single executor.

   This should include being executed in a multi-threaded scenerio, in which case
   we want to ensure that each actor is only executed on a single thread at a time.

   * In theory each actor being processed is a stream of incoming messages
   * for_each-ing the stream gives a future per actor
   * which should become a task per actor in the executor
   * The system can just for-each each actor and then join the futures
   * But what about dynamic spawning?
     * With a handle to the system, actors can spawn more actors

   */

#[derive(Clone)]
pub struct System {
    inner: Arc<RwLock<SystemInner>>
}

impl System {
    /// Spawn an Actor of type A, returning a handle to its inbox
    pub fn spawn<A: Actor + Send + Sync + 'static>(&mut self, actor: A) -> ActorHandle<A::Message> {
        let (sender, inbox) = mpsc::unbounded::<A::Message>();
        let cx = ActorContext{ system: self.clone(), handle: ActorHandle::Channel(sender.clone())};
        let dyn_actor: Box<dyn ActorProc + Send + Sync + 'static> = Box::new(ActorCell{ actor, inbox, cx });
        self.inner.write().spawn(dyn_actor);
        ActorHandle::Channel(sender)
    }

    /// Run the system using the given Executor. This allows the System to spawn further tasks onto
    /// the executor during execution
    // TODO - abstract using a trait for the Executor?
    pub fn run(self, executor: fasync::Executor) -> Result<(),Error> {
        self.inner.write().run(executor)
    }

    /// Create a new System
    pub fn new() -> System {
        System { inner: Arc::new(RwLock::new(SystemInner::new())) }
    }
}

/// Inner implementation details of the Actor System
struct SystemInner {
    actors: Vec<ActorStream>,
    executor: Option<fasync::Executor>
}

impl SystemInner {
    fn new() -> SystemInner {
        SystemInner {
            actors: vec![],
            executor: None
        }
    }
    fn spawn(&mut self, actor: Box<dyn ActorProc + Send + Sync + 'static>) {
        /*
        if let Some(executor) = self.executor {
            executor.spawn(runActor(a, self));
        }
        */
        self.actors.push(ActorStream{ inner: actor })
    }

    fn run(&mut self, executor: fasync::Executor) -> Result<(),Error> {
        if self.executor.is_some() {
            return Err(err_msg("Actor System is already running"));
        }
        self.executor = Some(executor);
        if let Some(ref _ex) = self.executor {
            for _a in self.actors.iter() {
                //ex.spawn(runActor(a, self));
            }
            //ex.run_until_stalled();
        }
        Ok(())
    }
}

/// A mailbox that can receive messages of type `Msg`
pub enum ActorHandle<Msg> {
    Channel(mpsc::UnboundedSender<Msg>),
    Indirect(Arc<dyn IsHandle<Msg> + Send + Sync + 'static>)
}

pub trait IsHandle<Msg> {
    fn send(&self, message: Msg);
}

impl<Msg: Send + Sync + 'static> ActorHandle<Msg> {
    pub fn send(&mut self, message: Msg) {
        match self {
            ActorHandle::Channel(chan) => {
               // TODO - use this result
                let _result = chan.unbounded_send(message);
            },
            ActorHandle::Indirect(handle) => handle.send(message),
        }
    }

    pub fn contramap<T,F>(&self, f: F) -> ActorHandle<T>
        where F: Fn(T) -> Msg + Send + Sync + 'static,
              T: Send + Sync + 'static {
            ActorHandle::Indirect(Arc::new(MappedHandle{ handle: self.clone(), f }))
        }
}

struct MappedHandle<T,F> {
    handle: ActorHandle<T>,
    f: F
}

impl<Msg,F,T> IsHandle<Msg> for MappedHandle<T,F>
    where F: Fn(Msg) -> T,
          Msg: Send + Sync + 'static,
          T: Send + Sync + 'static {
        fn send(&self, message: Msg) {
            self.handle.clone().send((self.f)(message))
        }
}

impl<Msg> Clone for ActorHandle<Msg> {
    fn clone(&self) -> ActorHandle<Msg> {
        match self {
            ActorHandle::Channel(chan) => ActorHandle::Channel( chan.clone() ),
            ActorHandle::Indirect(handle) => ActorHandle::Indirect( handle.clone() )
        }

    }
}

struct OneShotActor<T> ( Option<oneshot::Sender<T>> );

impl<T : Send + Sync + 'static> Actor for OneShotActor<T> {
    type Message = T;

    fn update(&mut self, msg: Self::Message, _context: ActorContext<Self>) {
        match self.0.take() {
            Some(chan) => {
                let _ignore_result = chan.send(msg);
            }
            None => () // do nothing
        }
    }
}

/*
impl<T : Send + Sync + 'static> Actor for oneshot::Sender<T> {
    type Message = T;

    fn update(&mut self, msg: Self::Message, _context: ActorContext<Self>) {
        let _ignore_result = self.send(msg);
    }
}
*/

/// Implements the *ask* pattern, whereby we send a message to an actor
/// passing the handle of a oneshot actor to receive the response message
/// and returna a `Future` that will be completed when the oneshot actor
/// receives the response
pub fn ask<Msg, Response, F>(system: &mut System, target: &mut ActorHandle<Msg>, construct_msg: F) -> impl Future<Output = Result<Response, oneshot::Canceled>>
    where F: FnOnce(ActorHandle<Response>) -> Msg,
          Msg: Send + Sync + 'static,
          Response: Send + Sync + 'static {
        let (sender, receiver) = oneshot::channel::<Response>();
        let handle = system.spawn(OneShotActor(Some(sender)));
        target.send(construct_msg(handle));
        receiver
}
