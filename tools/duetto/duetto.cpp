//===-- duetto.cpp - The Duetto code splitter -----------------------------===//
//
//	Copyright 2011-2012 Leaning Technlogies
//===----------------------------------------------------------------------===//

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Duetto/Utils.h"
#include <memory>
#include <iostream>
using namespace llvm;
using namespace std;

// General options for llc.  Other pass-specific options are specified
// within the corresponding llc passes, and target-specific options
// and back-end code generation options are specified with the target machine.
//
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

// Determine optimization level.
static cl::opt<char>
OptLevel("O",
         cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                  "(default = '-O2')"),
         cl::Prefix,
         cl::ZeroOrMore,
         cl::init(' '));

static cl::opt<std::string>
TargetTriple("mtriple", cl::desc("Override target triple for module"));

static cl::opt<std::string>
MArch("march", cl::desc("Architecture to generate code for (see --version)"));

static cl::opt<std::string>
MCPU("mcpu",
  cl::desc("Target a specific cpu type (-mcpu=help for details)"),
  cl::value_desc("cpu-name"),
  cl::init(""));

static cl::list<std::string>
MAttrs("mattr",
  cl::CommaSeparated,
  cl::desc("Target specific attributes (-mattr=help for details)"),
  cl::value_desc("a1,+a2,-a3,..."));

static cl::opt<Reloc::Model>
RelocModel("relocation-model",
             cl::desc("Choose relocation model"),
             cl::init(Reloc::Default),
             cl::values(
            clEnumValN(Reloc::Default, "default",
                       "Target default relocation model"),
            clEnumValN(Reloc::Static, "static",
                       "Non-relocatable code"),
            clEnumValN(Reloc::PIC_, "pic",
                       "Fully relocatable, position independent code"),
            clEnumValN(Reloc::DynamicNoPIC, "dynamic-no-pic",
                       "Relocatable external references, non-relocatable code"),
            clEnumValEnd));

static cl::opt<llvm::CodeModel::Model>
CMModel("code-model",
        cl::desc("Choose code model"),
        cl::init(CodeModel::Default),
        cl::values(clEnumValN(CodeModel::Default, "default",
                              "Target default code model"),
                   clEnumValN(CodeModel::Small, "small",
                              "Small code model"),
                   clEnumValN(CodeModel::Kernel, "kernel",
                              "Kernel code model"),
                   clEnumValN(CodeModel::Medium, "medium",
                              "Medium code model"),
                   clEnumValN(CodeModel::Large, "large",
                              "Large code model"),
                   clEnumValEnd));

static cl::opt<bool>
RelaxAll("mc-relax-all",
  cl::desc("When used with filetype=obj, "
           "relax all fixups in the emitted object file"));

cl::opt<TargetMachine::CodeGenFileType>
FileType("filetype", cl::init(TargetMachine::CGFT_AssemblyFile),
  cl::desc("Choose a file type (not all types are supported by all targets):"),
  cl::values(
       clEnumValN(TargetMachine::CGFT_AssemblyFile, "asm",
                  "Emit an assembly ('.s') file"),
       clEnumValN(TargetMachine::CGFT_ObjectFile, "obj",
                  "Emit a native object ('.o') file [experimental]"),
       clEnumValN(TargetMachine::CGFT_Null, "null",
                  "Emit nothing, for performance testing"),
       clEnumValEnd));

cl::opt<bool> NoVerify("disable-verify", cl::Hidden,
                       cl::desc("Do not verify input module"));

cl::opt<bool> DisableDotLoc("disable-dot-loc", cl::Hidden,
                            cl::desc("Do not use .loc entries"));

cl::opt<bool> DisableCFI("disable-cfi", cl::Hidden,
                         cl::desc("Do not use .cfi_* directives"));

cl::opt<bool> EnableDwarfDirectory("enable-dwarf-directory", cl::Hidden,
    cl::desc("Use .file directives with an explicit directory."));

