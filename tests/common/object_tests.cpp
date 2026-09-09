/**
 * @file tests/common/object_test.cpp
 * @brief Tests for the object module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <gtest/gtest.h>

#include "retdec/common/object.h"

using namespace ::testing;

namespace retdec {
namespace common {
namespace tests {

//
//=============================================================================
//  GlobalVarContainer
//=============================================================================
//

class GlobalVarContainerTests : public Test
{
	protected:
		GlobalVarContainer globals;
};

TEST_F(GlobalVarContainerTests, ElementWithTheSameNameGetsReplaced)
{
	globals.insert( Object("g1", common::Storage::inMemory(0x1000)) );
	globals.insert( Object("g2", common::Storage::inMemory(0x2000)) );
	globals.insert( Object("g3", common::Storage::inMemory(0x3000)) );

	EXPECT_EQ(0x2000, globals.getObjectByName("g2")->getStorage().getAddress());

	globals.insert( Object("g2", common::Storage::inMemory(0x4000)) );

	EXPECT_EQ(0x4000, globals.getObjectByName("g2")->getStorage().getAddress());
}

// The replaced element's address must stop resolving.
//
// insert() dropped the old set node without erasing its _addr2global entry,
// and that entry is keyed by the OLD address -- so getObjectByAddress(0x2000)
// returned a pointer into the freed node. The test above walks the same path
// and passes, because it never asks about 0x2000.
//
// Two "globals" entries in a config file with the same name at different
// addresses are enough to reach it.
TEST_F(GlobalVarContainerTests, ReplacedElementsOldAddressNoLongerResolves)
{
	globals.insert(Object("g", common::Storage::inMemory(0x1000)));
	ASSERT_NE(nullptr, globals.getObjectByAddress(0x1000));

	globals.insert(Object("g", common::Storage::inMemory(0x2000)));

	EXPECT_EQ(nullptr, globals.getObjectByAddress(0x1000));
	ASSERT_NE(nullptr, globals.getObjectByAddress(0x2000));
	EXPECT_EQ("g", globals.getObjectByAddress(0x2000)->getName());
	EXPECT_EQ(1u, globals.size());
}

// erase() looked up the element by id and then erased the ARGUMENT's address
// from the map, so erasing by an object that merely shares the name left the
// stored element's map entry pointing at a node it had just freed.
TEST_F(GlobalVarContainerTests, EraseByNameAloneClearsTheStoredAddress)
{
	globals.insert(Object("g", common::Storage::inMemory(0x1000)));

	globals.erase(Object("g", common::Storage::inMemory(0x9000)));

	EXPECT_EQ(nullptr, globals.getObjectByAddress(0x1000));
	EXPECT_EQ(nullptr, globals.getObjectByName("g"));
	EXPECT_EQ(0u, globals.size());
}

// A "globals" entry that names a register reaches insert() straight from a
// config file. It used to assert, which is a file-controlled abort.
TEST_F(GlobalVarContainerTests, NonMemoryStorageIsRefusedRatherThanAsserted)
{
	auto res = globals.insert(Object("g", common::Storage::inRegister("eax")));

	EXPECT_FALSE(res.second);
	EXPECT_EQ(0u, globals.size());
}

TEST_F(GlobalVarContainerTests, ElementWithTheSameAddressGetsReplaced)
{
	globals.insert( Object("g1", common::Storage::inMemory(0x1000)) );
	globals.insert( Object("g2", common::Storage::inMemory(0x2000)) );
	globals.insert( Object("g3", common::Storage::inMemory(0x3000)) );

	EXPECT_EQ("g2", globals.getObjectByAddress(0x2000)->getName());
	EXPECT_EQ("g2", globals._addr2global.find(0x2000)->second->getName());

	globals.insert( Object("g4", common::Storage::inMemory(0x2000)) );

	EXPECT_EQ("g4", globals.getObjectByAddress(0x2000)->getName());
	EXPECT_EQ("g4", globals._addr2global.find(0x2000)->second->getName());
}

TEST_F(GlobalVarContainerTests, OperationsOnUnderlyingContainerAreReflectedInaddr2global)
{
	EXPECT_TRUE(globals.empty());
	EXPECT_TRUE(globals._addr2global.empty());

	auto g2 = Object("g2", common::Storage::inMemory(0x2000));
	globals.insert( Object("g1", common::Storage::inMemory(0x1000)) );
	globals.insert( g2 );
	globals.insert( Object("g3", common::Storage::inMemory(0x3000)) );

	EXPECT_EQ(3, globals.size());
	EXPECT_EQ(3, globals._addr2global.size());

	globals.erase(g2);

	EXPECT_EQ(2, globals.size());
	EXPECT_EQ(2, globals._addr2global.size());

	globals.clear();

	EXPECT_TRUE(globals.empty());
	EXPECT_TRUE(globals._addr2global.empty());
}

TEST_F(GlobalVarContainerTests, WhenGlobalVarContainerIsCopiedPointersInAddressToObjectContainerAreUpdated)
{
	globals.insert( Object("g1", common::Storage::inMemory(0x1000)) );
	auto copy = globals;

	EXPECT_EQ(globals.getObjectByName("g1"), globals.getObjectByAddress(0x1000));
	EXPECT_EQ(copy.getObjectByName("g1"), copy.getObjectByAddress(0x1000));
}

TEST_F(GlobalVarContainerTests, WhenGlobalVarContainerIsCopyConstructedPointersInAddressToObjectContainerAreUpdated)
{
	globals.insert( Object("g1", common::Storage::inMemory(0x1000)) );
	auto copy(globals);

	EXPECT_EQ(globals.getObjectByName("g1"), globals.getObjectByAddress(0x1000));
	EXPECT_EQ(copy.getObjectByName("g1"), copy.getObjectByAddress(0x1000));
}

} // namespace tests
} // namespace common
} // namespace retdec
