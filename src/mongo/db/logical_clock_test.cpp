/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_clock_test_fixture.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

std::string kDummyNamespaceString = "test.foo";

using LogicalClockTest = LogicalClockTestFixture;

// Check that the initial time does not change during logicalClock creation.
TEST_F(LogicalClockTest, roundtrip) {
    Timestamp tX(1);
    auto time = LogicalTime(tX);

    getClock()->initClusterTimeFromTrustedSource(time);
    auto storedTime(getClock()->getClusterTime());

    ASSERT_TRUE(storedTime.getTime() == time);
}

// Verify the reserve ticks functionality.
TEST_F(LogicalClockTest, reserveTicks) {
    // Set clock to a non-zero time, so we can verify wall clock synchronization.
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    auto t1 = getClock()->reserveTicks(1);
    auto t2(getClock()->getClusterTime());
    ASSERT_TRUE(t1 == t2.getTime());

    // Make sure we synchronized with the wall clock.
    ASSERT_TRUE(t2.getTime().asTimestamp().getSecs() == 10);

    auto t3 = getClock()->reserveTicks(1);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = getClock()->reserveTicks(100);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    ASSERT_TRUE(t3 == t1);

    // Ensure overflow to a new second.
    auto initTimeSecs = getClock()->getClusterTime().getTime().asTimestamp().getSecs();
    getClock()->reserveTicks((1U << 31) - 1);
    auto newTimeSecs = getClock()->getClusterTime().getTime().asTimestamp().getSecs();
    ASSERT_TRUE(newTimeSecs == initTimeSecs + 1);
}

// Verify the advanceClusterTime functionality.
TEST_F(LogicalClockTest, advanceClusterTime) {
    auto t1 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    SignedLogicalTime l1 = makeSignedLogicalTime(t1);
    ASSERT_OK(getClock()->advanceClusterTimeFromTrustedSource(l1));
    auto l2(getClock()->getClusterTime());
    ASSERT_TRUE(l1.getTime() == l2.getTime());
}

// Verify rate limiter rejects logical times whose seconds values are too far ahead.
TEST_F(LogicalClockTest, RateLimiterRejectsLogicalTimesTooFarAhead) {
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    Timestamp tooFarAheadTimestamp(
        durationCount<Seconds>(getMockClockSourceTime().toDurationSinceEpoch()) +
            durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDrift) +
            10,  // Add 10 seconds to ensure limit is exceeded.
        1);
    SignedLogicalTime l1 = makeSignedLogicalTime(LogicalTime(tooFarAheadTimestamp));

    ASSERT_EQ(ErrorCodes::ClusterTimeFailsRateLimiter, getClock()->advanceClusterTime(l1));
    ASSERT_EQ(ErrorCodes::ClusterTimeFailsRateLimiter,
              getClock()->advanceClusterTimeFromTrustedSource(l1));
}

// Verify cluster time can be initialized to a very old time.
TEST_F(LogicalClockTest, InitFromTrustedSourceCanAcceptVeryOldLogicalTime) {
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(
        durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDrift) * 10 * 1000));

    Timestamp veryOldTimestamp(
        durationCount<Seconds>(getMockClockSourceTime().toDurationSinceEpoch()) -
        (durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDrift) * 5));
    auto veryOldTime = LogicalTime(veryOldTimestamp);
    getClock()->initClusterTimeFromTrustedSource(veryOldTime);

    ASSERT_TRUE(getClock()->getClusterTime().getTime() == veryOldTime);
}

// A clock with no TimeProofService should reject new times in advanceClusterTime.
TEST_F(LogicalClockTest, AdvanceClusterTimeFailsWithoutTimeProofService) {
    LogicalTime initialTime(Timestamp(10));
    getClock()->initClusterTimeFromTrustedSource(initialTime);

    unsetTimeProofService();

    SignedLogicalTime l1 = makeSignedLogicalTime(LogicalTime(Timestamp(100)));
    ASSERT_EQ(ErrorCodes::CannotVerifyAndSignLogicalTime, getClock()->advanceClusterTime(l1));
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == initialTime);

    resetTimeProofService();

    SignedLogicalTime l2 = makeSignedLogicalTime(LogicalTime(Timestamp(200)));
    ASSERT_OK(getClock()->advanceClusterTime(l2));
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == l2.getTime());
}

// A clock with no TimeProofService can still advance its time through certain methods.
TEST_F(LogicalClockTest, CertainMethodsCanAdvanceClockWithoutTimeProofService) {
    unsetTimeProofService();

    LogicalTime t1(Timestamp(100));
    getClock()->initClusterTimeFromTrustedSource(t1);
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == t1);

    auto t2 = getClock()->reserveTicks(1);
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == t2);

    LogicalTime t3(Timestamp(300));
    ASSERT_OK(getClock()->signAndAdvanceClusterTime(t3));
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == t3);

    SignedLogicalTime l4 = makeSignedLogicalTime(LogicalTime(Timestamp(400)));
    ASSERT_OK(getClock()->advanceClusterTimeFromTrustedSource(l4));
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == l4.getTime());
}

// Verify writes to the oplog advance cluster time.
TEST_F(LogicalClockTest, WritesToOplogAdvanceClusterTime) {
    Timestamp tX(1);
    auto initialTime = LogicalTime(tX);

    getClock()->initClusterTimeFromTrustedSource(initialTime);
    ASSERT_TRUE(getClock()->getClusterTime().getTime() == initialTime);

    getDBClient()->insert(kDummyNamespaceString, BSON("x" << 1));
    ASSERT_TRUE(getClock()->getClusterTime().getTime() > initialTime);
    ASSERT_EQ(getClock()->getClusterTime().getTime().asTimestamp(),
              replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp());
}

}  // unnamed namespace
}  // namespace mongo