static cl::opt<bool>
DisableRedZone("disable-red-zone",
  cl::desc("Do not emit code that uses the red zone."),
  cl::init(false));

static cl::opt<bool>
EnableFPMAD("enable-fp-mad",
  cl::desc("Enable less precise MAD instructions to be generated"),
  cl::init(false));

static cl::opt<bool>
PrintCode("print-machineinstrs",
  cl::desc("Print generated machine code"),
  cl::init(false));

static cl::opt<bool>
DisableFPElim("disable-fp-elim",
  cl::desc("Disable frame pointer elimination optimization"),
  cl::init(false));

static cl::opt<bool>
DisableFPElimNonLeaf("disable-non-leaf-fp-elim",
  cl::desc("Disable frame pointer elimination optimization for non-leaf funcs"),
  cl::init(false));

static cl::opt<bool>
DisableExcessPrecision("disable-excess-fp-precision",
  cl::desc("Disable optimizations that may increase FP precision"),
  cl::init(false));

static cl::opt<bool>
EnableUnsafeFPMath("enable-unsafe-fp-math",
  cl::desc("Enable optimizations that may decrease FP precision"),
  cl::init(false));

static cl::opt<bool>
EnableNoInfsFPMath("enable-no-infs-fp-math",
  cl::desc("Enable FP math optimizations that assume no +-Infs"),
  cl::init(false));

static cl::opt<bool>
EnableNoNaNsFPMath("enable-no-nans-fp-math",
  cl::desc("Enable FP math optimizations that assume no NaNs"),
  cl::init(false));

static cl::opt<bool>
EnableHonorSignDependentRoundingFPMath("enable-sign-dependent-rounding-fp-math",
  cl::Hidden,
  cl::desc("Force codegen to assume rounding mode can change dynamically"),
  cl::init(false));

static cl::opt<bool>
GenerateSoftFloatCalls("soft-float",
  cl::desc("Generate software floating point library calls"),
  cl::init(false));

static cl::opt<llvm::FloatABI::ABIType>
FloatABIForCalls("float-abi",
  cl::desc("Choose float ABI type"),
  cl::init(FloatABI::Default),
  cl::values(
    clEnumValN(FloatABI::Default, "default",
               "Target default float ABI type"),
    clEnumValN(FloatABI::Soft, "soft",
               "Soft float ABI (implied by -soft-float)"),
    clEnumValN(FloatABI::Hard, "hard",
               "Hard float ABI (uses FP registers)"),
    clEnumValEnd));

static cl::opt<bool>
DontPlaceZerosInBSS("nozero-initialized-in-bss",
  cl::desc("Don't place zero-initialized symbols into bss section"),
  cl::init(false));

static cl::opt<bool>
EnableGuaranteedTailCallOpt("tailcallopt",
  cl::desc("Turn fastcc calls into tail calls by (potentially) changing ABI."),
  cl::init(false));

static cl::opt<bool>
DisableTailCalls("disable-tail-calls",
  cl::desc("Never emit tail calls"),
  cl::init(false));

static cl::opt<unsigned>
OverrideStackAlignment("stack-alignment",
  cl::desc("Override default stack alignment"),
  cl::init(0));

static cl::opt<bool>
EnableRealignStack("realign-stack",
  cl::desc("Realign stack if needed"),
  cl::init(true));

static cl::opt<bool>
DisableSwitchTables(cl::Hidden, "disable-jump-tables",
  cl::desc("Do not generate jump tables."),
  cl::init(false));

static cl::opt<std::string>
TrapFuncName("trap-func", cl::Hidden,
  cl::desc("Emit a call to trap function rather than a trap instruction"),
  cl::init(""));

static cl::opt<bool>
EnablePIE("enable-pie",
  cl::desc("Assume the creation of a position independent executable."),
  cl::init(false));

static cl::opt<bool>
SegmentedStacks("segmented-stacks",
  cl::desc("Use segmented stacks if possible."),
  cl::init(false));


