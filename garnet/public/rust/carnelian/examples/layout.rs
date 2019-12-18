// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]

use carnelian::{
    make_app_assistant, measure_text, App, AppAssistant, Canvas, Color, Coord, FontDescription,
    FontFace, MappingPixelSink, Paint, Point, Rect, Size, ViewAssistant, ViewAssistantContext,
    ViewAssistantPtr, ViewKey, ViewMode, COORD_INFINITY,
};
use failure::Error;
use lazy_static::lazy_static;
use std::{
    collections::HashMap,
    env,
    io::{self, Read},
    thread,
};

// This font creation method isn't ideal. The correct method would be to ask the Fuchsia
// font service for the font data.
static FONT_DATA: &'static [u8] =
    include_bytes!("../../../../../prebuilt/third_party/fonts/robotoslab/RobotoSlab-Regular.ttf");

lazy_static! {
    pub static ref FONT_FACE: FontFace<'static> =
        FontFace::new(&FONT_DATA).expect("Failed to create font");
}

const BASELINE: i32 = 0;

fn make_font_description<'a, 'b>(size: u32, baseline: i32) -> FontDescription<'a, 'b> {
    FontDescription { face: &FONT_FACE, size: size, baseline: baseline }
}

type FacetId = id_tree::NodeId;
type FacetTree = id_tree::Tree<()>;

/// Convenience function that can be called from main and causes the Fuchsia process being
/// run over ssh to be terminated when the user hits control-C.
pub fn wait_for_close() {
    if let Some(argument) = env::args().next() {
        if !argument.starts_with("/tmp") {
            return;
        }
    }

    thread::spawn(move || loop {
        let mut input = [0; 1];
        match io::stdin().read_exact(&mut input) {
            Ok(()) => {}
            Err(_) => {
                std::process::exit(0);
            }
        }
    });
}

pub enum FlexFit {
    Tight,
    Loose,
}

#[derive(Clone, Copy, Debug)]
pub struct BoxConstraints {
    minimum: Size,
    maximum: Size,
}

impl BoxConstraints {
    pub fn new() -> BoxConstraints {
        BoxConstraints { minimum: Size::zero(), maximum: Size::new(COORD_INFINITY, COORD_INFINITY) }
    }

    pub fn tight(size: &Size) -> BoxConstraints {
        BoxConstraints { minimum: *size, maximum: *size }
    }

    pub fn loose(size: &Size) -> BoxConstraints {
        BoxConstraints { minimum: Size::zero(), maximum: *size }
    }
}

#[derive(Debug)]
pub enum LayoutResult {
    Size(Size),
    RequestChild(FacetId, BoxConstraints),
}

pub trait Facet {
    // derived from https://github.com/xi-editor/druid/blob/master/src/widget/mod.rs#L62
    fn layout(
        &mut self,
        bc: &BoxConstraints,
        children: &mut id_tree::ChildrenIds,
        size: Option<Size>,
        _bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
    ) -> LayoutResult {
        if let Some(size) = size {
            LayoutResult::Size(size)
        } else {
            LayoutResult::RequestChild(children.next().expect("children").clone(), *bc)
        }
    }

    fn update(
        &mut self,
        bounds: &Rect,
        _canvas: &mut Canvas<MappingPixelSink>,
    ) -> Result<(), Error>;
}

pub type FacetPtr = Box<dyn Facet>;

//
// BoxFacet
//
struct BoxFacet {
    color: Color,
}

impl BoxFacet {
    pub fn new(color: Color) -> FacetPtr {
        let box_facet = BoxFacet { color };
        Box::new(box_facet)
    }
}

impl Facet for BoxFacet {
    fn layout(
        &mut self,
        bc: &BoxConstraints,
        _children: &mut id_tree::ChildrenIds,
        _size: Option<Size>,
        _bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
    ) -> LayoutResult {
        LayoutResult::Size(bc.maximum)
    }

    fn update(
        &mut self,
        bounds: &Rect,
        canvas: &mut Canvas<MappingPixelSink>,
    ) -> Result<(), Error> {
        canvas.fill_rect(bounds, self.color);
        Ok(())
    }
}

//
// PaddingFacet
//

struct PaddingFacet {
    left: Coord,
    top: Coord,
    right: Coord,
    bottom: Coord,
}

