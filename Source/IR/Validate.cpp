#include "IR/Module.h"
#include "IR/Operators.h"
#include "IR/OperatorLoggingProxy.h"
#include "IR/Validate.h"

#include <set>

#define ENABLE_LOGGING 0

namespace IR
{
	#define VALIDATE_UNLESS(reason,comparison) \
		if(comparison) \
		{ \
			throw ValidationException(reason #comparison); \
		}

	#define VALIDATE_INDEX(index,arraySize) VALIDATE_UNLESS("invalid index: ",index>=arraySize)

	void validate(ValueType valueType)
	{
		if(valueType == ValueType::any || valueType > ValueType::max)
		{
			throw ValidationException("invalid value type (" + std::to_string((uintp)valueType) + ")");
		}
	}

	void validate(ResultType returnType)
	{
		if(returnType > ResultType::max)
		{
			throw ValidationException("invalid return type (" + std::to_string((uintp)returnType) + ")");
		}
	}

	void validate(ObjectKind kind)
	{
		if(kind > ObjectKind::max)
		{
			throw ValidationException("invalid external kind (" + std::to_string((uintp)kind) + ")");
		}
	}

	void validate(SizeConstraints size,size_t maxMax)
	{
		size_t max = size.max == UINT64_MAX ? maxMax : size.max;
		VALIDATE_UNLESS("disjoint size bounds: ",size.min>max);
		VALIDATE_UNLESS("maximum size exceeds limit: ",max>maxMax);
	}

	void validate(TableElementType type)
	{
		if(type != TableElementType::anyfunc) { throw ValidationException("invalid table element type (" + std::to_string((uintp)type) + ")"); }
	}

	void validate(GlobalType type)
	{
		validate(type.valueType);
	}
	
	void validateImportKind(ObjectType importType,ObjectKind expectedKind)
	{
		if(importType.kind != expectedKind)
		{
			throw ValidationException("incorrect kind");
		}
	}

	template<typename Type>
	void validateType(Type expectedType,Type actualType,const char* context)
	{
		if(expectedType != actualType)
		{
			throw ValidationException(
				std::string("type mismatch: expected ") + asString(expectedType)
				+ " but got " + asString(actualType)
				+ " in " + context
				);
		}
	}

	void validateOperandType(ValueType expectedType,ValueType actualType,const char* context)
	{
		// Handle polymorphic values popped off the operand stack after unconditional branches.
		if(expectedType != actualType && expectedType != ValueType::any && actualType != ValueType::any)
		{
			throw ValidationException(
				std::string("type mismatch: expected ") + asString(expectedType)
				+ " but got " + asString(actualType)
				+ " in " + context + " operand"
				);
		}
	}

	ValueType validateGlobalIndex(const Module& module,uintp globalIndex,bool mustBeMutable,bool mustBeImmutable,bool mustBeImport,const char* context)
	{
		VALIDATE_INDEX(globalIndex,module.globals.size());
		const GlobalType& globalType = module.globals.getType(globalIndex);
		if(mustBeMutable && !globalType.isMutable) { throw ValidationException("attempting to mutate immutable global"); }
		else if(mustBeImport && globalIndex >= module.globals.imports.size()) { throw ValidationException("global variable initializer expression may only access imported globals"); }
		else if(mustBeImmutable && globalType.isMutable) { throw ValidationException("global variable initializer expression may only access immutable globals"); }
		return globalType.valueType;
	}

	const FunctionType* validateFunctionIndex(const Module& module,uintp functionIndex)
	{
		VALIDATE_INDEX(functionIndex,module.functions.size());
		return module.types[module.functions.getType(functionIndex).index];
	}
		
	void validateInitializer(const Module& module,const InitializerExpression& expression,ValueType expectedType,const char* context)
	{
		switch(expression.type)
		{
		case InitializerExpression::Type::i32_const: validateType(expectedType,ValueType::i32,context); break;
		case InitializerExpression::Type::i64_const: validateType(expectedType,ValueType::i64,context); break;
		case InitializerExpression::Type::f32_const: validateType(expectedType,ValueType::f32,context); break;
		case InitializerExpression::Type::f64_const: validateType(expectedType,ValueType::f64,context); break;
		case InitializerExpression::Type::get_global:
		{
			const ValueType globalValueType = validateGlobalIndex(module,expression.globalIndex,false,true,true,"initializer expression global index");
			validateType(expectedType,globalValueType,context);
			break;
		}
		default:
			throw ValidationException("invalid initializer expression");
		};
	}

	struct FunctionValidationContext
	{
		FunctionValidationContext(const Module& inModule,const FunctionDef& inFunctionDef)
		: module(inModule), functionDef(inFunctionDef), functionType(inModule.types[inFunctionDef.type.index])
		{
			// Initialize the local types.
			locals.reserve(functionType->parameters.size() + functionDef.nonParameterLocalTypes.size());
			locals = functionType->parameters;
			locals.insert(locals.end(),functionDef.nonParameterLocalTypes.begin(),functionDef.nonParameterLocalTypes.end());

			// Push the function context onto the control stack.
			pushControlStack(ControlContext::Type::function,functionType->ret,functionType->ret);
		}

		size_t getControlStackSize() { return controlStack.size(); }
		
		void logOperator(const std::string& operatorDescription)
		{
			if(ENABLE_LOGGING)
			{
				std::string controlStackString;
				for(uintp stackIndex = 0;stackIndex < controlStack.size();++stackIndex)
				{
					if(!controlStack[stackIndex].isReachable) { controlStackString += "("; }
					switch(controlStack[stackIndex].type)
					{
					case ControlContext::Type::function: controlStackString += "F"; break;
					case ControlContext::Type::block: controlStackString += "B"; break;
					case ControlContext::Type::ifThen: controlStackString += "T"; break;
					case ControlContext::Type::ifElse: controlStackString += "E"; break;
					case ControlContext::Type::loop: controlStackString += "L"; break;
					default: Core::unreachable();
					};
					if(!controlStack[stackIndex].isReachable) { controlStackString += ")"; }
				}

				std::string stackString;
				const uintp stackBase = controlStack.size() == 0 ? 0 : controlStack.back().outerStackSize;
				for(uintp stackIndex = 0;stackIndex < stack.size();++stackIndex)
				{
					if(stackIndex == stackBase) { stackString += "| "; }
					stackString +=  asString(stack[stackIndex]);
					stackString +=  " ";
				}
				if(stack.size() == stackBase) { stackString += "|"; }

				Log::printf(Log::Category::debug,"%-50s %-50s %-50s\n",controlStackString.c_str(),operatorDescription.c_str(),stackString.c_str());
			}
		}

		// Operation dispatch methods.
		void unknown(Opcode opcode)
		{
			throw ValidationException("Unknown opcode: " + std::to_string((uintp)opcode));
		}
		void block(ControlStructureImm imm)
		{
			validate(imm.resultType);
			pushControlStack(ControlContext::Type::block,imm.resultType,imm.resultType);
		}
		void loop(ControlStructureImm imm)
		{
			validate(imm.resultType);
			pushControlStack(ControlContext::Type::loop,ResultType::none,imm.resultType);
		}
		void if_(ControlStructureImm imm)
		{
			validate(imm.resultType);
			popAndValidateOperand("if condition",ValueType::i32);
			pushControlStack(ControlContext::Type::ifThen,imm.resultType,imm.resultType);
		}
		void else_(NoImm imm)
		{
			popAndValidateResultType("if result",controlStack.back().resultType);
			popControlStack(true);
		}
		void end(NoImm)
		{
			popAndValidateResultType("end result",controlStack.back().resultType);
			popControlStack();
		}
		
		void return_(NoImm)
		{
			popAndValidateResultType("ret",functionType->ret);
			enterUnreachable();
		}

		void br(BranchImm imm)
		{
			popAndValidateResultType("br argument",getBranchTargetByDepth(imm.targetDepth).branchArgumentType);
			enterUnreachable();
		}
		void br_table(BranchTableImm imm)
		{
			popAndValidateOperand("br_table index",ValueType::i32);
			const ResultType defaultTargetArgumentType = getBranchTargetByDepth(imm.defaultTargetDepth).branchArgumentType;
			popAndValidateResultType("br_table argument",defaultTargetArgumentType);

			for(uintp targetIndex = 0;targetIndex < imm.targetDepths.size();++targetIndex)
			{
				const ResultType targetArgumentType = getBranchTargetByDepth(imm.targetDepths[targetIndex]).branchArgumentType;
				VALIDATE_UNLESS("br_table target argument must match default target argument: ",targetArgumentType != defaultTargetArgumentType);
			}

			enterUnreachable();
		}
		void br_if(BranchImm imm)
		{
			popAndValidateOperand("br_if condition",ValueType::i32);
			popAndValidateResultType("br_if argument",getBranchTargetByDepth(imm.targetDepth).branchArgumentType);
			pushOperand(getBranchTargetByDepth(imm.targetDepth).branchArgumentType);
		}

		void nop(NoImm) {}
		void unreachable(NoImm)
		{
			enterUnreachable();
		}
		void drop(NoImm)
		{
			popOperand();
		}

		void select(NoImm)
		{
			const ValueType condition = popOperand();
			const ValueType falseType = popOperand();
			const ValueType trueType = popOperand();
			validateOperandType(ValueType::i32,condition,"select condition");
			validateOperandType(falseType,trueType,"select operands");
			pushOperand(falseType);
		}

		void get_local(GetOrSetVariableImm imm)
		{
			pushOperand(validateLocalIndex(imm.variableIndex));
		}
		void set_local(GetOrSetVariableImm imm)
		{
			popAndValidateOperand("set_local",validateLocalIndex(imm.variableIndex));
		}
		void tee_local(GetOrSetVariableImm imm)
		{
			const ValueType localType = validateLocalIndex(imm.variableIndex);
			popAndValidateOperand("tee_local",localType);
			pushOperand(localType);
		}
		
		void get_global(GetOrSetVariableImm imm)
		{
			pushOperand(validateGlobalIndex(module,imm.variableIndex,false,false,false,"get_global"));
		}
		void set_global(GetOrSetVariableImm imm)
		{
			popAndValidateOperand("set_global",validateGlobalIndex(module,imm.variableIndex,true,false,false,"set_global"));
		}

		void call(CallImm imm)
		{
			const FunctionType* calleeType = validateFunctionIndex(module,imm.functionIndex);
			popAndValidateOperands("call arguments",calleeType->parameters.data(),calleeType->parameters.size());
			pushOperand(calleeType->ret);
		}
		void call_indirect(CallIndirectImm imm)
		{
			VALIDATE_INDEX(imm.type.index,module.types.size());
			VALIDATE_UNLESS("call_indirect is only valid if there is a default function table: ",module.tables.size()==0);
			const FunctionType* calleeType = module.types[imm.type.index];
			popAndValidateOperand("call_indirect function index",ValueType::i32);
			popAndValidateOperands("call_indirect arguments",calleeType->parameters.data(),calleeType->parameters.size());
			pushOperand(calleeType->ret);
		}

		void grow_memory(MemoryImm)
		{
			VALIDATE_UNLESS("grow_memory is only valid if there is a default memory",module.memories.size() == 0);
			popAndValidateOperand("grow_memory",ValueType::i32); pushOperand(ValueType::i32);
		}
		void current_memory(MemoryImm)
		{
			VALIDATE_UNLESS("current_memory is only valid if there is a default memory",module.memories.size() == 0);
			pushOperand(ValueType::i32);
		}

		void error(ErrorImm imm) { throw ValidationException("error opcode"); }

		#define VALIDATE_CONST(typeId,nativeType) \
			void typeId##_const(LiteralImm<nativeType> imm) { pushOperand(ValueType::typeId); }
		VALIDATE_CONST(i32,int32); VALIDATE_CONST(i64,int64);
		VALIDATE_CONST(f32,float32); VALIDATE_CONST(f64,float64);

		#define VALIDATE_LOAD_OPCODE(name,nameString,naturalAlignmentLog2,resultType) void name(LoadOrStoreImm imm) \
			{ \
				popAndValidateOperand(#name,ValueType::i32); \
				VALIDATE_UNLESS(nameString " alignment greater than natural alignment: ",imm.alignmentLog2>naturalAlignmentLog2); \
				VALIDATE_UNLESS(nameString " in module without default memory: ",module.memories.size()==0); \
				VALIDATE_UNLESS(nameString " offset too large: ",imm.offset > UINT32_MAX); \
				pushOperand(ValueType::resultType); \
			}

		VALIDATE_LOAD_OPCODE(i32_load8_s,"i32.load8_s",1,i32)  VALIDATE_LOAD_OPCODE(i32_load8_u,"i32.load8_u",1,i32)
		VALIDATE_LOAD_OPCODE(i32_load16_s,"i32.load16_s",2,i32) VALIDATE_LOAD_OPCODE(i32_load16_u,"i32.load16_u",2,i32)
		VALIDATE_LOAD_OPCODE(i64_load8_s,"i64.load8_s",1,i64)  VALIDATE_LOAD_OPCODE(i64_load8_u,"i64.load8_u",1,i64)
		VALIDATE_LOAD_OPCODE(i64_load16_s,"i64.load16_s",2,i64)  VALIDATE_LOAD_OPCODE(i64_load16_u,"i64.load16_u",2,i64)
		VALIDATE_LOAD_OPCODE(i64_load32_s,"i64.load32_s",4,i64)  VALIDATE_LOAD_OPCODE(i64_load32_u,"i64.load32_u",4,i64)

		VALIDATE_LOAD_OPCODE(i32_load,"i32.load",4,i32) VALIDATE_LOAD_OPCODE(i64_load,"i64.load",8,i64)
		VALIDATE_LOAD_OPCODE(f32_load,"f32.load",4,f32) VALIDATE_LOAD_OPCODE(f64_load,"f64.load",8,f64)
			
		#define VALIDATE_STORE_OPCODE(name,nameString,naturalAlignmentLog2,valueTypeId) void name(LoadOrStoreImm imm) \
			{ \
				popAndValidateOperands(nameString,ValueType::i32,ValueType::valueTypeId); \
				VALIDATE_UNLESS(nameString " alignment greater than natural alignment: ",imm.alignmentLog2>naturalAlignmentLog2); \
				VALIDATE_UNLESS(nameString " in module without default memory: ",module.memories.size()==0); \
				VALIDATE_UNLESS(nameString " offset too large: ",imm.offset > UINT32_MAX); \
			}

		VALIDATE_STORE_OPCODE(i32_store8,"i32.store8",1,i32) VALIDATE_STORE_OPCODE(i32_store16,"i32.store16",2,i32) VALIDATE_STORE_OPCODE(i32_store,"i32.store",4,i32)
		VALIDATE_STORE_OPCODE(i64_store8,"i64.store8",1,i64) VALIDATE_STORE_OPCODE(i64_store16,"i64.store16",2,i64) VALIDATE_STORE_OPCODE(i64_store32,"i64.store32",4,i64) VALIDATE_STORE_OPCODE(i64_store,"i64.store",8,i64)
		VALIDATE_STORE_OPCODE(f32_store,"f32.store",4,f32) VALIDATE_STORE_OPCODE(f64_store,"f64.store",8,f64)

		#define VALIDATE_BINARY_OPCODE(name,nameString,operandTypeId,resultTypeId) void name(NoImm) \
			{ \
				popAndValidateOperands(nameString,ValueType::operandTypeId,ValueType::operandTypeId); \
				pushOperand(ValueType::resultTypeId); \
			}
		#define VALIDATE_UNARY_OPCODE(name,nameString,operandTypeId,resultTypeId) void name(NoImm) \
			{ \
				popAndValidateOperand(nameString,ValueType::operandTypeId); \
				pushOperand(ValueType::resultTypeId); \
			}

		VALIDATE_BINARY_OPCODE(i32_add,"i32.add",i32,i32) VALIDATE_BINARY_OPCODE(i64_add,"i64.add",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_sub,"i32.sub",i32,i32) VALIDATE_BINARY_OPCODE(i64_sub,"i64.sub",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_mul,"i32.mul",i32,i32) VALIDATE_BINARY_OPCODE(i64_mul,"i64.mul",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_div_s,"i32.div_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_div_s,"i64.div_s",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_div_u,"i32.div_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_div_u,"i64.div_u",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_rem_s,"i32.rem_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_rem_s,"i64.rem_s",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_rem_u,"i32.rem_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_rem_u,"i64.rem_u",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_and,"i32.and",i32,i32) VALIDATE_BINARY_OPCODE(i64_and,"i64.and",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_or,"i32.or",i32,i32) VALIDATE_BINARY_OPCODE(i64_or,"i64.or",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_xor,"i32.xor",i32,i32) VALIDATE_BINARY_OPCODE(i64_xor,"i64.xor",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_shl,"i32.shl",i32,i32) VALIDATE_BINARY_OPCODE(i64_shl,"i64.shl",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_shr_u,"i32.shr_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_shr_u,"i64.shr_u",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_shr_s,"i32.shr_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_shr_s,"i64.shr_s",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_rotr,"i32.rotr",i32,i32) VALIDATE_BINARY_OPCODE(i64_rotr,"i64.rotr",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_rotl,"i32.rotl",i32,i32) VALIDATE_BINARY_OPCODE(i64_rotl,"i64.rotl",i64,i64)
		VALIDATE_BINARY_OPCODE(i32_eq,"i32.eq",i32,i32) VALIDATE_BINARY_OPCODE(i64_eq,"i64.eq",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_ne,"i32.ne",i32,i32) VALIDATE_BINARY_OPCODE(i64_ne,"i64.ne",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_lt_s,"i32.lt_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_lt_s,"i64.lt_s",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_le_s,"i32.le_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_le_s,"i64.le_s",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_lt_u,"i32.lt_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_lt_u,"i64.lt_u",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_le_u,"i32.le_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_le_u,"i64.le_u",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_gt_s,"i32.gt_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_gt_s,"i64.gt_s",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_ge_s,"i32.ge_s",i32,i32) VALIDATE_BINARY_OPCODE(i64_ge_s,"i64.ge_s",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_gt_u,"i32.gt_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_gt_u,"i64.gt_u",i64,i32)
		VALIDATE_BINARY_OPCODE(i32_ge_u,"i32.ge_u",i32,i32) VALIDATE_BINARY_OPCODE(i64_ge_u,"i64.ge_u",i64,i32)
		VALIDATE_UNARY_OPCODE(i32_clz,"i32.clz",i32,i32) VALIDATE_UNARY_OPCODE(i64_clz,"i64.clz",i64,i64)
		VALIDATE_UNARY_OPCODE(i32_ctz,"i32.ctz",i32,i32) VALIDATE_UNARY_OPCODE(i64_ctz,"i64.ctz",i64,i64)
		VALIDATE_UNARY_OPCODE(i32_popcnt,"i32.popcnt",i32,i32) VALIDATE_UNARY_OPCODE(i64_popcnt,"i64.popcnt",i64,i64)
		VALIDATE_UNARY_OPCODE(i32_eqz,"i32.eqz",i32,i32) VALIDATE_UNARY_OPCODE(i64_eqz,"i64.eqz",i64,i32)

		VALIDATE_BINARY_OPCODE(f32_add,"f32.add",f32,f32) VALIDATE_BINARY_OPCODE(f64_add,"f64.add",f64,f64)
		VALIDATE_BINARY_OPCODE(f32_sub,"f32.sub",f32,f32) VALIDATE_BINARY_OPCODE(f64_sub,"f64.sub",f64,f64)
		VALIDATE_BINARY_OPCODE(f32_mul,"f32.mul",f32,f32) VALIDATE_BINARY_OPCODE(f64_mul,"f64.mul",f64,f64)
		VALIDATE_BINARY_OPCODE(f32_div,"f32.div",f32,f32) VALIDATE_BINARY_OPCODE(f64_div,"f64.div",f64,f64)
		VALIDATE_BINARY_OPCODE(f32_min,"f32.min",f32,f32) VALIDATE_BINARY_OPCODE(f64_min,"f64.min",f64,f64)
		VALIDATE_BINARY_OPCODE(f32_max,"f32.max",f32,f32) VALIDATE_BINARY_OPCODE(f64_max,"f64.max",f64,f64)
		VALIDATE_BINARY_OPCODE(f32_copysign,"f32.copysign",f32,f32) VALIDATE_BINARY_OPCODE(f64_copysign,"f64.copysign",f64,f64)

		VALIDATE_BINARY_OPCODE(f32_eq,"f32.eq",f32,i32) VALIDATE_BINARY_OPCODE(f64_eq,"f64.eq",f64,i32)
		VALIDATE_BINARY_OPCODE(f32_ne,"f32.ne",f32,i32) VALIDATE_BINARY_OPCODE(f64_ne,"f64.ne",f64,i32)
		VALIDATE_BINARY_OPCODE(f32_lt,"f32.lt",f32,i32) VALIDATE_BINARY_OPCODE(f64_lt,"f64.lt",f64,i32)
		VALIDATE_BINARY_OPCODE(f32_le,"f32.le",f32,i32) VALIDATE_BINARY_OPCODE(f64_le,"f64.le",f64,i32)
		VALIDATE_BINARY_OPCODE(f32_gt,"f32.gt",f32,i32) VALIDATE_BINARY_OPCODE(f64_gt,"f64.gt",f64,i32)
		VALIDATE_BINARY_OPCODE(f32_ge,"f32.ge",f32,i32) VALIDATE_BINARY_OPCODE(f64_ge,"f64.ge",f64,i32)

		VALIDATE_UNARY_OPCODE(f32_abs,"f32.abs",f32,f32) VALIDATE_UNARY_OPCODE(f64_abs,"f64.abs",f64,f64)
		VALIDATE_UNARY_OPCODE(f32_neg,"f32.neg",f32,f32) VALIDATE_UNARY_OPCODE(f64_neg,"f64.neg",f64,f64)
		VALIDATE_UNARY_OPCODE(f32_ceil,"f32.ceil",f32,f32) VALIDATE_UNARY_OPCODE(f64_ceil,"f64.ceil",f64,f64)
		VALIDATE_UNARY_OPCODE(f32_floor,"f32.floor",f32,f32) VALIDATE_UNARY_OPCODE(f64_floor,"f64.floor",f64,f64)
		VALIDATE_UNARY_OPCODE(f32_trunc,"f32.trunc",f32,f32) VALIDATE_UNARY_OPCODE(f64_trunc,"f64.trunc",f64,f64)
		VALIDATE_UNARY_OPCODE(f32_nearest,"f32.nearest",f32,f32) VALIDATE_UNARY_OPCODE(f64_nearest,"f64.nearest",f64,f64)
		VALIDATE_UNARY_OPCODE(f32_sqrt,"f32.sqrt",f32,f32) VALIDATE_UNARY_OPCODE(f64_sqrt,"f64.sqrt",f64,f64)

		VALIDATE_UNARY_OPCODE(i32_trunc_s_f32,"i32.trunc_s/f32",f32,i32)
		VALIDATE_UNARY_OPCODE(i32_trunc_s_f64,"i32.trunc_s/f64",f64,i32)
		VALIDATE_UNARY_OPCODE(i32_trunc_u_f32,"i32.trunc_u/f32",f32,i32)
		VALIDATE_UNARY_OPCODE(i32_trunc_u_f64,"i32.trunc_u/f64",f64,i32)
		VALIDATE_UNARY_OPCODE(i32_wrap_i64,"i32.wrap/i64",i64,i32)
		VALIDATE_UNARY_OPCODE(i64_trunc_s_f32,"i64.trunc_s/f32",f32,i64)
		VALIDATE_UNARY_OPCODE(i64_trunc_s_f64,"i64.trunc_s/f64",f64,i64)
		VALIDATE_UNARY_OPCODE(i64_trunc_u_f32,"i64.trunc_u/f32",f32,i64)
		VALIDATE_UNARY_OPCODE(i64_trunc_u_f64,"i64.trunc_u/f64",f64,i64)
		VALIDATE_UNARY_OPCODE(i64_extend_s_i32,"i64.extend_s/i32",i32,i64)
		VALIDATE_UNARY_OPCODE(i64_extend_u_i32,"i64.extend_u/i32",i32,i64)
		VALIDATE_UNARY_OPCODE(f32_convert_s_i32,"f32.convert_s/i32",i32,f32)
		VALIDATE_UNARY_OPCODE(f32_convert_u_i32,"f32.convert_u/i32",i32,f32)
		VALIDATE_UNARY_OPCODE(f32_convert_s_i64,"f32.convert_s/i64",i64,f32)
		VALIDATE_UNARY_OPCODE(f32_convert_u_i64,"f32.convert_u/i64",i64,f32)
		VALIDATE_UNARY_OPCODE(f32_demote_f64,"f32.demote/f64",f64,f32)
		VALIDATE_UNARY_OPCODE(f32_reinterpret_i32,"f32.reinterpret/i32",i32,f32)
		VALIDATE_UNARY_OPCODE(f64_convert_s_i32,"f64.convert_s/i32",i32,f64)
		VALIDATE_UNARY_OPCODE(f64_convert_u_i32,"f64.convert_u/i32",i32,f64)
		VALIDATE_UNARY_OPCODE(f64_convert_s_i64,"f64.convert_s/i64",i64,f64)
		VALIDATE_UNARY_OPCODE(f64_convert_u_i64,"f64.convert_u/i64",i64,f64)
		VALIDATE_UNARY_OPCODE(f64_promote_f32,"f64.promote/f32",f32,f64)
		VALIDATE_UNARY_OPCODE(f64_reinterpret_i64,"f64.reinterpret/i64",i64,f64)
		VALIDATE_UNARY_OPCODE(i32_reinterpret_f32,"i32.reinterpret/f32",f32,i32)
		VALIDATE_UNARY_OPCODE(i64_reinterpret_f64,"i64.reinterpret/f64",f64,i64)

	private:
		
		struct ControlContext
		{
			enum class Type : uint8
			{
				function,
				block,
				ifWithoutElse,
				ifThen,
				ifElse,
				loop
			};

			Type type;
			uintp outerStackSize;
			
			ResultType branchArgumentType;
			ResultType resultType;
			bool isReachable;
		};

		const Module& module;
		const FunctionDef& functionDef;
		const FunctionType* functionType;

		std::vector<ValueType> locals;
		std::vector<ControlContext> controlStack;
		std::vector<ValueType> stack;

		void pushControlStack(ControlContext::Type type,ResultType branchArgumentType,ResultType resultType)
		{
			controlStack.push_back({type,stack.size(),branchArgumentType,resultType,true});
		}

		void popControlStack(bool isElse = false)
		{
			VALIDATE_UNLESS("stack was not empty at end of control structure: ",stack.size() > controlStack.back().outerStackSize);

			if(isElse && controlStack.back().type == ControlContext::Type::ifThen)
			{
				controlStack.back().type = ControlContext::Type::ifElse;
				controlStack.back().isReachable = true;
			}
			else
			{
				VALIDATE_UNLESS("else only allowed in if context: ",isElse);
				const ResultType resultType = controlStack.back().resultType;
				if(controlStack.back().type == ControlContext::Type::ifThen && resultType != ResultType::none)
				{
					throw ValidationException("else-less if may not yield a result");
				}
				controlStack.pop_back();
				if(controlStack.size()) { pushOperand(resultType); }
			}
		}

		void enterUnreachable()
		{
			stack.resize(controlStack.back().outerStackSize);
			controlStack.back().isReachable = false;
		}

		void validateBranchDepth(uintp depth) const
		{
			VALIDATE_INDEX(depth,controlStack.size());
			if(depth >= controlStack.size()) { throw ValidationException("invalid branch depth"); }
		}

		const ControlContext& getBranchTargetByDepth(uintp depth) const
		{
			validateBranchDepth(depth);
			return controlStack[controlStack.size() - depth - 1];
		}

		ValueType validateLocalIndex(uintp localIndex)
		{
			VALIDATE_INDEX(localIndex,locals.size());
			return locals[localIndex];
		}
		
		ValueType popOperand()
		{
			if(stack.size() > controlStack.back().outerStackSize)
			{
				const ValueType result = stack.back();
				stack.pop_back();
				return result;
			}
			else if(controlStack.back().isReachable)
			{
				throw ValidationException("invalid stack access");
			}
			else
			{
				return ValueType::any;
			}
		}

		void popAndValidateOperands(const char* context,const ValueType* expectedTypes,size_t num)
		{
			for(uintp operandIndexFromEnd = 0;operandIndexFromEnd < num;++operandIndexFromEnd)
			{
				const uintp operandIndex = num - operandIndexFromEnd - 1;
				const ValueType actualType = popOperand();
				validateOperandType(expectedTypes[operandIndex],actualType,context);
			}
		}

		template<size_t num>
		void popAndValidateOperands(const char* context,const ValueType (&expectedTypes)[num]) { popAndValidateOperands(context,expectedTypes,num); }
		
		template<typename... OperandTypes>
		void popAndValidateOperands(const char* context,OperandTypes... operands)
		{
			ValueType operandTypes[] = {operands...};
			popAndValidateOperands(context,operandTypes);
		}

		void popAndValidateOperand(const char* context,const ValueType expectedType)
		{
			const ValueType actualType = popOperand();
			validateOperandType(expectedType,actualType,context);
		}

		void popAndValidateResultType(const char* context,ResultType expectedType)
		{
			if(expectedType != ResultType::none) { popAndValidateOperand(context,asValueType(expectedType)); }
		}

		void pushOperand(ValueType type)
		{
			stack.push_back(type);
		}
		void pushOperand(ResultType type)
		{
			if(type != ResultType::none) { pushOperand(asValueType(type)); }
		}
	};
	
	void validateDefinitions(const Module& module)
	{
		Core::Timer timer;
		
		for(uintp typeIndex = 0;typeIndex < module.types.size();++typeIndex)
		{
			const FunctionType* functionType = module.types[typeIndex];
			for(auto parameterType : functionType->parameters) { validate(parameterType); }
			validate(functionType->ret);
		}

		for(auto& functionImport : module.functions.imports)
		{
			VALIDATE_INDEX(functionImport.type.index,module.types.size());
		}
		for(auto& tableImport : module.tables.imports)
		{
			validate(tableImport.type.elementType);
			validate(tableImport.type.size,UINTPTR_MAX);
		}
		for(auto& memoryImport : module.memories.imports)
		{
			validate(memoryImport.type.size,IR::maxMemoryPages);
		}
		for(auto& globalImport : module.globals.imports)
		{
			validate(globalImport.type);
			VALIDATE_UNLESS("mutable globals cannot be imported: ",globalImport.type.isMutable);
		}
		
		for(uintp functionDefIndex = 0;functionDefIndex < module.functions.defs.size();++functionDefIndex)
		{
			const FunctionDef& functionDef = module.functions.defs[functionDefIndex];
			VALIDATE_INDEX(functionDef.type.index,module.types.size());
			for(auto localType : functionDef.nonParameterLocalTypes) { validate(localType); }
		}

		for(auto& globalDef : module.globals.defs)
		{
			validate(globalDef.type);
			validateInitializer(module,globalDef.initializer,globalDef.type.valueType,"global initializer expression");
		}
		
		for(auto& tableDef : module.tables.defs) { validate(tableDef.type.size,UINT32_MAX); }
		VALIDATE_UNLESS("too many tables: ",module.tables.size()>1);

		for(auto& memoryDef : module.memories.defs) { validate(memoryDef.type.size,IR::maxMemoryPages); }
		VALIDATE_UNLESS("too many memories: ",module.memories.size()>1);

		std::set<std::string> exportNameMap;
		for(auto& exportIt : module.exports)
		{
			switch(exportIt.kind)
			{
			case ObjectKind::function:
				VALIDATE_INDEX(exportIt.index,module.functions.size());
				break;
			case ObjectKind::table:
				VALIDATE_INDEX(exportIt.index,module.tables.size());
				break;
			case ObjectKind::memory:
				VALIDATE_INDEX(exportIt.index,module.memories.size());
				break;
			case ObjectKind::global:
				validateGlobalIndex(module,exportIt.index,false,true,false,"exported global index");
				break;
			default: throw ValidationException("unknown export kind");
			};

			VALIDATE_UNLESS("duplicate export: ",exportNameMap.count(exportIt.name));
			exportNameMap.emplace(exportIt.name);
		}

		if(module.startFunctionIndex != UINTPTR_MAX)
		{
			VALIDATE_INDEX(module.startFunctionIndex,module.functions.size());
			const FunctionType* startFunctionType = module.types[module.functions.getType(module.startFunctionIndex).index];
			VALIDATE_UNLESS("start function must not have any parameters or results: ",startFunctionType != FunctionType::get());
		}
			
		for(auto& dataSegment : module.dataSegments)
		{
			VALIDATE_INDEX(dataSegment.memoryIndex,module.memories.size());
			validateInitializer(module,dataSegment.baseOffset,ValueType::i32,"data segment base initializer");
		}

		for(auto& tableSegment : module.tableSegments)
		{
			VALIDATE_INDEX(tableSegment.tableIndex,module.tables.size());
			validateInitializer(module,tableSegment.baseOffset,ValueType::i32,"table segment base initializer");
			for(auto functionIndex : tableSegment.indices) { VALIDATE_INDEX(functionIndex,module.functions.size()); }
		}

		Log::printf(Log::Category::metrics,"Validated WebAssembly module definitions in %.2fms\n",timer.getMilliseconds());
	}

	void validateCode(const Module& module)
	{
		Core::Timer timer;

		for(uintp functionDefIndex = 0;functionDefIndex < module.functions.defs.size();++functionDefIndex)
		{
			const FunctionDef& functionDef = module.functions.defs[functionDefIndex];
			FunctionValidationContext functionContext(module,functionDef);

			Serialization::MemoryInputStream codeStream(module.code.data() + functionDef.code.offset,functionDef.code.numBytes);
			OperationDecoder decoder(codeStream);
			if(ENABLE_LOGGING)
			{
				OperatorLoggingProxy<FunctionValidationContext> loggingProxy(module,functionContext);
				functionContext.logOperator("---- function start ----");
				while(decoder && functionContext.getControlStackSize()) { decoder.decodeOp(loggingProxy); };
			}
			else
			{
				while(decoder && functionContext.getControlStackSize()) { decoder.decodeOp(functionContext); };
			}
			
			if(decoder) { throw ValidationException("function end reached before end of code"); }
			if(functionContext.getControlStackSize()) { throw ValidationException("end of code reached before end of function"); }
		}

		Log::printf(Log::Category::metrics,"Validated WebAssembly module code in %.2fms\n",timer.getMilliseconds());
	}

	struct CodeValidationStreamImpl
	{
		FunctionValidationContext functionContext;
		OperatorLoggingProxy<FunctionValidationContext> loggingProxy;

		CodeValidationStreamImpl(const Module& module,const FunctionDef& functionDef)
		: functionContext(module,functionDef)
		, loggingProxy(module,functionContext)
		{}
	};

	CodeValidationStream::CodeValidationStream(const Module& module,const FunctionDef& functionDef)
	{
		impl = new CodeValidationStreamImpl(module,functionDef);
	}

	CodeValidationStream::~CodeValidationStream()
	{
		delete impl;
		impl = nullptr;
	}

	void CodeValidationStream::finish()
	{
		if(impl->functionContext.getControlStackSize()) { throw ValidationException("end of code reached before end of function"); }
	}

	#define VISIT_OPCODE(_,name,nameString,Imm) \
		void CodeValidationStream::name(Imm imm) \
		{ \
			if(ENABLE_LOGGING) { impl->loggingProxy.name(imm); } \
			else { impl->functionContext.name(imm); } \
		}
	ENUM_OPERATORS(VISIT_OPCODE)
	#undef VISIT_OPCODE
}