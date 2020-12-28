/*  ===========================================================================
*
*   This file is part of HISE.
*   Copyright 2016 Christoph Hart
*
*   HISE is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option any later version.
*
*   HISE is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with HISE.  If not, see <http://www.gnu.org/licenses/>.
*
*   Commercial licences for using HISE in an closed source project are
*   available on request. Please visit the project's website to get more
*   information about commercial licencing:
*
*   http://www.hartinstruments.net/hise/
*
*   HISE is based on the JUCE library,
*   which also must be licenced for commercial applications:
*
*   http://www.juce.com
*
*   ===========================================================================
*/


namespace snex {
namespace jit {
using namespace juce;
using namespace asmjit;


bool Operations::StatementBlock::isRealStatement(Statement* s)
{
	if (dynamic_cast<InlinedArgument*>(s) != nullptr)
		return false;

	if (dynamic_cast<Noop*>(s) != nullptr)
		return false;

	if (as<ReturnStatement>(s))
		return s->getType() != Types::ID::Void;

	if (dynamic_cast<VariableReference*>(s) != nullptr)
		return false;

	return true;
}

void Operations::StatementBlock::process(BaseCompiler* compiler, BaseScope* scope)
{
	auto bs = createOrGetBlockScope(scope);

	processBaseWithChildren(compiler, bs);

	auto path = getPath();

	COMPILER_PASS(BaseCompiler::DataAllocation)
	{
		Array<Symbol> destructorIds;

		forEachRecursive([path, &destructorIds, scope](Ptr p)
		{
			if (auto cd = as<ComplexTypeDefinition>(p))
			{
				if (cd->isStackDefinition(scope))
				{
					if (cd->type.getComplexType()->hasDestructor())
					{
						for (auto& id : cd->getInstanceIds())
						{
							if (path == id.getParent())
							{
								destructorIds.add(Symbol(id, cd->type));
							}
						}
					}
				}
			}

			return false;
		});

		//  Reverse the order of destructor execution.
		for (int i = destructorIds.size() - 1; i >= 0; i--)
		{
			auto id = destructorIds[i];

			ComplexType::DeconstructData d;
			ScopedPointer<SyntaxTreeInlineData> b = new SyntaxTreeInlineData(this, getPath());

			d.inlineData = b.get();
			b->object = this;
			b->expression = new Operations::VariableReference(location, id);
			auto r = id.typeInfo.getComplexType()->callDestructor(d);
			location.test(r);
		}
	}

	COMPILER_PASS(BaseCompiler::RegisterAllocation)
	{
		if (hasReturnType())
		{
			if (!isInlinedFunction)
			{
				allocateReturnRegister(compiler, bs);
			}
		}

		reg = returnRegister;
	}
}

snex::jit::BaseScope* Operations::StatementBlock::createOrGetBlockScope(BaseScope* parent)
{
	if (parent->getScopeType() == BaseScope::Class)
		return parent;

	if (blockScope == nullptr)
		blockScope = new RegisterScope(parent, getPath());

	return blockScope;
}

snex::jit::Operations::InlinedArgument* Operations::StatementBlock::findInlinedParameterInParentBlocks(Statement* p, const Symbol& s)
{
	if (p == nullptr)
		return nullptr;

	if (auto parentInlineArgument = findParentStatementOfType<InlinedArgument>(p))
	{
		auto parentBlock = findParentStatementOfType<StatementBlock>(parentInlineArgument);

		auto ipInParent = findInlinedParameterInParentBlocks(parentBlock->parent, s);

		if (ipInParent != nullptr)
			return ipInParent;

	}


	if (auto sb = dynamic_cast<StatementBlock*>(p))
	{
		if (sb->isInlinedFunction)
		{
			for (auto c : *sb)
			{
				if (auto ia = dynamic_cast<InlinedArgument*>(c))
				{
					if (ia->s == s)
						return ia;
				}
			}

			return nullptr;
		}
	}

	p = p->parent.get();

	if (p != nullptr)
		return findInlinedParameterInParentBlocks(p, s);

	return nullptr;
}

void Operations::ReturnStatement::process(BaseCompiler* compiler, BaseScope* scope)
{
	processBaseWithChildren(compiler, scope);

	COMPILER_PASS(BaseCompiler::TypeCheck)
	{
		if (auto fScope = dynamic_cast<FunctionScope*>(findFunctionScope(scope)))
		{
			TypeInfo actualType(Types::ID::Void);

			if (auto first = getSubExpr(0))
				actualType = first->getTypeInfo();

			if (isVoid() && actualType != Types::ID::Void)
				throwError("Can't return a value from a void function.");
			if (!isVoid() && actualType == Types::ID::Void)
				throwError("function must return a value");

			checkAndSetType(0, getTypeInfo());
		}
		else
			throwError("Can't deduce return type.");
	}

	COMPILER_PASS(BaseCompiler::CodeGeneration)
	{
		auto t = getTypeInfo().toPointerIfNativeRef();

		auto asg = CREATE_ASM_COMPILER(t.getType());

		bool isFunctionReturn = true;

		if (!isVoid())
		{
			if (auto sb = findInlinedRoot())
			{
				reg = getSubRegister(0);
				sb->reg = reg;

				if (reg != nullptr && reg->isActive())
					jassert(reg->isValid());
			}
			else if (auto sl = findRoot())
			{
				reg = sl->getReturnRegister();

				if (reg != nullptr && reg->isActive())
					jassert(reg->isValid());
			}

			if (reg == nullptr)
				throwError("Can't find return register");

			if (reg->isActive())
				jassert(reg->isValid());
		}

		if (findInlinedRoot() == nullptr)
		{
			auto sourceReg = isVoid() ? nullptr : getSubRegister(0);

			asg.emitReturn(compiler, reg, sourceReg);
		}
		else
		{
			asg.writeDirtyGlobals(compiler);
		}
	}
}

snex::jit::Operations::StatementBlock* Operations::ReturnStatement::findInlinedRoot() const
{
	if (auto sl = findRoot())
	{
		if (auto sb = dynamic_cast<StatementBlock*>(sl))
		{
			if (sb->isInlinedFunction)
			{
				return sb;
			}
		}
	}

	return nullptr;
}

void Operations::TernaryOp::process(BaseCompiler* compiler, BaseScope* scope)
{
	// We need to have precise control over the code generation
	// for the subexpressions to avoid execution of both branches
	if (compiler->getCurrentPass() == BaseCompiler::CodeGeneration)
		processBaseWithoutChildren(compiler, scope);
	else
		processBaseWithChildren(compiler, scope);

	COMPILER_PASS(BaseCompiler::TypeCheck)
	{
		type = checkAndSetType(1, type);
	}

	COMPILER_PASS(BaseCompiler::CodeGeneration)
	{
		auto asg = CREATE_ASM_COMPILER(getType());
		reg = asg.emitTernaryOp(this, compiler, scope);
		jassert(reg->isActive());
	}
}

void Operations::WhileLoop::process(BaseCompiler* compiler, BaseScope* scope)
{
	if (compiler->getCurrentPass() == BaseCompiler::CodeGeneration)
		Statement::processBaseWithoutChildren(compiler, scope);
	else
		Statement::processBaseWithChildren(compiler, scope);

	COMPILER_PASS(BaseCompiler::TypeCheck)
	{
		if (getSubExpr(0)->isConstExpr())
		{
			auto v = getSubExpr(0)->getConstExprValue();

			if (v.toInt() != 0)
			{
				throwError("Endless loop detected");
			}
		}
	}

	COMPILER_PASS(BaseCompiler::CodeGeneration)
	{
		auto acg = CREATE_ASM_COMPILER(Types::ID::Integer);
		auto safeCheck = scope->getGlobalScope()->isRuntimeErrorCheckEnabled();
		auto cond = acg.cc.newLabel();
		auto exit = acg.cc.newLabel();
		auto why = acg.cc.newGpd();

		if (safeCheck)
			acg.cc.xor_(why, why);

		acg.cc.nop();
		acg.cc.bind(cond);

		auto cp = getCompareCondition();

		if (cp != nullptr)
			cp->useAsmFlag = true;

		getSubExpr(0)->process(compiler, scope);
		auto cReg = getSubRegister(0);

		if (cp != nullptr)
		{
#define INT_COMPARE(token, command) if (cp->op == token) command(exit);

			INT_COMPARE(JitTokens::greaterThan, acg.cc.jle);
			INT_COMPARE(JitTokens::lessThan, acg.cc.jge);
			INT_COMPARE(JitTokens::lessThanOrEqual, acg.cc.jg);
			INT_COMPARE(JitTokens::greaterThanOrEqual, acg.cc.jl);
			INT_COMPARE(JitTokens::equals, acg.cc.jne);
			INT_COMPARE(JitTokens::notEquals, acg.cc.je);

#undef INT_COMPARE

			if (safeCheck)
			{
				acg.cc.inc(why);
				acg.cc.cmp(why, 10000000);
				auto okBranch = acg.cc.newLabel();
				acg.cc.jb(okBranch);

				auto errorFlag = x86::ptr(scope->getGlobalScope()->getRuntimeErrorFlag()).cloneResized(4);
				acg.cc.mov(why, (int)RuntimeError::ErrorType::WhileLoop);
				acg.cc.mov(errorFlag, why);
				acg.cc.mov(why, (int)location.getLine());
				acg.cc.mov(errorFlag.cloneAdjustedAndResized(4, 4), why);
				acg.cc.mov(why, (int)location.getColNumber(location.program, location.location));
				acg.cc.mov(errorFlag.cloneAdjustedAndResized(8, 4), why);
				acg.cc.jmp(exit);
				acg.cc.bind(okBranch);
			}
		}
		else
		{
			acg.cc.setInlineComment("check condition");
			acg.cc.cmp(INT_REG_R(cReg), 0);
			acg.cc.je(exit);

			if (safeCheck)
			{
				acg.cc.inc(why);
				acg.cc.cmp(why, 10000000);
				auto okBranch = acg.cc.newLabel();
				acg.cc.jb(okBranch);

				auto errorFlag = x86::ptr(scope->getGlobalScope()->getRuntimeErrorFlag()).cloneResized(4);
				acg.cc.mov(why, (int)RuntimeError::ErrorType::WhileLoop);
				acg.cc.mov(errorFlag, why);
				acg.cc.mov(why, (int)location.getLine());
				acg.cc.mov(errorFlag.cloneAdjustedAndResized(4, 4), why);
				acg.cc.mov(why, (int)location.getColNumber(location.program, location.location));
				acg.cc.mov(errorFlag.cloneAdjustedAndResized(8, 4), why);
				acg.cc.jmp(exit);
				acg.cc.bind(okBranch);
			}
		}

		getSubExpr(1)->process(compiler, scope);

		acg.cc.jmp(cond);
		acg.cc.bind(exit);
	}
}

snex::jit::Operations::Compare* Operations::WhileLoop::getCompareCondition()
{
	if (auto cp = as<Compare>(getSubExpr(0)))
		return cp;

	if (auto sb = as<StatementBlock>(getSubExpr(0)))
	{
		for (auto s : *sb)
		{
			if (auto cb = as<ConditionalBranch>(s))
				return nullptr;

			if (auto rt = as<ReturnStatement>(s))
			{
				return as<Compare>(rt->getSubExpr(0));
			}
		}
	}

	return nullptr;
}

void Operations::Loop::process(BaseCompiler* compiler, BaseScope* scope)
{
	processBaseWithoutChildren(compiler, scope);

	if (compiler->getCurrentPass() != BaseCompiler::DataAllocation &&
		compiler->getCurrentPass() != BaseCompiler::CodeGeneration)
	{

		getTarget()->process(compiler, scope);
		getLoopBlock()->process(compiler, scope);
	}

	COMPILER_PASS(BaseCompiler::DataAllocation)
	{
		tryToResolveType(compiler);

		getTarget()->process(compiler, scope);

		auto targetType = getTarget()->getTypeInfo();

		if (auto sp = targetType.getTypedIfComplexType<SpanType>())
		{
			loopTargetType = Span;

			if (iterator.typeInfo.isDynamic())
				iterator.typeInfo = sp->getElementType();
			else if (iterator.typeInfo != sp->getElementType())
				location.throwError("iterator type mismatch: " + iterator.typeInfo.toString() + " expected: " + sp->getElementType().toString());
		}
		else if (auto dt = targetType.getTypedIfComplexType<DynType>())
		{
			loopTargetType = Dyn;

			if (iterator.typeInfo.isDynamic())
				iterator.typeInfo = dt->elementType;
			else if (iterator.typeInfo != dt->elementType)
				location.throwError("iterator type mismatch: " + iterator.typeInfo.toString() + " expected: " + sp->getElementType().toString());
		}
		else if (targetType.getType() == Types::ID::Block)
		{
			loopTargetType = Dyn;

			if (iterator.typeInfo.isDynamic())
				iterator.typeInfo = TypeInfo(Types::ID::Float, iterator.isConst(), iterator.isReference());
			else if (iterator.typeInfo.getType() != Types::ID::Float)
				location.throwError("Illegal iterator type");
		}
		else
		{
			if (auto st = targetType.getTypedIfComplexType<StructType>())
			{
				FunctionClass::Ptr fc = st->getFunctionClass();

				customBegin = fc->getSpecialFunction(FunctionClass::BeginIterator);
				customSizeFunction = fc->getSpecialFunction(FunctionClass::SizeFunction);

				if (!customBegin.isResolved() || !customSizeFunction.isResolved())
					throwError(st->toString() + " does not have iterator methods");



				loopTargetType = CustomObject;

				if (iterator.typeInfo.isDynamic())
					iterator.typeInfo = customBegin.returnType;
				else if (iterator.typeInfo != customBegin.returnType)
					location.throwError("iterator type mismatch: " + iterator.typeInfo.toString() + " expected: " + customBegin.returnType.toString());

			}
			else
			{
				throwError("Can't deduce loop target type");
			}


		}


		compiler->namespaceHandler.setTypeInfo(iterator.id, NamespaceHandler::Variable, iterator.typeInfo);

		getLoopBlock()->process(compiler, scope);

		evaluateIteratorLoad();
	}

	COMPILER_PASS(BaseCompiler::CodeGeneration)
	{
		auto acg = CREATE_ASM_COMPILER(compiler->getRegisterType(iterator.typeInfo));

		getTarget()->process(compiler, scope);

		auto t = getTarget();

		auto r = getTarget()->reg;

		jassert(r != nullptr && r->getScope() != nullptr);

		allocateDirtyGlobalVariables(getLoopBlock(), compiler, scope);

		if (loopTargetType == Span)
		{
			auto le = new SpanLoopEmitter(compiler, iterator, getTarget()->reg, getLoopBlock(), loadIterator);
			le->typePtr = getTarget()->getTypeInfo().getTypedComplexType<SpanType>();
			loopEmitter = le;
		}
		else if (loopTargetType == Dyn)
		{
			auto le = new DynLoopEmitter(compiler, iterator, getTarget()->reg, getLoopBlock(), loadIterator);
			le->typePtr = getTarget()->getTypeInfo().getTypedComplexType<DynType>();
			loopEmitter = le;
		}
		else if (loopTargetType == CustomObject)
		{
			auto le = new CustomLoopEmitter(compiler, iterator, getTarget()->reg, getLoopBlock(), loadIterator);
			le->beginFunction = customBegin;
			le->sizeFunction = customSizeFunction;
			loopEmitter = le;
		}

		if (loopEmitter != nullptr)
			loopEmitter->emitLoop(acg, compiler, scope);
	}
}

bool Operations::Loop::evaluateIteratorLoad()
{
	if (!loadIterator)
		return false;

	SyntaxTreeWalker w(getLoopBlock(), false);

	while (auto v = w.getNextStatementOfType<VariableReference>())
	{
		if (v->id == iterator)
		{
			if (auto a = findParentStatementOfType<Assignment>(v))
			{
				if (a->getSubExpr(1).get() == v && a->assignmentType == JitTokens::assign_)
				{
					auto sId = v->id;

					bool isSelfAssign = a->getSubExpr(0)->forEachRecursive([sId](Operations::Statement::Ptr p)
					{
						if (auto v = dynamic_cast<VariableReference*>(p.get()))
						{
							if (v->id == sId)
								return true;
						}

						return false;
					});

					loadIterator = isSelfAssign;
				}

				if (a->assignmentType != JitTokens::assign_)
					loadIterator = true;

				if (a->getSubExpr(1).get() != v)
					loadIterator = true;
			}

			break;
		}
	}

	return loadIterator;
}

bool Operations::Loop::evaluateIteratorStore()
{
	if (storeIterator)
		return true;

	SyntaxTreeWalker w(getLoopBlock(), false);

	while (auto v = w.getNextStatementOfType<VariableReference>())
	{
		if (v->id == iterator)
		{
			if (v->parent->hasSideEffect())
			{
				if (auto a = as<Assignment>(v->parent.get()))
				{
					if (a->getSubExpr(0).get() == v)
						continue;
				}

				storeIterator = true;
				break;
			}
		}
	}

	return storeIterator;
}

bool Operations::Loop::tryToResolveType(BaseCompiler* compiler)
{
	getTarget()->tryToResolveType(compiler);

	auto tt = getTarget()->getTypeInfo();

	if (auto targetType = tt.getTypedIfComplexType<ArrayTypeBase>())
	{
		auto r = compiler->namespaceHandler.setTypeInfo(iterator.id, NamespaceHandler::Variable, targetType->getElementType());

		auto iteratorType = targetType->getElementType().withModifiers(iterator.isConst(), iterator.isReference());

		iterator = { iterator.id, iteratorType };

		if (r.failed())
			throwError(r.getErrorMessage());
	}

	if (auto fpType = tt.getTypedIfComplexType<StructType>())
	{
		if (fpType->id == NamespacedIdentifier("FrameProcessor"))
		{
			TypeInfo floatType(Types::ID::Float, false, true);

			auto r = compiler->namespaceHandler.setTypeInfo(iterator.id, NamespaceHandler::Variable, floatType);

			iterator = { iterator.id, floatType };

			if (r.failed())
				throwError(r.getErrorMessage());
		}
	}

	Statement::tryToResolveType(compiler);

	return true;
}

void Operations::ControlFlowStatement::process(BaseCompiler* compiler, BaseScope* scope)
{
	processBaseWithChildren(compiler, scope);

	COMPILER_PASS(BaseCompiler::TypeCheck)
	{
		parentLoop = findParentStatementOfType<Loop>(this);

		if (parentLoop == nullptr)
		{
			juce::String s;
			s << "a " << getStatementId().toString() << " may only be used within a loop or switch";
			throwError(s);
		}
	}

	COMPILER_PASS(BaseCompiler::CodeGeneration)
	{
		auto acg = CREATE_ASM_COMPILER(Types::ID::Integer);
		acg.emitLoopControlFlow(parentLoop, isBreak);
	}
}

void Operations::IfStatement::process(BaseCompiler* compiler, BaseScope* scope)
{
	processBaseWithoutChildren(compiler, scope);

	if (compiler->getCurrentPass() != BaseCompiler::CodeGeneration)
		processAllChildren(compiler, scope);

	COMPILER_PASS(BaseCompiler::TypeCheck)
	{
		processAllChildren(compiler, scope);

		if (getCondition()->getTypeInfo() != Types::ID::Integer)
			throwError("Condition must be boolean expression");
	}

	COMPILER_PASS(BaseCompiler::CodeGeneration)
	{
		auto acg = CREATE_ASM_COMPILER(Types::ID::Integer);

		allocateDirtyGlobalVariables(getTrueBranch(), compiler, scope);

		if (hasFalseBranch())
			allocateDirtyGlobalVariables(getFalseBranch(), compiler, scope);

		auto cond = dynamic_cast<Expression*>(getCondition().get());
		auto trueBranch = getTrueBranch();
		auto falseBranch = getFalseBranch();

		acg.emitBranch(TypeInfo(Types::ID::Void), cond, trueBranch, falseBranch, compiler, scope);
	}
}

}
}