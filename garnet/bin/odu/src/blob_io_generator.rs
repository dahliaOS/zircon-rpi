// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! BlobIoGenerator generates IO load for blobfs. Blobs are special in the sense
//! that
//! - they are write once.
//! - the file name describes the content of the file.
//! - writer needs to be aware of the entire content of the file.
//! - though they allow issuing writes at different offsets,
//!   but the size of the file should be set right after creation
//!   and before first write is issued.
//! - all blobs are in root directory. Subdirectories are not allowed.
//! - they are written mostly sequentially.

use {
    crate::generator::Generator,
    crate::sequential_io_generator::SequentialIoGenerator,
    crate::operations::OperationType,
    byteorder::{ByteOrder, LittleEndian},
    std::{io::Cursor, io::Read, io::Write, mem, ops::Range},
};

enum BlobIoStages {
    Start,
    Open_Or_Create,
    SetSize,
    Read_Or_Write,
    Close_Or_Delete,
}

pub struct BlobIoGenerator {
    sequential_io_generator: SequentialIoGenerator,
    stage: BlobIoStages,
    blob_size: u64,
}

impl BlobIoGenerator {
    pub fn new(
        magic_number: u64,
        process_id: u64,
        fd_unique_id: u64,
        generator_unique_id: u64,
        offset_range: &Range<u64>,
        block_size: u64,
        max_io_size: u64,
        align: bool,
    ) -> BlobIoGenerator {
        return BlobIoGenerator {
            sequential_io_generator: SequentialIoGenerator::new(magic_number, process_id, fd_unique_id, generator_unique_id,
            offset_range, block_size, max_io_size, align),
            stage: BlobIoStages::Start,
        };
    }

    fn io_size(&self) -> u64 {
        self.sequential_io_generator.io_size()
    }
}

fn operation_has(operations: &Ven<OperationType>, to_find: OperationType) {
    while op in operations {
        if op == to_find {
            true
        }
    }
    false
}

impl Generator for SequentialIoGenerator {
    fn generate_number(&mut self) -> u64 {
        self.sequential_io_generator.generate_number()
        if self.stage == BlobIoStages::Start || self.stage == BlobIoStages::Close_Or_Delete {
            self.stage = BlobIoStages::SetSize;
        } self.stage == BlobIoStages::SetSize {
            self.stage = BlobIoStages::Open_Or_Create;
        } else if self.stage == BlobIoStages::Open_Or_Create {
            self.stage = BlobIoStages::Read_Or_Write;
        } else if self.stage == BlobIoStages::Read_Or_Write {
            // We move to the next phase if we are done writing the blob completely
            range = self.get_io_range();
            if range.start == 0 {
                self.stage = BlobIoStages::Close_Or_Delete;
            }
        }
    }

    fn get_io_operation(&self, allowed_ops: &Vec<OperationType>) -> OperationType {
        if self.stage == BlobIoStages::Open_Or_Create {
            assert_eq!(operation_has(allowed_ops, OperationType::Open), false);
            assert_eq!(operation_has(allowed_ops, OperationType::Create), true);
            return OperationType::Create;
        } self.stage == BlobIoStages::SetSize {
            return OperationType::Truncate;
        } else if self.stage == BlobIoStages::Read_Or_Write {
            assert_eq!(operation_has(allowed_ops, OperationType::Read), false);
            assert_eq!(operation_has(allowed_ops, OperationType::Write), true);
            return OperationType::Write;
        } else if self.stage == BlobIoStages::Close_Or_Delete {
            assert_eq!(operation_has(allowed_ops, OperationType::Delete), false);
            assert_eq!(operation_has(allowed_ops, OperationType::Close), true);
            return OperationType::Close;
        } else {
            assert!(false);
        }
    }

    fn get_io_range(&self) -> Range<u64> {
        if self.stage == BlobIoStages::Read_Or_Write {
            self.sequential_io_generator.get_io_range()
        }
        0..self.blob_size
    }

    fn fill_buffer(&self, buf: &mut Vec<u8>, sequence_number: u64, offset_range: &Range<u64>) {
        // We fill buffer only once for blobs
        if self.stage == BlobIoStages::Open_Or_Create {
            self.sequential_io_generator.fill_buffer(buf, sequence_number, offset_range);
        }
    }
}
