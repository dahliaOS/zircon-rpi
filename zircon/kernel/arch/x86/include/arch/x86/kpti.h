// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <lib/code_patching.h>

/* When set in PML4, indicates that a PML4 is a uPML4 rather than a kPML4. */
#define USER_PML4_BIT 0x1000

/* helpers codepatching KPTI address space switching in kernel entry points */
#define BEGIN_KPTI_ASPACE_SWITCH(label) .L ## label :
#define END_KPTI_ASPACE_SWITCH(label) \
        .L ## label ## _end: \
        APPLY_CODE_PATCH_FUNC_WITH_DEFAULT(x86_kpti_codepatch, .L ## label, .L ## label ## _end - .L ## label)
