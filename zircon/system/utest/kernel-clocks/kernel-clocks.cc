// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <lib/affine/transform.h>
#include <lib/zx/clock.h>
#include <zircon/syscalls/clock.h>
#include <zxtest/zxtest.h>

namespace {

// Unpack a zx_clock_transformation_t from a syscall result and put it into an
// affine::Transform so we can call methods on it.
inline affine::Transform UnpackTransform(const zx_clock_transformation_t& ct) {
    return affine::Transform { ct.reference_offset,
                               ct.synthetic_offset,
                               { ct.rate.synthetic_ticks, ct.rate.reference_ticks } };
}

// Unpack a zx_clock_rate_t from a syscall result and put it into an
// affine::Ratio so we can call methods on it.
inline affine::Ratio UnpackRatio(const zx_clock_rate_t& rate) {
    return affine::Ratio { rate.synthetic_ticks, rate.reference_ticks };
}

TEST(KernelClocksTestCase, Create) {
    zx::clock clock;

    // Creating a clock with no special options should succeed.
    ASSERT_OK(zx::clock::create(0, &clock));

    // Creating a monotonic clock should succeed.
    ASSERT_OK(zx::clock::create(ZX_CLOCK_OPT_MONOTONIC, &clock));

    // Creating a monotonic + continuous clock should succeed.
    ASSERT_OK(zx::clock::create(ZX_CLOCK_OPT_MONOTONIC | ZX_CLOCK_OPT_CONTINUOUS, &clock));

    // Creating a continuous clock, but failing to say that it is also
    // monotonic, should fail.  The arguments are invalid.
    ASSERT_EQ(zx::clock::create(ZX_CLOCK_OPT_CONTINUOUS, &clock), ZX_ERR_INVALID_ARGS);

    // Attempting to create a clock with any currently undefined option flags
    // should fail.  The arguments are invalid.
    constexpr uint32_t ILLEGAL_OPT = 0x80000000;
    static_assert((ZX_CLOCK_OPTS_ALL & ILLEGAL_OPT) == 0, "Illegal option is actually legal!");
    ASSERT_EQ(zx::clock::create(ILLEGAL_OPT, &clock), ZX_ERR_INVALID_ARGS);
}

TEST(KernelClocksTestCase, Read) {
    zx::clock the_clock;
    int64_t read_val;

    // Create a basic clock.
    ASSERT_OK(zx::clock::create(0, &the_clock));

    // Attempt to read the clock.  It has never been set before, so it should
    // zero.
    ASSERT_OK(the_clock.read(&read_val));
    ASSERT_EQ(0, read_val);

    // Wait a bit and try again.  It should still read zero; synthetic clocks do
    // not start to tick until after their first update.
    zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    ASSERT_OK(the_clock.read(&read_val));
    ASSERT_EQ(0, read_val);

    // Set the clock to a time.  Record clock monotonic before and after we
    // perform the initial update operation.  While we cannot control the exact
    // time at which the set operation will take place, we can bound the range
    // of possible transformations and establish a min and max.
    constexpr int64_t INITIAL_VALUE =  1'000'000;
    zx::clock::update_args args;
    args.set_value(INITIAL_VALUE);

    zx::time before_update = zx::clock::get_monotonic();
    ASSERT_OK(the_clock.update(args));
    zx::time after_update = zx::clock::get_monotonic();

    // Now read the clock, and make sure that the value we read makes sense
    // given our bounds.
    zx::time before_read = zx::clock::get_monotonic();
    ASSERT_OK(the_clock.read(&read_val));
    zx::time after_read = zx::clock::get_monotonic();

    // Compute the minimum and maximum values we should be able to get from our
    // read operation based on the various bounds we have established.
    affine::Transform min_function{ after_update.get(), INITIAL_VALUE, {} };
    affine::Transform max_function{ before_update.get(), INITIAL_VALUE, {} };
    int64_t min_expected = min_function.Apply(before_read.get());
    int64_t max_expected = max_function.Apply(after_read.get());

    ASSERT_GE(read_val, min_expected);
    ASSERT_LE(read_val, max_expected);

    // Remove the READ rights from the clock, then verify that we can no longer read the clock.
    ASSERT_OK(the_clock.replace(ZX_DEFAULT_CLOCK_RIGHTS & ~ZX_RIGHT_READ, &the_clock));
    ASSERT_EQ(the_clock.read(&read_val), ZX_ERR_ACCESS_DENIED);
}

TEST(KernelClocksTestCase, GetDeatils) {
    // Create the 3 types of clocks (basic, monotonic, and monotonic +
    // continuous), then make sure that get_details behaves properly for each
    // clock type as we update the clocks.
    std::array OPTIONS {
        static_cast<uint32_t>(0),
        ZX_CLOCK_OPT_MONOTONIC,
        ZX_CLOCK_OPT_MONOTONIC | ZX_CLOCK_OPT_CONTINUOUS,
    };

    for (const auto options : OPTIONS) {
        // Create the clock
        zx::clock the_clock;
        ASSERT_OK(zx::clock::create(options, &the_clock));

        ////////////////////////////////////////////////////////////////////////
        //
        // Phase 1: Fetch the initial details
        //
        ////////////////////////////////////////////////////////////////////////
        zx_clock_details_t details;
        zx::ticks get_details_before = zx::ticks::now();
        ASSERT_OK(the_clock.get_details(&details));
        zx::ticks get_details_after = zx::ticks::now();

        // Check the generation counter.  It does not have a defined starting
        // value, but it should always be even.  An odd generation counter
        // indicates a clock which is in the process of being updates (something
        // we should never see when querying details)
        ASSERT_TRUE((details.generation_counter & 0x1) == 0);

        // The options reported should match those used to create the clock.
        ASSERT_EQ(options, details.options);

        // The |query_ticks| field of the details should indicate that this
        // clock was queried sometime between the before and after times latched
        // above.
        ASSERT_GE(details.query_ticks, get_details_before.get());
        ASSERT_LE(details.query_ticks, get_details_after.get());

        // The error bound should default to "unknown"
        ASSERT_EQ(ZX_CLOCK_UNKNOWN_ERROR, details.error_bound);

        // None of the dynamic properties of the clock have ever been set.
        // Their last update times should be 0.
        ASSERT_EQ(0, details.last_value_update_ticks);
        ASSERT_EQ(0, details.last_rate_adjust_update_ticks);
        ASSERT_EQ(0, details.last_error_bounds_update_ticks);

        // Both initial transformations should indicate that the clock has never
        // been set.  This is done by setting the numerator of the
        // transformation to 0, effectively stopping the synthetic clock.
        ASSERT_EQ(0, details.ticks_to_synthetic.rate.synthetic_ticks);
        ASSERT_EQ(0, details.mono_to_synthetic.rate.synthetic_ticks);

        // Record the details we just observed so we can observe how they change
        // as we update.
        zx_clock_details_t last_details = details;

        ////////////////////////////////////////////////////////////////////////
        //
        // Phase 2: Set the initial value of the clock, then sanity check the
        // details.
        //
        ////////////////////////////////////////////////////////////////////////
        constexpr int64_t INITIAL_VALUE =  1'000'000;
        zx::ticks update_before = zx::ticks::now();
        ASSERT_OK(the_clock.update(zx::clock::update_args{}.set_value(INITIAL_VALUE)));
        zx::ticks update_after = zx::ticks::now();

        get_details_before = zx::ticks::now();
        ASSERT_OK(the_clock.get_details(&details));
        get_details_after = zx::ticks::now();

        // Sanity check the query time
        ASSERT_GE(details.query_ticks, get_details_before.get());
        ASSERT_LE(details.query_ticks, get_details_after.get());

        // The generation counter should have incremented by exactly 2.
        ASSERT_EQ(last_details.generation_counter + 2, details.generation_counter);

        // The options should not have changed.
        ASSERT_EQ(options, details.options);

        // The error bound should still be "unknown"
        ASSERT_EQ(ZX_CLOCK_UNKNOWN_ERROR, details.error_bound);

        // The last value update time should be between the ticks that we
        // latched above.  Since this was the initial clock set operation, the
        // last rate adjustment time should update as well.  Even though we
        // didn't request it explicitly, the rate did go from stopped to
        // running.
        ASSERT_GE(details.last_value_update_ticks, update_before.get());
        ASSERT_LE(details.last_value_update_ticks, update_after.get());
        ASSERT_EQ(details.last_value_update_ticks, details.last_rate_adjust_update_ticks);
        ASSERT_EQ(details.last_value_update_ticks, details.last_rate_adjust_update_ticks);
        ASSERT_EQ(last_details.last_error_bounds_update_ticks,
                  details.last_error_bounds_update_ticks);

        // The synthetic clock offset for both transformations should be the
        // initial value we set for the clock.
        ASSERT_EQ(INITIAL_VALUE, details.ticks_to_synthetic.synthetic_offset);
        ASSERT_EQ(INITIAL_VALUE, details.mono_to_synthetic.synthetic_offset);

        // The rate of the mono <-> synthetic transformation should be 1:1.  We
        // have not adjusted its rate yet, and its nominal rate is the same as
        // clock monotonic.
        ASSERT_EQ(1, details.mono_to_synthetic.rate.synthetic_ticks);
        ASSERT_EQ(1, details.mono_to_synthetic.rate.reference_ticks);

        // The expected ticks reference should be the update time.
        //
        // Note: this validation behavior assumes a particular behavior of the
        // kernel's update implementation.  Technically, there are many valid
        // solutions for computing this equation; the two offsets allow us to
        // write the equation for a line many different ways.  Even so, we
        // expect the kernel to be using the method we validate here because it
        // is simple, cheap, and precise.
        ASSERT_EQ(details.last_value_update_ticks, details.ticks_to_synthetic.reference_offset);

        // The rate of the ticks <-> synthetic should be equal to the ticks to
        // clock monotonic ratio.  Right now, however, we don't have a good way
        // to query the VDSO constants in order to find this ratio.  Instead, we
        // take it on faith that this is correct, then use the ratio to compute
        // and check the mono <-> synthetic reference offset.
        //
        // TODO(johngro): consider exposing this ratio from a VDSO based
        // syscall.
        int64_t expected_mono_reference;
        affine::Ratio ticks_to_mono = UnpackRatio( details.ticks_to_synthetic.rate );
        expected_mono_reference = ticks_to_mono.Scale(details.ticks_to_synthetic.reference_offset);
        ASSERT_EQ(expected_mono_reference, details.mono_to_synthetic.reference_offset);

        // Update the last_details and move on to the next phase.
        last_details = details;

        ////////////////////////////////////////////////////////////////////////
        //
        // Phase 3: Change the rate of the clock, then sanity check the details.
        //
        ////////////////////////////////////////////////////////////////////////
        constexpr int32_t PPM_ADJ = 65;
        update_before = zx::ticks::now();
        ASSERT_OK(the_clock.update(zx::clock::update_args{}.set_rate_adjust(PPM_ADJ)));
        update_after = zx::ticks::now();

        get_details_before = zx::ticks::now();
        ASSERT_OK(the_clock.get_details(&details));
        get_details_after = zx::ticks::now();

        // Sanity check the query time
        ASSERT_GE(details.query_ticks, get_details_before.get());
        ASSERT_LE(details.query_ticks, get_details_after.get());

        // The generation counter should have incremented by exactly 2.
        ASSERT_EQ(last_details.generation_counter + 2, details.generation_counter);

        // The options should not have changed.
        ASSERT_EQ(options, details.options);

        // The error bound should still be "unknown"
        ASSERT_EQ(ZX_CLOCK_UNKNOWN_ERROR, details.error_bound);

        // The last value and error bound update times should not have changed.
        // The last rate adjustment timestamp should be bounded by
        // update_before/update_after.
        ASSERT_EQ(last_details.last_value_update_ticks,
                  details.last_value_update_ticks);
        ASSERT_EQ(last_details.last_error_bounds_update_ticks,
                  details.last_error_bounds_update_ticks);
        ASSERT_GE(details.last_rate_adjust_update_ticks, update_before.get());
        ASSERT_LE(details.last_rate_adjust_update_ticks, update_after.get());

        // Validate the various transformation equations.
        //
        // Note: this validation behavior assumes a particular behavior of the
        // kernel.  Technically, there are many valid solutions for computing
        // this equation; the two offsets allow us to write the equation for a
        // line many different ways.  Even so, we expect the kernel to be using
        // the method we validate here because it is simple, cheap, and precise.
        //
        // If the behavior changes, there should be a Very Good Reason, and we
        // would like this test to break if someone decides to update the
        // methodology without updating the tests as well.

        // The expected synthetic clock offset for the transformations should be
        // the projected value of the last_rate_adjust_ticks time using the previous
        // transformation.
        int64_t expected_synth_offset;
        affine::Transform last_ticks_to_synth = UnpackTransform(last_details.ticks_to_synthetic);
        expected_synth_offset = last_ticks_to_synth.Apply(details.last_rate_adjust_update_ticks);

        ASSERT_EQ(expected_synth_offset, details.ticks_to_synthetic.synthetic_offset);
        ASSERT_EQ(expected_synth_offset, details.mono_to_synthetic.synthetic_offset);

        // The reference offset for ticks <-> synth should be the update time.
        // The reference for mono <-> synth should be the ticks reference
        // converted to mono.
        expected_mono_reference = ticks_to_mono.Scale(details.ticks_to_synthetic.reference_offset);
        ASSERT_EQ(expected_mono_reference, details.mono_to_synthetic.reference_offset);
        ASSERT_EQ(details.last_rate_adjust_update_ticks,
                  details.ticks_to_synthetic.reference_offset);

        // Check our ratios.  We need to be a bit careful here; one cannot
        // simply compare ratios for equality without reducing them first.
        //
        // The mono <-> synth ratio should just be a function of the PPM
        // adjustment we applied.
        affine::Ratio expected_mono_ratio{ 1'000'000 + PPM_ADJ, 1'000'000 };
        affine::Ratio actual_mono_ratio = UnpackRatio(details.mono_to_synthetic.rate);

        expected_mono_ratio.Reduce();
        actual_mono_ratio.Reduce();

        ASSERT_EQ(expected_mono_ratio.numerator(), actual_mono_ratio.numerator());
        ASSERT_EQ(expected_mono_ratio.denominator(), actual_mono_ratio.denominator());

        // The ticks <-> synth ratio should be the product of ticks to mono and
        // mono to synth.
        affine::Ratio expected_ticks_ratio = ticks_to_mono * expected_mono_ratio;
        affine::Ratio actual_ticks_ratio = UnpackRatio(details.ticks_to_synthetic.rate);

        expected_ticks_ratio.Reduce();
        actual_ticks_ratio.Reduce();

        ASSERT_EQ(expected_ticks_ratio.numerator(), actual_ticks_ratio.numerator());
        ASSERT_EQ(expected_ticks_ratio.denominator(), actual_ticks_ratio.denominator());

        // Update the last_details and move on to the next phase.
        last_details = details;

        ////////////////////////////////////////////////////////////////////////
        //
        // Phase 4: Update the error bound and verify that it sticks.  None
        // of the other core details should change.
        //
        ////////////////////////////////////////////////////////////////////////
        constexpr uint64_t ERROR_BOUND = 1234567;
        update_before = zx::ticks::now();
        ASSERT_OK(the_clock.update(zx::clock::update_args{}.set_error_bound(ERROR_BOUND)));
        update_after = zx::ticks::now();

        get_details_before = zx::ticks::now();
        ASSERT_OK(the_clock.get_details(&details));
        get_details_after = zx::ticks::now();

        // Sanity check the query time
        ASSERT_GE(details.query_ticks, get_details_before.get());
        ASSERT_LE(details.query_ticks, get_details_after.get());

        // The generation counter should have incremented by exactly 2.
        ASSERT_EQ(last_details.generation_counter + 2, details.generation_counter);

        // The options should not have changed.
        ASSERT_EQ(options, details.options);

        // The error bound should be what we set it to.
        ASSERT_EQ(ERROR_BOUND, details.error_bound);

        // The last value and rate adjust update times not have changed.  The
        // last error bound timestamp should be bounded by
        // update_before/update_after.
        ASSERT_EQ(last_details.last_value_update_ticks,
                  details.last_value_update_ticks);
        ASSERT_EQ(last_details.last_rate_adjust_update_ticks,
                  details.last_rate_adjust_update_ticks);
        ASSERT_GE(details.last_error_bounds_update_ticks, update_before.get());
        ASSERT_LE(details.last_error_bounds_update_ticks, update_after.get());


        // None of the transformations should have changed.
        auto CompareTransformation = [](const zx_clock_transformation_t& expected,
                                        const zx_clock_transformation_t& actual) {
            ASSERT_EQ(expected.reference_offset, expected.reference_offset);
            ASSERT_EQ(expected.synthetic_offset, expected.synthetic_offset);
            ASSERT_EQ(expected.rate.synthetic_ticks, expected.rate.synthetic_ticks);
            ASSERT_EQ(expected.rate.reference_ticks, expected.rate.reference_ticks);
        };

        ASSERT_NO_FAILURES(CompareTransformation(last_details.ticks_to_synthetic,
                                                 details.ticks_to_synthetic));
        ASSERT_NO_FAILURES(CompareTransformation(last_details.mono_to_synthetic,
                                                 details.mono_to_synthetic));

        ////////////////////////////////////////////////////////////////////////
        //
        // Phase 5: Finally, reduce the rights on the clock, discarding the READ
        // right in the process.  Make sure that we can no longer get_details.
        //
        ////////////////////////////////////////////////////////////////////////
        ASSERT_OK(the_clock.replace(ZX_DEFAULT_CLOCK_RIGHTS & ~ZX_RIGHT_READ, &the_clock));
        ASSERT_EQ(the_clock.get_details(&details), ZX_ERR_ACCESS_DENIED);
    }
}

TEST(KernelClocksTestCase, Update) {
    zx::clock basic;
    zx::clock mono;
    zx::clock mono_cont;

    // Create three clocks.  A basic clock, a monotonic clock, and a monotonic
    // + continuous clock.
    ASSERT_OK(zx::clock::create(0, &basic));
    ASSERT_OK(zx::clock::create(ZX_CLOCK_OPT_MONOTONIC, &mono));
    ASSERT_OK(zx::clock::create(ZX_CLOCK_OPT_MONOTONIC | ZX_CLOCK_OPT_CONTINUOUS, &mono_cont));

    // Set each clock to its initial value.  All clocks need to allow being
    // initially set, so this should be just fine.
    constexpr int64_t INITIAL_VALUE =  1'000'000;
    zx::clock::update_args args;
    args.set_value(INITIAL_VALUE);

    ASSERT_OK(basic.update(args));
    ASSERT_OK(mono.update(args));
    ASSERT_OK(mono_cont.update(args));

    // Attempt to make each clock jump forward.  This should succeed for the
    // basic clock and the monotonic clock, but fail for the continuous clock
    // with 'invalid args'.  Note that this operation is timing sensitive.  If
    // the clocks are permitted to advance to the point that our value is no
    // longer in the future, then the monotonic set operation will fail as well.
    // To make sure this does not happen, make the jump be something enormous;
    // much larger than the maximum conceivable test watchdog timeout.  We use a
    // full day.
    constexpr int64_t FWD_JUMP = ZX_SEC(86400);
    args.set_value(INITIAL_VALUE + FWD_JUMP);

    ASSERT_OK(basic.update(args));
    ASSERT_OK(mono.update(args));
    ASSERT_EQ(mono_cont.update(args), ZX_ERR_INVALID_ARGS);

    // Attempt to make each clock jump backwards.  This should only succeed the
    // basic clock.  Neither flavor of monotonic should allow this.
    args.set_value(INITIAL_VALUE / 2);
    ASSERT_OK(basic.update(args));
    ASSERT_EQ(mono.update(args), ZX_ERR_INVALID_ARGS);
    ASSERT_EQ(mono_cont.update(args), ZX_ERR_INVALID_ARGS);

    // Test rate adjustments.  All clocks should permit rate adjustment, but the
    // legal rate adjustment is fixed.
    struct RateTestVector {
        int32_t adj;
        zx_status_t expected_result;
    };
    constexpr std::array RATE_TEST_VECTORS = {
        RateTestVector{ 0, ZX_OK },
        RateTestVector{ ZX_CLOCK_UPDATE_MIN_RATE_ADJUST, ZX_OK },
        RateTestVector{ ZX_CLOCK_UPDATE_MAX_RATE_ADJUST, ZX_OK },
        RateTestVector{ ZX_CLOCK_UPDATE_MIN_RATE_ADJUST - 1, ZX_ERR_INVALID_ARGS },
        RateTestVector{ ZX_CLOCK_UPDATE_MAX_RATE_ADJUST + 1, ZX_ERR_INVALID_ARGS },
    };

    for (const auto& v : RATE_TEST_VECTORS) {
        args.reset().set_rate_adjust(v.adj);
        ASSERT_EQ(basic.update(args), v.expected_result);
        ASSERT_EQ(mono.update(args), v.expected_result);
        ASSERT_EQ(mono_cont.update(args), v.expected_result);
    }

    // Test error bound reporting.  Error bounds are just information which is
    // atomically updated while making adjustments to the clock.  The kernel
    // should permit any value for this.
    //
    // TODO(johngro) : Should it be legal to update the clock with a new error
    // bound, but make no adjustments to the clock at all?
    constexpr std::array ERROR_BOUND_VECTORS = {
        static_cast<uint64_t>(12345),
        std::numeric_limits<uint64_t>::min(),
        std::numeric_limits<uint64_t>::max(),
        ZX_CLOCK_UNKNOWN_ERROR,
    };

    for (const auto& err_bound : ERROR_BOUND_VECTORS) {
        args.reset().set_error_bound(err_bound);
        ASSERT_OK(basic.update(args));
        ASSERT_OK(mono.update(args));
        ASSERT_OK(mono_cont.update(args));
    }

    // Attempt to set a flag in the update structure which is illegal.  This
    // should always fail.
    constexpr uint32_t ILLEGAL_FLAG = 0x80000000;
    static_assert((ZX_CLOCK_UPDATE_FLAGS_ALL & ILLEGAL_FLAG) == 0,
                  "Illegal flag is actually legal!");

    args.flags = ILLEGAL_FLAG;
    ASSERT_EQ(basic.update(args), ZX_ERR_INVALID_ARGS);
    ASSERT_EQ(mono.update(args), ZX_ERR_INVALID_ARGS);
    ASSERT_EQ(mono_cont.update(args), ZX_ERR_INVALID_ARGS);

    // Attempt to send an update command with no valid flags at all (eg; a
    // no-op).  This should also fail.
    args.reset();
    ASSERT_EQ(basic.update(args), ZX_ERR_INVALID_ARGS);
    ASSERT_EQ(mono.update(args), ZX_ERR_INVALID_ARGS);
    ASSERT_EQ(mono_cont.update(args), ZX_ERR_INVALID_ARGS);

    // Remove the WRITE rights from the basic clock handle, then verify that we
    // can no longer update it.
    args.reset().set_rate_adjust(0);
    ASSERT_OK(basic.replace(ZX_DEFAULT_CLOCK_RIGHTS & ~ZX_RIGHT_WRITE, &basic));
    ASSERT_EQ(basic.update(args), ZX_ERR_ACCESS_DENIED);
}

}  // namespace
