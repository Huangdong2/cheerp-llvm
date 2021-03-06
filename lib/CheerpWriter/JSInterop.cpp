//===-- JSInterop.cpp - The Cheerp JavaScript generator -------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2014 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "llvm/Cheerp/Writer.h"

using namespace llvm;
using namespace cheerp;

void CheerpWriter::compileClassesExportedToJs()
{
	//Look for metadata which ends in _methods. They are lists
	//of exported methods for JS layout classes
	for( Module::const_named_metadata_iterator it = module.named_metadata_begin(),
		itE = module.named_metadata_end(); it!=itE; ++it)
	{
		const NamedMDNode* namedNode = it;
		StringRef name = namedNode->getName();

		if (!name.endswith("_methods") || !name.startswith("class.") )
			continue;

		StringRef mangledName = name.drop_front(6).drop_back(8);

		demangler_iterator demangler( mangledName );

		StringRef jsClassName = *demangler++;

		if ( demangler != demangler_iterator() )
		{
			Twine errorString("Class: ",jsClassName);

			for ( ; demangler != demangler_iterator(); ++ demangler )
				errorString.concat("::").concat(*demangler);

			errorString.concat(" is not a valid [[jsexport]] class (not in global namespace)\n");

			llvm::report_fatal_error( errorString );
		}

		assert( jsClassName.end() > name.begin() && std::size_t(jsClassName.end() - name.begin()) <= name.size() );
		StructType * t = module.getTypeByName( StringRef(name.begin(), jsClassName.end() - name.begin() ) );
		assert(t);

		auto getMethodName = [&](const MDNode * node) -> StringRef
		{
			assert( isa<Function>(node->getOperand(0) ) );

			StringRef mangledName = cast<Function>(node->getOperand(0))->getName();

			demangler_iterator dmg(mangledName);
			if ( *dmg++ != jsClassName )
			{
				assert( false && "[[jsexport]]: method should be in class" );
			}

			StringRef functionName = *dmg++;

			assert( dmg == demangler_iterator() );
			return functionName;
		};

		//TODO many things to check.. For. ex C1/C2/C3, names collisions, template classes!
		auto isConstructor = [&](const MDNode * node ) -> bool
		{
			return getMethodName(node).startswith("C1");
		};

		auto constructor = std::find_if(namedNode->op_begin(), namedNode->op_end(), isConstructor );

		//First compile the constructor
		if (constructor == namedNode->op_end() )
		{
			llvm::report_fatal_error( Twine("Class: ", jsClassName).concat(" does not define a constructor!") );
			return;
		}

		if ( std::find_if( std::next(constructor), namedNode->op_end(), isConstructor ) != namedNode->op_end() )
		{
			llvm::report_fatal_error( Twine("More than one constructor defined for class: ", jsClassName) );
			return;
		}

		const MDNode* node = *constructor;
		const Function * f = cast<Function>(node->getOperand(0));

		stream << "function " << jsClassName << '(';
		for(uint32_t i=0;i<f->arg_size()-1;i++)
		{
			if(i!=0)
				stream << ",";
			stream << 'a' << i;
		}
		stream << "){" << NewLine;
		compileType(t, THIS_OBJ);
		//We need to manually add the self pointer
		stream << ';' << NewLine << "this.s0=this;" << NewLine;
		compileOperand(f);
		stream << "({d:this,o:'s'}";
		for(uint32_t i=0;i<f->arg_size()-1;i++)
			stream << ",a" << i;
		stream << ");" << NewLine << "}" << NewLine;

		assert( globalDeps.isReachable(f) );

		//Then compile other methods and add them to the prototype
		for ( NamedMDNode::const_op_iterator it = namedNode->op_begin(); it != namedNode->op_end(); ++ it )
		{
			if ( isConstructor(*it) )
				continue;

			StringRef methodName = getMethodName(*it);

			const MDNode * node = *it;
			const Function * f = cast<Function>(node->getOperand(0));

			stream << jsClassName << ".prototype." << methodName << "=function (";
			for(uint32_t i=0;i<f->arg_size()-1;i++)
			{
				if(i!=0)
					stream << ",";
				stream << 'a' << i;
			}
			stream << "){" << NewLine << "return ";
			compileOperand(f);
			stream << "({d:this,o:'s'}";
			for(uint32_t i=0;i<f->arg_size()-1;i++)
				stream << ",a" << i;
			stream << ");" << NewLine << '}' << NewLine;

			assert( globalDeps.isReachable(f) );
		}
	}
}