// GetFileNameRoot - Helper function to get the basename of a filename.
static inline std::string
GetFileNameRoot(const std::string &InputFilename) {
  std::string IFN = InputFilename;
  std::string outputFilename;
  int Len = IFN.length();
  if ((Len > 2) &&
      IFN[Len-3] == '.' &&
      ((IFN[Len-2] == 'b' && IFN[Len-1] == 'c') ||
       (IFN[Len-2] == 'l' && IFN[Len-1] == 'l'))) {
    outputFilename = std::string(IFN.begin(), IFN.end()-3); // s/.bc/.s/
  } else {
    outputFilename = IFN;
  }
  return outputFilename;
}

class DuettoWriter
{
public:
	void rewriteServerMethod(Module& M, Function& F);
	void rewriteClientMethod(Function& F);
	Function* getMeta(Function& F, Module& M, int index);
	Constant* getSkel(Function& F, Module& M, StructType* mapType);
	Function* getStub(Function& F, Module& M);
	void makeClient(Module* M);
	void makeServer(Module* M);
};

void DuettoWriter::rewriteServerMethod(Module& M, Function& F)
{
	std::cerr << "CLIENT: Deleting body of server function " << (std::string)F.getName() << std::endl;
	F.deleteBody();
	//Create a new basic in the function, since everything has been deleted
	BasicBlock* bb=BasicBlock::Create(M.getContext(),"entry",&F);

	SmallVector<Value*, 4> args;
	args.reserve(F.arg_size()+1);
	Constant* nameConst = ConstantDataArray::getString(M.getContext(), F.getName());
	llvm::Constant *Zero = llvm::Constant::getNullValue(Type::getInt32Ty(M.getContext()));
	llvm::Constant *Zeros[] = { Zero, Zero };

	GlobalVariable *nameGV = new llvm::GlobalVariable(M, nameConst->getType(), true,
			GlobalVariable::PrivateLinkage, nameConst, ".str");

	Constant* ptrNameConst = ConstantExpr::getGetElementPtr(nameGV, Zeros);
	args.push_back(ptrNameConst);
	for(Function::arg_iterator it=F.arg_begin();it!=F.arg_end();++it)
		args.push_back(&(*it));
	Function* stub=getStub(F,M);

	//Detect if the first two parameters are in the wrong order, this may
	//happen when the return value is a complex type and becomes an argument
	llvm::Value* firstArg = stub->arg_begin();
	llvm::Value* secondArg = (++stub->arg_begin());
	if(stub->arg_size()>1 && !(args[0]->getType()==firstArg->getType() &&
		args[1]->getType()==secondArg->getType()))
	{
		std::cerr << "Inverting first two parameters" << std::endl;
		llvm::Value* tmp;
		tmp=args[0];
		args[0]=args[1];
		args[1]=tmp;
	}

	assert(stub->arg_size()==1 || (args[0]->getType()==firstArg->getType() &&
		args[1]->getType()==secondArg->getType()));

	Value* skelFuncCall=CallInst::Create(stub,args,"",bb);
	if(skelFuncCall->getType()->isVoidTy())
		ReturnInst::Create(M.getContext(),bb);
	else
		ReturnInst::Create(M.getContext(),skelFuncCall,bb);
}

void DuettoWriter::makeClient(Module* M)
{
	SmallVector<Function*, 4> toRemove;

	Module::iterator F=M->begin();
	Module::iterator FE=M->end();
	for (; F != FE;)
	{
		Function& current=*F;
		++F;
		//Make stubs out of server side code
		//Make sure custom attributes are removed, they
		//may confuse emscripten
		if(current.hasFnAttribute(Attribute::Server))
			rewriteServerMethod(*M, current);
		else
			DuettoUtils::rewriteNativeObjectsConstructors(*M, current);
		current.removeFnAttr(Attribute::Client);
		current.removeFnAttr(Attribute::Server);
	}
	Module::named_metadata_iterator mdIt=M->named_metadata_begin();
	Module::named_metadata_iterator mdEnd=M->named_metadata_end();
	for(; mdIt!=mdEnd;)
	{
		NamedMDNode& current=*mdIt;
		++mdIt;
		M->eraseNamedMetadata(&current);
	}
	for(unsigned i=0;i<toRemove.size();i++)
		toRemove[i]->eraseFromParent();
}

