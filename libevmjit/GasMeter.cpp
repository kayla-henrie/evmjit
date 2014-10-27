
#include "GasMeter.h"

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>

#include <libevmface/Instruction.h>
#include <libevm/FeeStructure.h>

#include "Type.h"
#include "Ext.h"
#include "Runtime.h"

namespace dev
{
namespace eth
{
namespace jit
{

namespace // Helper functions
{

uint64_t getStepCost(Instruction inst) // TODO: Add this function to FeeSructure (pull request submitted)
{
	switch (inst)
	{
	case Instruction::STOP:
	case Instruction::SUICIDE:
		return 0;

	case Instruction::SSTORE:
		return static_cast<uint64_t>(c_sstoreGas);

	case Instruction::SLOAD:
		return static_cast<uint64_t>(c_sloadGas);

	case Instruction::SHA3:
		return static_cast<uint64_t>(c_sha3Gas);

	case Instruction::BALANCE:
		return static_cast<uint64_t>(c_sha3Gas);

	case Instruction::CALL:
	case Instruction::CALLCODE:
		return static_cast<uint64_t>(c_callGas);

	case Instruction::CREATE:
		return static_cast<uint64_t>(c_createGas);

	default: // Assumes instruction code is valid
		return static_cast<uint64_t>(c_stepGas);
	}
}

bool isCostBlockEnd(Instruction _inst)
{
	// Basic block terminators like STOP are not needed on the list
	// as cost will be commited at the end of basic block

	// CALL & CALLCODE are commited manually

	switch (_inst)
	{
	case Instruction::CALLDATACOPY:
	case Instruction::CODECOPY:
	case Instruction::MLOAD:
	case Instruction::MSTORE:
	case Instruction::MSTORE8:
	case Instruction::SSTORE:
	case Instruction::GAS:
	case Instruction::CREATE:
		return true;

	default:
		return false;
	}
}

}

GasMeter::GasMeter(llvm::IRBuilder<>& _builder, RuntimeManager& _runtimeManager) :
	CompilerHelper(_builder),
	m_runtimeManager(_runtimeManager)
{
	auto module = getModule();

	m_gasCheckFunc = llvm::Function::Create(llvm::FunctionType::get(Type::Void, Type::i256, false), llvm::Function::PrivateLinkage, "gas.check", module);
	InsertPointGuard guard(m_builder); 

	auto checkBB = llvm::BasicBlock::Create(_builder.getContext(), "Check", m_gasCheckFunc);
	auto outOfGasBB = llvm::BasicBlock::Create(_builder.getContext(), "OutOfGas", m_gasCheckFunc);
	auto updateBB = llvm::BasicBlock::Create(_builder.getContext(), "Update", m_gasCheckFunc);

	m_builder.SetInsertPoint(checkBB);
	llvm::Value* cost = m_gasCheckFunc->arg_begin();
	cost->setName("cost");
	auto gas = m_runtimeManager.getGas();
	auto isOutOfGas = m_builder.CreateICmpUGT(cost, gas, "isOutOfGas");
	m_builder.CreateCondBr(isOutOfGas, outOfGasBB, updateBB);

	m_builder.SetInsertPoint(outOfGasBB);

	//auto longjmpFunc = llvm::Intrinsic::getDeclaration(_module, llvm::Intrinsic::eh_sjlj_longjmp);
	auto extJmpBuf = new llvm::GlobalVariable(*module, Type::BytePtr, false, llvm::GlobalVariable::ExternalLinkage, nullptr, "rt_jmpBuf");
	llvm::Type* args[] = {Type::BytePtr, m_builder.getInt32Ty()};
	auto longjmpNative = llvm::Function::Create(llvm::FunctionType::get(Type::Void, args, false), llvm::Function::ExternalLinkage, "longjmp", module);
	m_builder.CreateCall2(longjmpNative, m_builder.CreateLoad(extJmpBuf), Constant::get(ReturnCode::OutOfGas));
	m_builder.CreateUnreachable();

	m_builder.SetInsertPoint(updateBB);
	gas = m_builder.CreateSub(gas, cost);
	m_runtimeManager.setGas(gas);
	m_builder.CreateRetVoid();
}

void GasMeter::count(Instruction _inst)
{
	if (!m_checkCall)
	{
		// Create gas check call with mocked block cost at begining of current cost-block
		m_checkCall = m_builder.CreateCall(m_gasCheckFunc, llvm::UndefValue::get(Type::i256));
	}
	
	if (_inst != Instruction::SSTORE) // Handle cost of SSTORE separately in countSStore()
		m_blockCost += getStepCost(_inst);

	if (isCostBlockEnd(_inst))
		commitCostBlock();
}

void GasMeter::countSStore(Ext& _ext, llvm::Value* _index, llvm::Value* _newValue)
{
	assert(!m_checkCall); // Everything should've been commited before

	static const auto sstoreCost = static_cast<uint64_t>(c_sstoreGas);

	// [ADD] if oldValue == 0 and newValue != 0  =>  2*cost
	// [DEL] if oldValue != 0 and newValue == 0  =>  0

	auto oldValue = _ext.store(_index);
	auto oldValueIsZero = m_builder.CreateICmpEQ(oldValue, Constant::get(0), "oldValueIsZero");
	auto newValueIsZero = m_builder.CreateICmpEQ(_newValue, Constant::get(0), "newValueIsZero");
	auto oldValueIsntZero = m_builder.CreateICmpNE(oldValue, Constant::get(0), "oldValueIsntZero");
	auto newValueIsntZero = m_builder.CreateICmpNE(_newValue, Constant::get(0), "newValueIsntZero");
	auto isAdd = m_builder.CreateAnd(oldValueIsZero, newValueIsntZero, "isAdd");
	auto isDel = m_builder.CreateAnd(oldValueIsntZero, newValueIsZero, "isDel");
	auto cost = m_builder.CreateSelect(isAdd, Constant::get(2 * sstoreCost), Constant::get(sstoreCost), "cost");
	cost = m_builder.CreateSelect(isDel, Constant::get(0), cost, "cost");
	m_builder.CreateCall(m_gasCheckFunc, cost);
}

void GasMeter::giveBack(llvm::Value* _gas)
{
	m_runtimeManager.setGas(m_builder.CreateAdd(m_runtimeManager.getGas(), _gas));
}

void GasMeter::commitCostBlock(llvm::Value* _additionalCost)
{
	assert(!_additionalCost || m_checkCall); // _additionalCost => m_checkCall; Must be inside cost-block

	// If any uncommited block
	if (m_checkCall)
	{
		if (m_blockCost == 0) // Do not check 0
		{
			m_checkCall->eraseFromParent(); // Remove the gas check call
			m_checkCall = nullptr;
			return;
		}

		m_checkCall->setArgOperand(0, Constant::get(m_blockCost)); // Update block cost in gas check call
		m_checkCall = nullptr; // End cost-block
		m_blockCost = 0;

		if (_additionalCost)
		{
			m_builder.CreateCall(m_gasCheckFunc, _additionalCost);
		}
	}
	assert(m_blockCost == 0);
}

void GasMeter::checkMemory(llvm::Value* _additionalMemoryInWords, llvm::IRBuilder<>& _builder)
{
	// Memory uses other builder, but that can be changes later
	auto cost = _builder.CreateMul(_additionalMemoryInWords, Constant::get(static_cast<uint64_t>(c_memoryGas)), "memcost");
	_builder.CreateCall(m_gasCheckFunc, cost);
}

}
}
}
