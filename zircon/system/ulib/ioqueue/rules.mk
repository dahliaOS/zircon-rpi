# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)
MODULE_TYPE := userlib

MODULE_SRCS := \
    $(LOCAL_DIR)/queue.cpp \
    $(LOCAL_DIR)/scheduler.cpp \
    $(LOCAL_DIR)/stream.cpp \
    $(LOCAL_DIR)/worker.cpp \

MODULE_STATIC_LIBS := system/ulib/fbl
MODULE_LIBS := system/ulib/c system/ulib/unittest

MODULE_PACKAGE := src

include make/module.mk

MODULE := $(LOCAL_DIR).test

MODULE_TYPE := usertest

MODULE_NAME := ioqueue-test

MODULE_SRCS := \
    $(LOCAL_DIR)/tests/ioqueue-test.cpp \
    $(LOCAL_DIR)/tests/main.cpp \

MODULE_STATIC_LIBS := \
    system/ulib/fbl \
    system/ulib/ioqueue \
    system/ulib/zxcpp \

MODULE_LIBS := \
    system/ulib/c \
    system/ulib/fdio \
    system/ulib/driver \
    system/ulib/zircon \
    system/ulib/unittest \

MODULE_PACKAGE := src

include make/module.mk