impl PaddingFacet {
    // pub fn uniform(amount: Coord) -> FacetPtr {
    //     Box::new(PaddingFacet { left: amount, top: amount, right: amount, bottom: amount })
    // }

    pub fn new(top_left: Size, bottom_right: Size) -> FacetPtr {
        Box::new(PaddingFacet {
            left: top_left.width,
            top: top_left.height,
            right: bottom_right.width,
            bottom: bottom_right.height,
        })
    }
}

impl Facet for PaddingFacet {
    fn layout(
        &mut self,
        bc: &BoxConstraints,
        children: &mut id_tree::ChildrenIds,
        size: Option<Size>,
        bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
    ) -> LayoutResult {
        let horizontal_padding = self.left + self.right;
        let vertical_padding = self.top + self.bottom;
        let first_child = children.next().expect("padding child");
        if let Some(size) = size {
            let bounds = Rect::new(Point::new(self.left, self.top), size);
            bounds_map.insert(first_child.clone(), bounds);
            LayoutResult::Size(bc.maximum)
        } else {
            let min = Size::new(
                bc.minimum.width - horizontal_padding,
                bc.minimum.height - vertical_padding,
            );
            let max = Size::new(
                bc.maximum.width - horizontal_padding,
                bc.maximum.height - vertical_padding,
            );
            LayoutResult::RequestChild(
                first_child.clone(),
                BoxConstraints { minimum: min, maximum: max },
            )
        }
    }

    fn update(
        &mut self,
        _bounds_map: &Rect,
        _canvas: &mut Canvas<MappingPixelSink>,
    ) -> Result<(), Error> {
        Ok(())
    }
}

//
// StackFacet
//
struct StackFacet {
    child_index: usize,
}

impl StackFacet {
    pub fn new() -> FacetPtr {
        Box::new(StackFacet { child_index: 0 })
    }
}

impl Facet for StackFacet {
    fn layout(
        &mut self,
        bc: &BoxConstraints,
        children: &mut id_tree::ChildrenIds,
        size: Option<Size>,
        bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
    ) -> LayoutResult {
        if let Some(child) = children.nth(self.child_index) {
            if let Some(size) = size {
                let offset_size = (bc.maximum - size) / 2.0;
                let bounds_origin = Point::new(offset_size.width, offset_size.height);
                let bounds = Rect::new(bounds_origin, size);
                bounds_map.insert(child.clone(), bounds);
                self.child_index += 1;
                if let Some(child) = children.next() {
                    let new_box_constraints = BoxConstraints::loose(&bc.maximum);
                    LayoutResult::RequestChild(child.clone(), new_box_constraints)
                } else {
                    self.child_index = 0;
                    LayoutResult::Size(bc.maximum)
                }
            } else {
                let new_box_constraints = BoxConstraints::loose(&bc.maximum);
                LayoutResult::RequestChild(child.clone(), new_box_constraints)
            }
        } else {
            self.child_index = 0;
            LayoutResult::Size(bc.maximum)
        }
    }

    fn update(
        &mut self,
        _bounds_map: &Rect,
        _canvas: &mut Canvas<MappingPixelSink>,
    ) -> Result<(), Error> {
        Ok(())
    }
}

//
// LabelFacet
//

struct LabelFacet {
    text: String,
    size: u32,
    paint: Paint,
}

impl LabelFacet {
    /// Create a new label
    pub fn new(text: &str, size: u32, paint: Paint) -> FacetPtr {
        Box::new(LabelFacet { text: text.to_string(), size, paint })
    }
}

impl Facet for LabelFacet {
    fn layout(
        &mut self,
        bc: &BoxConstraints,
        _children: &mut id_tree::ChildrenIds,
        _size: Option<Size>,
        _bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
    ) -> LayoutResult {
        let mut font_description = make_font_description(self.size, BASELINE);
        let text_size = measure_text(&self.text, &mut font_description);
        let clamped_size = text_size.clamp(bc.minimum, bc.maximum);
        println!("LabelFacet layout {} {:?} {}", text_size, bc, clamped_size);
        LayoutResult::Size(text_size.clamp(bc.minimum, bc.maximum))
    }