void DuettoWriter::rewriteClientMethod(Function& F)
{
	std::cerr << "SERVER: Deleting body of client function " << (std::string)F.getName() << std::endl;
	F.deleteBody();
}

Constant* DuettoWriter::getSkel(Function& F, Module& M, StructType* mapType)
{
	LLVMContext& C = M.getContext();
	Function* skelFunc=getMeta(F,M,0);
	//Make the function external, it should be visible from the outside
	Constant* nameConst = ConstantDataArray::getString(C, F.getName());
	
	llvm::Constant *Zero = llvm::Constant::getNullValue(Type::getInt32Ty(C));
	llvm::Constant *Zeros[] = { Zero, Zero };

	GlobalVariable *nameGV = new llvm::GlobalVariable(M, nameConst->getType(), true,
			GlobalVariable::PrivateLinkage, nameConst, ".str");

	Constant* ptrNameConst = ConstantExpr::getGetElementPtr(nameGV, Zeros);
	vector<Constant*> structFields;
	structFields.push_back(ptrNameConst);
	structFields.push_back(skelFunc);
	return ConstantStruct::get(mapType, structFields);
}

Function* DuettoWriter::getMeta(Function& F, Module& M, int index)
{
	llvm::Twine skelName=F.getName()+"_duettoSkel";
	cout << "SERVER: Generating skeleton for " << (std::string)F.getName() << endl;
	NamedMDNode* meta=M.getNamedMetadata(skelName);

	Value* val=meta->getOperand(0)->getOperand(index);
	Function* skelFunc=dyn_cast<llvm::Function>(val);
	assert(skelFunc);
	return skelFunc;
}

Function* DuettoWriter::getStub(Function& F, Module& M)
{
	return getMeta(F,M,1);
}

void DuettoWriter::makeServer(Module* M)
{
	vector<Function*> serverFunction;
	Module::iterator F=M->begin();
	Module::iterator FE=M->end();
	for (; F != FE;)
	{
		Function& current=*F;
		++F;
		//Delete client side code and
		//save aside the function that needs a skel
		if(current.hasFnAttribute(Attribute::Client))
			rewriteClientMethod(current);
		else if(current.hasFnAttribute(Attribute::Server))
			serverFunction.push_back(&current);
	}

	//Count the function we have to work on
	LLVMContext& C=M->getContext();
	//Add a global structure for name and function pairs
	Type* bytePtrType = Type::getInt8PtrTy(C);
	vector<Type*> funcTypes;
	funcTypes.push_back(bytePtrType);
	funcTypes.push_back(bytePtrType);
	FunctionType* FT=FunctionType::get(Type::getVoidTy(C), funcTypes, false);
	vector<Type*> structTypes;
	structTypes.push_back(Type::getInt8PtrTy(C));
	structTypes.push_back(PointerType::getUnqual(FT));
	StructType* mapType=StructType::create(C, structTypes);
	vector<Constant*> funcs;

	for(unsigned int i=0;i<serverFunction.size();i++)
	{
		Constant* funcMap=getSkel(*serverFunction[i], *M, mapType);
		funcs.push_back(funcMap);
	}

	Constant* nulls[] = { ConstantPointerNull::get(static_cast<PointerType*>(structTypes[0])),
				ConstantPointerNull::get(static_cast<PointerType*>(structTypes[1]))};
	Constant* nullEntry = ConstantStruct::get(mapType, nulls);
	funcs.push_back(nullEntry);

	ArrayType* arrayType=ArrayType::get(mapType, funcs.size());
	Constant* mapConst = ConstantArray::get(arrayType, funcs);
	GlobalVariable* mapVar = new GlobalVariable(*M, arrayType, true,
			GlobalVariable::ExternalLinkage, mapConst, "duettoFuncMap");
}

