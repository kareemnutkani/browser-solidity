
/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Christian <c@ethdev.com>
 * @date 2014
 * Tests for the Solidity optimizer.
 */

#include <string>
#include <tuple>
#include <boost/test/unit_test.hpp>
#include <boost/lexical_cast.hpp>
#include <test/solidityExecutionFramework.h>
#include <libevmcore/CommonSubexpressionEliminator.h>
#include <libevmcore/Assembly.h>

using namespace std;
using namespace dev::eth;

namespace dev
{
namespace solidity
{
namespace test
{

class OptimizerTestFramework: public ExecutionFramework
{
public:
	OptimizerTestFramework() { }
	/// Compiles the source code with and without optimizing.
	void compileBothVersions(
		std::string const& _sourceCode,
		u256 const& _value = 0,
		std::string const& _contractName = ""
	)
	{
		m_optimize = false;
		bytes nonOptimizedBytecode = compileAndRun(_sourceCode, _value, _contractName);
		m_nonOptimizedContract = m_contractAddress;
		m_optimize = true;
		bytes optimizedBytecode = compileAndRun(_sourceCode, _value, _contractName);
		BOOST_CHECK_MESSAGE(
			nonOptimizedBytecode.size() > optimizedBytecode.size(),
			"Optimizer did not reduce bytecode size."
		);
		m_optimizedContract = m_contractAddress;
	}

	template <class... Args>
	void compareVersions(std::string _sig, Args const&... _arguments)
	{
		m_contractAddress = m_nonOptimizedContract;
		bytes nonOptimizedOutput = callContractFunction(_sig, _arguments...);
		m_contractAddress = m_optimizedContract;
		bytes optimizedOutput = callContractFunction(_sig, _arguments...);
		BOOST_CHECK_MESSAGE(nonOptimizedOutput == optimizedOutput, "Computed values do not match."
							"\nNon-Optimized: " + toHex(nonOptimizedOutput) +
							"\nOptimized:     " + toHex(optimizedOutput));
	}

	void checkCSE(AssemblyItems const& _input, AssemblyItems const& _expectation)
	{
		eth::CommonSubexpressionEliminator cse;
		BOOST_REQUIRE(cse.feedItems(_input.begin(), _input.end()) == _input.end());
		AssemblyItems output = cse.getOptimizedItems();
		BOOST_CHECK_EQUAL_COLLECTIONS(_expectation.begin(), _expectation.end(), output.begin(), output.end());
	}

protected:
	Address m_optimizedContract;
	Address m_nonOptimizedContract;
};

BOOST_FIXTURE_TEST_SUITE(SolidityOptimizer, OptimizerTestFramework)

BOOST_AUTO_TEST_CASE(smoke_test)
{
	char const* sourceCode = R"(
		contract test {
			function f(uint a) returns (uint b) {
				return a;
			}
		})";
	compileBothVersions(sourceCode);
	compareVersions("f(uint256)", u256(7));
}

BOOST_AUTO_TEST_CASE(identities)
{
	char const* sourceCode = R"(
		contract test {
			function f(int a) returns (int b) {
				return int(0) | (int(1) * (int(0) ^ (0 + a)));
			}
		})";
	compileBothVersions(sourceCode);
	compareVersions("f(uint256)", u256(0x12334664));
}

BOOST_AUTO_TEST_CASE(unused_expressions)
{
	char const* sourceCode = R"(
		contract test {
			uint data;
			function f() returns (uint a, uint b) {
				10 + 20;
				data;
			}
		})";
	compileBothVersions(sourceCode);
	compareVersions("f()");
}

BOOST_AUTO_TEST_CASE(constant_folding_both_sides)
{
	// if constants involving the same associative and commutative operator are applied from both
	// sides, the operator should be applied only once, because the expression compiler pushes
	// literals as late as possible
	char const* sourceCode = R"(
		contract test {
			function f(uint x) returns (uint y) {
				return 98 ^ (7 * ((1 | (x | 1000)) * 40) ^ 102);
			}
		})";
	compileBothVersions(sourceCode);
	compareVersions("f(uint256)");
}

BOOST_AUTO_TEST_CASE(storage_access)
{
	char const* sourceCode = R"(
		contract test {
			uint8[40] data;
			function f(uint x) returns (uint y) {
				data[2] = data[7] = uint8(x);
				data[4] = data[2] * 10 + data[3];
			}
		}
	)";
	compileBothVersions(sourceCode);
	compareVersions("f(uint256)");
}

