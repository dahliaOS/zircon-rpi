use std::rc::Rc;

pub struct Device { }

impl Device {
    pub fn new() -> Rc<Self> {
        let dev = Device {


        };
        Rc::new(dev)
    }
}