    fn update(
        &mut self,
        bounds: &Rect,
        canvas: &mut Canvas<MappingPixelSink>,
    ) -> Result<(), Error> {
        canvas.fill_rect(&bounds, self.paint.bg);

        // Fill the text
        canvas.fill_text(
            &self.text,
            bounds.origin,
            &mut make_font_description(self.size, BASELINE),
            &self.paint,
        );
        Ok(())
    }
}

#[derive(Default)]
struct LayoutAppAssistant;

impl AppAssistant for LayoutAppAssistant {
    fn setup(&mut self) -> Result<(), Error> {
        Ok(())
    }

    fn create_view_assistant_canvas(&mut self, _: ViewKey) -> Result<ViewAssistantPtr, Error> {
        Ok(Box::new(LayoutViewAssistant::new()?))
    }

    fn get_mode(&self) -> ViewMode {
        ViewMode::Canvas
    }
}

struct LayoutContext {
    pub tree: FacetTree,
    pub facets: HashMap<id_tree::NodeId, FacetPtr>,
    pub bounds: HashMap<id_tree::NodeId, Rect>,
}

impl LayoutContext {
    pub fn new() -> Result<LayoutContext, Error> {
        Ok(LayoutContext {
            tree: id_tree::Tree::new(),
            facets: HashMap::new(),
            bounds: HashMap::new(),
        })
    }

    pub fn add_facet(
        &mut self,
        facet: FacetPtr,
        insert_behavior: id_tree::InsertBehavior,
    ) -> Result<FacetId, Error> {
        let facet_id = self.tree.insert(id_tree::Node::new(()), insert_behavior)?;
        self.facets.insert(facet_id.clone(), facet);
        Ok(facet_id)
    }

    pub fn layout(&mut self, bc: &BoxConstraints) {
        fn layout_recursive(
            facets: &mut HashMap<FacetId, FacetPtr>,
            bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
            tree: &FacetTree,
            bc: &BoxConstraints,
            facet: &FacetId,
        ) -> Size {
            let mut size = None;
            loop {
                let layout_res = facets.get_mut(&facet).expect("facet").layout(
                    bc,
                    &mut tree.children_ids(&facet).expect("children_for_layout"),
                    size,
                    bounds_map,
                );
                match layout_res {
                    LayoutResult::Size(size) => {
                        return size;
                    }
                    LayoutResult::RequestChild(child, child_bc) => {
                        size = Some(layout_recursive(facets, bounds_map, tree, &child_bc, &child));
                    }
                }
            }
        }

        if let Some(root_id) = self.tree.root_node_id() {
            layout_recursive(&mut self.facets, &mut self.bounds, &self.tree, bc, root_id);
        }
    }
}

struct LayoutViewAssistant {
    layout_context: LayoutContext,
    bg_color: Color,
    fg_color: Color,
}

impl LayoutViewAssistant {
    pub fn new() -> Result<LayoutViewAssistant, Error> {
        let bg_color = Color::from_hash_code("#303030")?;
        let fg_color = Color::from_hash_code("#FF00FF")?;
        let mut view_assistant =
            LayoutViewAssistant { layout_context: LayoutContext::new()?, bg_color, fg_color };
        view_assistant.setup_facets()?;
        Ok(view_assistant)
    }

    fn setup_facets(&mut self) -> Result<(), Error> {
        let root_id =
            self.layout_context.add_facet(StackFacet::new(), id_tree::InsertBehavior::AsRoot)?;

        let _ = self.layout_context.add_facet(
            BoxFacet::new(self.bg_color),
            id_tree::InsertBehavior::UnderNode(&root_id),
        )?;

        let padding_id = self.layout_context.add_facet(
            PaddingFacet::new(Size::new(200.0, 150.0), Size::new(20.0, 20.0)),
            id_tree::InsertBehavior::UnderNode(&root_id),
        )?;

        let inner_stack_id = self
            .layout_context
            .add_facet(StackFacet::new(), id_tree::InsertBehavior::UnderNode(&padding_id))?;

        let _ = self.layout_context.add_facet(
            BoxFacet::new(self.fg_color),
            id_tree::InsertBehavior::UnderNode(&inner_stack_id),
        )?;

        let paint = Paint { fg: Color::white(), bg: self.fg_color };

        self.layout_context.add_facet(
            LabelFacet::new("Now is the Time", 128, paint),
            id_tree::InsertBehavior::UnderNode(&inner_stack_id),
        )?;

        Ok(())
    }