BOOST_AUTO_TEST_CASE(array_copy)
{
	char const* sourceCode = R"(
		contract test {
			bytes2[] data1;
			bytes5[] data2;
			function f(uint x) returns (uint l, uint y) {
				for (uint i = 0; i < msg.data.length; ++i)
					data1[i] = msg.data[i];
				data2 = data1;
				l = data2.length;
				y = uint(data2[x]);
			}
		}
	)";
	compileBothVersions(sourceCode);
	compareVersions("f(uint256)", 0);
	compareVersions("f(uint256)", 10);
	compareVersions("f(uint256)", 36);
}

BOOST_AUTO_TEST_CASE(function_calls)
{
	char const* sourceCode = R"(
		contract test {
			function f1(uint x) returns (uint) { return x*x; }
			function f(uint x) returns (uint) { return f1(7+x) - this.f1(x**9); }
		}
	)";
	compileBothVersions(sourceCode);
	compareVersions("f(uint256)", 0);
	compareVersions("f(uint256)", 10);
	compareVersions("f(uint256)", 36);
}

BOOST_AUTO_TEST_CASE(cse_intermediate_swap)
{
	eth::CommonSubexpressionEliminator cse;
	AssemblyItems input{
		Instruction::SWAP1, Instruction::POP, Instruction::ADD, u256(0), Instruction::SWAP1,
		Instruction::SLOAD, Instruction::SWAP1, u256(100), Instruction::EXP, Instruction::SWAP1,
		Instruction::DIV, u256(0xff), Instruction::AND
	};
	BOOST_REQUIRE(cse.feedItems(input.begin(), input.end()) == input.end());
	AssemblyItems output = cse.getOptimizedItems();
	BOOST_CHECK(!output.empty());
}

BOOST_AUTO_TEST_CASE(cse_negative_stack_access)
{
	AssemblyItems input{Instruction::DUP2, u256(0)};
	checkCSE(input, input);
}

BOOST_AUTO_TEST_CASE(cse_negative_stack_end)
{
	AssemblyItems input{Instruction::ADD};
	checkCSE(input, input);
}

BOOST_AUTO_TEST_CASE(cse_intermediate_negative_stack)
{
	AssemblyItems input{Instruction::ADD, u256(1), Instruction::DUP1};
	checkCSE(input, input);
}

BOOST_AUTO_TEST_CASE(cse_pop)
{
	checkCSE({Instruction::POP}, {Instruction::POP});
}

BOOST_AUTO_TEST_CASE(cse_unneeded_items)
{
	AssemblyItems input{
		Instruction::ADD,
		Instruction::SWAP1,
		Instruction::POP,
		u256(7),
		u256(8),
	};
	checkCSE(input, input);
}

BOOST_AUTO_TEST_CASE(cse_constant_addition)
{
	AssemblyItems input{u256(7), u256(8), Instruction::ADD};
	checkCSE(input, {u256(7 + 8)});
}

BOOST_AUTO_TEST_CASE(cse_invariants)
{
	AssemblyItems input{
		Instruction::DUP1,
		Instruction::DUP1,
		u256(0),
		Instruction::OR,
		Instruction::OR
	};
	checkCSE(input, {Instruction::DUP1});
}

BOOST_AUTO_TEST_CASE(cse_subself)
{
	checkCSE({Instruction::DUP1, Instruction::SUB}, {Instruction::POP, u256(0)});
}

BOOST_AUTO_TEST_CASE(cse_subother)
{
	checkCSE({Instruction::SUB}, {Instruction::SUB});
}

BOOST_AUTO_TEST_CASE(cse_double_negation)
{
	checkCSE({Instruction::DUP5, Instruction::NOT, Instruction::NOT}, {Instruction::DUP5});
}

BOOST_AUTO_TEST_CASE(cse_associativity)
{
	AssemblyItems input{
		Instruction::DUP1,
		Instruction::DUP1,
		u256(0),
		Instruction::OR,
		Instruction::OR
	};
	checkCSE(input, {Instruction::DUP1});
}

BOOST_AUTO_TEST_CASE(cse_associativity2)
{
	AssemblyItems input{
		u256(0),
		Instruction::DUP2,
		u256(2),
		u256(1),
		Instruction::DUP6,
		Instruction::ADD,
		u256(2),
		Instruction::ADD,
		Instruction::ADD,
		Instruction::ADD,
		Instruction::ADD
	};
	checkCSE(input, {Instruction::DUP2, Instruction::DUP2, Instruction::ADD, u256(5), Instruction::ADD});
}

BOOST_AUTO_TEST_SUITE_END()

}
}
} // end namespaces