// main - Entry point for the duetto double target compiler.
//
int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  // Enable debug stream buffering.
  EnableDebugBuffering = true;

  LLVMContext &Context = getGlobalContext();
  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // Initialize targets first, so that --version shows registered targets.
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // Register the target printer for --version.
  cl::AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion);

  cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

  // Load the module to be compiled...
  SMDiagnostic Err;
  std::auto_ptr<Module> M;

  M.reset(ParseIRFile(InputFilename, Err, Context));
  if (M.get() == 0) {
    Err.print(argv[0], errs());
    return 1;
  }
  Module* clientMod = M.get();
  //First of all kill the llvm.used global var
  GlobalVariable* g=clientMod->getGlobalVariable("llvm.used");
  if(g)
	  g->eraseFromParent();
  Module* serverMod = CloneModule(clientMod);

  DuettoWriter writer;
  writer.makeClient(clientMod);
  writer.makeServer(serverMod);

  std::string ClientOutputFilename = GetFileNameRoot(InputFilename) + "-client-base.bc";

  std::string errorInfo;
  raw_fd_ostream ClientOut(ClientOutputFilename.c_str(),errorInfo);
  std::cerr << errorInfo << std::endl;

  WriteBitcodeToFile(clientMod, ClientOut);

  ClientOut.close();

  const std::string nativeTriple=sys::getDefaultTargetTriple();
  std::cout << "Compiling for " << nativeTriple << std::endl;
  const Target* theTarget = TargetRegistry::lookupTarget(nativeTriple, errorInfo);
  
  TargetOptions options;
  TargetMachine* target=theTarget->createTargetMachine(nativeTriple, "", "", options,
                                          Reloc::Static, CodeModel::Default, CodeGenOpt::None);
  assert(target);
  // Build up all of the passes that we want to do to the module.
  PassManager PM;

  // Add the target data from the target machine, if it exists, or the module.
  if (const DataLayout *TD = target->getDataLayout())
    PM.add(new DataLayout(*TD));
  else
    PM.add(new DataLayout(serverMod));

  std::string ServerOutputFilename = GetFileNameRoot(InputFilename) + "-server.o";

  raw_fd_ostream ServerOut(ServerOutputFilename.c_str(),errorInfo);
  std::cerr << errorInfo << std::endl;
  {
    formatted_raw_ostream FOS(ServerOut);

    // Ask the target to add backend passes as necessary.
    if (target->addPassesToEmitFile(PM, FOS, TargetMachine::CGFT_ObjectFile, NoVerify)) {
      errs() << argv[0] << ": target does not support generation of this"
             << " file type!\n";
      return 1;
    }

    // Before executing passes, print the final values of the LLVM options.
    cl::PrintOptionValues();

    PM.run(*serverMod);
  }
  ServerOut.close();

  /*// If we are supposed to override the target triple, do so now.
  if (!TargetTriple.empty())
    mod.setTargetTriple(Triple::normalize(TargetTriple));

  if (TheTriple.getTriple().empty())
    TheTriple.setTriple(sys::getDefaultTargetTriple());

  } else {
    std::string Err;
    TheTarget = TargetRegistry::lookupTarget(TheTriple.getTriple(), Err);
    if (TheTarget == 0) {
      errs() << argv[0] << ": error auto-selecting target for module '"
             << Err << "'.  Please use the -march option to explicitly "
             << "pick a target.\n";
      return 1;
    }
  }

  // Package up features to be passed to target/subtarget
  std::string FeaturesStr;
  if (MAttrs.size()) {
    SubtargetFeatures Features;
    for (unsigned i = 0; i != MAttrs.size(); ++i)
      Features.AddFeature(MAttrs[i]);
    FeaturesStr = Features.getString();
  }

  CodeGenOpt::Level OLvl = CodeGenOpt::Default;
  switch (OptLevel) {
  default:
    errs() << argv[0] << ": invalid optimization level.\n";
    return 1;
  case ' ': break;
  case '0': OLvl = CodeGenOpt::None; break;
  case '1': OLvl = CodeGenOpt::Less; break;
  case '2': OLvl = CodeGenOpt::Default; break;
  case '3': OLvl = CodeGenOpt::Aggressive; break;
  }

  TargetOptions Options;
  Options.LessPreciseFPMADOption = EnableFPMAD;
  Options.PrintMachineCode = PrintCode;
  Options.NoFramePointerElim = DisableFPElim;
  Options.NoFramePointerElimNonLeaf = DisableFPElimNonLeaf;
  Options.NoExcessFPPrecision = DisableExcessPrecision;
  Options.UnsafeFPMath = EnableUnsafeFPMath;
  Options.NoInfsFPMath = EnableNoInfsFPMath;
  Options.NoNaNsFPMath = EnableNoNaNsFPMath;
  Options.HonorSignDependentRoundingFPMathOption =
      EnableHonorSignDependentRoundingFPMath;
  Options.UseSoftFloat = GenerateSoftFloatCalls;
  if (FloatABIForCalls != FloatABI::Default)
    Options.FloatABIType = FloatABIForCalls;
  Options.NoZerosInBSS = DontPlaceZerosInBSS;
  Options.GuaranteedTailCallOpt = EnableGuaranteedTailCallOpt;
  Options.DisableTailCalls = DisableTailCalls;
  Options.StackAlignmentOverride = OverrideStackAlignment;
  Options.RealignStack = EnableRealignStack;
  Options.DisableJumpTables = DisableSwitchTables;
  Options.TrapFuncName = TrapFuncName;
  Options.PositionIndependentExecutable = EnablePIE;
  Options.EnableSegmentedStacks = SegmentedStacks;

  std::auto_ptr<TargetMachine>
    target(TheTarget->createTargetMachine(TheTriple.getTriple(),
                                          MCPU, FeaturesStr, Options,
                                          RelocModel, CMModel, OLvl));
  assert(target.get() && "Could not allocate target machine!");
  TargetMachine &Target = *target.get();

  if (DisableDotLoc)
    Target.setMCUseLoc(false);

  if (DisableCFI)
    Target.setMCUseCFI(false);

  if (EnableDwarfDirectory)
    Target.setMCUseDwarfDirectory(true);

  if (GenerateSoftFloatCalls)
    FloatABIForCalls = FloatABI::Soft;

  // Disable .loc support for older OS X versions.
  if (TheTriple.isMacOSX() &&
      TheTriple.isMacOSXVersionLT(10, 6))
    Target.setMCUseLoc(false);

  // Figure out where we are going to send the output...
  // Build up all of the passes that we want to do to the module.
  PassManager PM;

  // Add the target data from the target machine, if it exists, or the module.
  if (const TargetData *TD = Target.getTargetData())
    PM.add(new TargetData(*TD));
  else
    PM.add(new TargetData(&mod));

  // Override default to generate verbose assembly.
  Target.setAsmVerbosityDefault(true);

  if (RelaxAll) {
    if (FileType != TargetMachine::CGFT_ObjectFile)
      errs() << argv[0]
             << ": warning: ignoring -mc-relax-all because filetype != obj";
    else
      Target.setMCRelaxAll(true);
  }

  {
    formatted_raw_ostream FOS(Out->os());

    // Ask the target to add backend passes as necessary.
    if (Target.addPassesToEmitFile(PM, FOS, FileType, NoVerify)) {
      errs() << argv[0] << ": target does not support generation of this"
             << " file type!\n";
      return 1;
    }

    // Before executing passes, print the final values of the LLVM options.
    cl::PrintOptionValues();

    PM.run(mod);
  }

  // Declare success.
  Out->keep();*/

  return 0;
}