    fn layout(&mut self, bc: &BoxConstraints) {
        self.layout_context.layout(bc);
    }

    fn update(&mut self, canvas: &mut Canvas<MappingPixelSink>) {
        fn update_recursive(
            facet: &FacetId,
            location: &Point,
            bounds_map: &HashMap<id_tree::NodeId, Rect>,
            facets: &mut HashMap<FacetId, FacetPtr>,
            tree: &FacetTree,
            canvas: &mut Canvas<MappingPixelSink>,
        ) {
            let zero = Rect::zero();
            let bounds = bounds_map.get(facet).unwrap_or(&zero).translate(&location.to_vector());
            facets
                .get_mut(&facet)
                .expect("facet in update")
                .update(&bounds, canvas)
                .expect("painting");
            for child_facet_id in tree.children_ids(facet).expect("children_ids") {
                update_recursive(child_facet_id, &bounds.origin, bounds_map, facets, tree, canvas);
            }
        }
        let root_id = self.layout_context.tree.root_node_id().expect("root_node_id");
        let location = Point::zero();
        update_recursive(
            root_id,
            &location,
            &self.layout_context.bounds,
            &mut self.layout_context.facets,
            &self.layout_context.tree,
            canvas,
        );
    }
}

impl ViewAssistant for LayoutViewAssistant {
    fn setup(&mut self, _context: &ViewAssistantContext) -> Result<(), Error> {
        Ok(())
    }

    fn update(&mut self, context: &ViewAssistantContext) -> Result<(), Error> {
        let canvas = &mut context.canvas.as_ref().unwrap().borrow_mut();
        let box_constraints = BoxConstraints::tight(&context.size);
        self.layout(&box_constraints);
        self.update(canvas);
        Ok(())
    }
}

fn main() -> Result<(), Error> {
    wait_for_close();
    App::run(make_app_assistant::<LayoutAppAssistant>())
}

#[cfg(test)]
mod tests {
    use crate::{BoxConstraints, Facet, FacetPtr, LayoutContext, LayoutResult, StackFacet};
    use carnelian::{Canvas, MappingPixelSink, Point, Rect, Size};
    use failure::Error;
    use std::collections::HashMap;

    struct TestBoxFacet {
        size: Size,
    }

    impl TestBoxFacet {
        pub fn new(size: Size) -> FacetPtr {
            let test_box_facet = TestBoxFacet { size };
            Box::new(test_box_facet)
        }
    }

    impl Facet for TestBoxFacet {
        fn layout(
            &mut self,
            bc: &BoxConstraints,
            _children: &mut id_tree::ChildrenIds,
            _size: Option<Size>,
            _bounds_map: &mut HashMap<id_tree::NodeId, Rect>,
        ) -> LayoutResult {
            let clamped_size = self.size.clamp(bc.minimum, bc.maximum);
            LayoutResult::Size(clamped_size)
        }

        fn update(
            &mut self,
            _bounds: &Rect,
            _canvas: &mut Canvas<MappingPixelSink>,
        ) -> Result<(), Error> {
            panic!("Update not implemented on test facets");
        }
    }

    #[test]
    fn test_stack() -> Result<(), Error> {
        let test_box_size = Size::new(200.0, 200.0);
        let mut layout_context = LayoutContext::new()?;
        let root_id =
            layout_context.add_facet(StackFacet::new(), id_tree::InsertBehavior::AsRoot)?;
        let box_id = layout_context.add_facet(
            TestBoxFacet::new(test_box_size),
            id_tree::InsertBehavior::UnderNode(&root_id),
        )?;
        let test_layout_size = Size::new(800.0, 600.0);
        let box_constraints = BoxConstraints::tight(&test_layout_size);
        layout_context.layout(&box_constraints);
        let box_bounds = layout_context.bounds.get(&box_id).unwrap();
        let expected_bounds = Rect::new(Point::new(300.0, 200.0), test_box_size);
        assert_eq!(
            box_bounds, &expected_bounds,
            "Expected bounds of {}, got {}",
            expected_bounds, box_bounds
        );
        Ok(())
    }
}
