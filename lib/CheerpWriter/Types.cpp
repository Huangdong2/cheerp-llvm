//===-- Types.cpp - The Cheerp JavaScript generator -----------------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2014 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "llvm/Cheerp/Writer.h"
#include "llvm/Cheerp/Utility.h"
#include <stdio.h>

using namespace cheerp;
using namespace llvm;

void CheerpWriter::compileTypedArrayType(Type* t)
{
	if(t->isIntegerTy(8))
		stream << "Uint8Array";
	else if(t->isIntegerTy(16))
		stream << "Uint16Array";
	else if(t->isIntegerTy(32))
		stream << "Int32Array";
	else if(t->isFloatTy())
		stream << "Float32Array";
	else if(t->isDoubleTy())
		stream << "Float64Array";
	else
	{
		llvm::errs() << "Typed array requested for type " << *t << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
	}
}

void CheerpWriter::compileTypeImpl(Type* t, COMPILE_TYPE_STYLE style)
{
	switch(t->getTypeID())
	{
		case Type::IntegerTyID:
		{
			//We only really have 32bit integers.
			//We will allow anything shorter.
			//NOTE: Only bit operations are allowed on shorter types
			//this is enforced on a per-operation basis
			//Print out a '0' to let the engine know this is an integer.
			stream << '0';
			break;
		}
		case Type::FloatTyID:
		case Type::DoubleTyID:
		{
			stream << '0';
			break;
		}
		case Type::StructTyID:
		{
			//Special case union first
			if(TypeSupport::hasByteLayout(t))
			{
				uint32_t typeSize = targetData.getTypeAllocSize(t);
				stream << "new DataView(new ArrayBuffer(";
				stream << typeSize;
				stream << "))";
				break;
			}
			StructType* st=static_cast<StructType*>(t);
			if(style == LITERAL_OBJ)
				stream << '{';
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				if(offset!=0)
				{
					if(style==LITERAL_OBJ)
						stream << ',';
					else
						stream << ';' << NewLine;
				}
				if(style==THIS_OBJ)
					stream << "this.";
				stream << 'a' << offset << '0';
				if(style==LITERAL_OBJ)
					stream << ':';
				else
					stream << '=';
				compileType(*E, LITERAL_OBJ);
				offset++;
			}
			if(style == LITERAL_OBJ)
				stream << '}';
			break;
		}
		case Type::PointerTyID:
		{
			if(PA.getPointerKindForStoredType(t)==COMPLETE_OBJECT)
				stream << "null";
			else
				stream << "nullObj";
			break;
		}
		case Type::ArrayTyID:
		{
			ArrayType* at=static_cast<ArrayType*>(t);
			Type* et=at->getElementType();
			//For numerical types, create typed arrays
			if(types.isTypedArrayType(et) && at->getNumElements()>1)
			{
				stream << "new ";
				compileTypedArrayType(et);
				stream << '(' << at->getNumElements() << ')';
			}
			else
			{
				stream << '[';
				for(uint64_t i=0;i<at->getNumElements();i++)
				{
					compileType(at->getElementType(), LITERAL_OBJ);
					if((i+1)<at->getNumElements())
						stream << ',';
				}
				stream << ']';
			}
			break;
		}
		default:
			llvm::errs() << "Support type ";
			t->dump();
			llvm::errs() << '\n';
	}
}

void CheerpWriter::compileType(Type* t, COMPILE_TYPE_STYLE style)
{
	bool addDowncastArray = false;
	if(StructType* st=dyn_cast<StructType>(t))
	{
		//TODO: Verify that it makes sense to assume struct with no name has no bases
		if(st->hasName() && module.getNamedMetadata(Twine(st->getName(),"_bases")) &&
			globalDeps.classesWithBaseInfo().count(st))
		{
			addDowncastArray = true;
		}
	}
	if(addDowncastArray)
	{
		if(style==LITERAL_OBJ)
		{
			stream << "create" << namegen.filterLLVMName(cast<StructType>(t)->getName(), true) << '(';
			compileTypeImpl(t, LITERAL_OBJ);
			stream << ')';
		}
		else
		{
			compileTypeImpl(t, THIS_OBJ);
			stream << "create" << namegen.filterLLVMName(cast<StructType>(t)->getName(), true) << "(this)";
		}
	}
	else
		compileTypeImpl(t, style);
}

uint32_t CheerpWriter::compileClassTypeRecursive(const std::string& baseName, StructType* currentType, uint32_t baseCount)
{
	stream << "a[" << baseCount << "]=" << baseName << ';' << NewLine;
	stream << baseName << ".o=" << baseCount << ';' << NewLine;
	stream << baseName << ".a=a;" << NewLine;
	baseCount++;

	uint32_t firstBase, localBaseCount;
	if(!types.getBasesInfo(currentType, firstBase, localBaseCount))
		return baseCount;
	//baseCount has been already incremented above

	for(uint32_t i=firstBase;i<(firstBase+localBaseCount);i++)
	{
		SmallString<16> buf;
		llvm::raw_svector_ostream bufStream(buf);
		bufStream << ".a" << i << '0';
		bufStream.flush();
		baseCount=compileClassTypeRecursive(baseName+buf.c_str(), cast<StructType>(currentType->getElementType(i)), baseCount);
	}
	return baseCount;
}

void CheerpWriter::compileClassType(StructType* T)
{
	if(!T->hasName())
	{
		llvm::errs() << "Expected name for struct " << *T << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return;
	}
	//This function is used as a constructor using the new syntax
	stream << "function create" << namegen.filterLLVMName(T->getName(), true) << "(obj){" << NewLine;

	NamedMDNode* basesNamedMeta=module.getNamedMetadata(Twine(T->getName(),"_bases"));
	assert(basesNamedMeta);
	MDNode* basesMeta=basesNamedMeta->getOperand(0);
	assert(basesMeta->getNumOperands()==2);
	uint32_t baseMax=getIntFromValue(basesMeta->getOperand(1));
	stream << "var a=new Array(" << baseMax << ");" << NewLine;
	compileClassTypeRecursive("obj", T, 0);
	stream << "return obj;}" << NewLine;
}

void CheerpWriter::compileArrayClassType(StructType* T)
{
	if(!T->hasName())
	{
		llvm::errs() << "Expected name for struct " << *T << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return;
	}
	stream << "function createArray";
	stream << namegen.filterLLVMName(T->getName(), true);
	stream << "(ret,start){" << NewLine;
	stream << "for(var __i__=start;__i__<ret.length;__i__++)" << NewLine;
	stream << "ret[__i__]=";
	compileType(T, LITERAL_OBJ);
	stream << ';' << NewLine << "return ret;" << NewLine << '}' << NewLine;
}

void CheerpWriter::compileArrayPointerType()
{
	stream << "function createPointerArray(ret, start) { for(var __i__=start;__i__<ret.length;__i__++) ret[__i__]={ d: null, o: 0}; return ret; }"
		<< NewLine;
}

