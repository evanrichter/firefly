#include "lumen/llvm/Target.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/FunctionImport.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/MemorySanitizer.h"
#include "llvm/Transforms/Instrumentation/ThreadSanitizer.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/FunctionImportUtils.h"
//#include "llvm/Transforms/Scalar/PlaceSafepoints.h"
#include "llvm/Support/CBindingWrapping.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/Timer.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include "llvm/Transforms/Utils/CanonicalizeAliases.h"
#include "llvm/Transforms/Utils/NameAnonGlobals.h"

#include <set>
#include <stdio.h>
#include <vector>

using ::llvm::any_cast;
using ::llvm::any_isa;
using ::llvm::CGSCCAnalysisManager;
using ::llvm::FunctionAnalysisManager;
using ::llvm::FunctionPassManager;
using ::llvm::LoopAnalysisManager;
using ::llvm::Module;
using ::llvm::ModuleAnalysisManager;
using ::llvm::ModulePassManager;
using ::llvm::Pass;
using ::llvm::PassBuilder;
using ::llvm::PassInstrumentationCallbacks;
using ::llvm::StringRef;
using ::llvm::TargetMachine;
using ::llvm::unwrap;

typedef struct _Pass *LLVMPassRef;
typedef struct _ModulePassManager *ModulePassManagerRef;
typedef struct _FunctionPassManager *FunctionPassManagerRef;

DEFINE_STDCXX_CONVERSION_FUNCTIONS(Pass, LLVMPassRef);
DEFINE_STDCXX_CONVERSION_FUNCTIONS(ModulePassManager, ModulePassManagerRef);
DEFINE_STDCXX_CONVERSION_FUNCTIONS(FunctionPassManager, FunctionPassManagerRef);

extern "C" void LLVMLumenInitializePasses() {
  thread_local bool initialized = false;
  if (initialized)
    return;
  initialized = true;

  auto &registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeCore(registry);
  llvm::initializeCodeGen(registry);
  llvm::initializeScalarOpts(registry);
  llvm::initializeVectorization(registry);
  llvm::initializeIPO(registry);
  llvm::initializeAnalysis(registry);
  llvm::initializeTransformUtils(registry);
  llvm::initializeInstCombine(registry);
  llvm::initializeInstrumentation(registry);
  llvm::initializeTarget(registry);
}

extern "C" void LLVMTimeTraceProfilerInitialize() {
  llvm::timeTraceProfilerInitialize(/*timeTraceGranularity*/ 0,
                                    /*procName*/ "lumen");
}

extern "C" void LLVMTimeTraceProfilerFinish(const char *fileName) {
  StringRef fn(fileName);
  std::error_code ec;
  llvm::raw_fd_ostream out(fn, ec, llvm::sys::fs::CD_CreateAlways);

  llvm::timeTraceProfilerWrite(out);
  llvm::timeTraceProfilerCleanup();
}

extern "C" void LLVMLumenPrintPasses() {
  LLVMLumenInitializePasses();

  struct LumenPassListener : llvm::PassRegistrationListener {
    void passEnumerate(const llvm::PassInfo *Info) {
      auto passArg = Info->getPassArgument();
      auto passName = Info->getPassName();
      if (!passArg.empty()) {
        // These unsigned->signed casts could theoretically overflow, but
        // realistically never will (and even if, the result is implementation
        // defined, rather than plain UB)
        printf("%32.*s | %.*s\n", (int)passArg.size(), passArg.data(),
               (int)passName.size(), passName.data());
      }
    }
  } listener;

  auto *registry = llvm::PassRegistry::getPassRegistry();
  registry->enumerateWith(&listener);
}

extern "C" typedef void (*LLVMLumenSelfProfileBeforePassCallback)(
    void *,        // profiler
    const char *,  // pass name
    const char *); // IR name
extern "C" typedef void (*LLVMLumenSelfProfileAfterPassCallback)(
    /*profiler*/ void *);

std::string getWrappedIRName(const llvm::Any &wrappedIR) {
  if (any_isa<const Module *>(wrappedIR))
    return any_cast<const Module *>(wrappedIR)->getName().str();
  if (any_isa<const llvm::Function *>(wrappedIR))
    return any_cast<const llvm::Function *>(wrappedIR)->getName().str();
  if (any_isa<const llvm::Loop *>(wrappedIR))
    return any_cast<const llvm::Loop *>(wrappedIR)->getName().str();
  if (any_isa<const llvm::LazyCallGraph::SCC *>(wrappedIR))
    return any_cast<const llvm::LazyCallGraph::SCC *>(wrappedIR)->getName();
  return "<UNKNOWN>";
}

void LLVMSelfProfileInitializeCallbacks(
    PassInstrumentationCallbacks *pic, void *selfProfiler,
    LLVMLumenSelfProfileBeforePassCallback beforePassCallback,
    LLVMLumenSelfProfileAfterPassCallback afterPassCallback) {
  pic->registerBeforeNonSkippedPassCallback(
      [selfProfiler, beforePassCallback](StringRef pass, llvm::Any ir) {
        std::string passName = pass.str();
        std::string irName = getWrappedIRName(ir);
        beforePassCallback(selfProfiler, passName.c_str(), irName.c_str());
      });

  pic->registerAfterPassCallback(
      [selfProfiler, afterPassCallback](StringRef pass, llvm::Any ir,
                                        const llvm::PreservedAnalyses &) {
        afterPassCallback(selfProfiler);
      });

  pic->registerAfterPassInvalidatedCallback(
      [selfProfiler, afterPassCallback](StringRef pass,
                                        const llvm::PreservedAnalyses &) {
        afterPassCallback(selfProfiler);
      });

  pic->registerBeforeAnalysisCallback(
      [selfProfiler, beforePassCallback](StringRef pass, llvm::Any ir) {
        std::string passName = pass.str();
        std::string irName = getWrappedIRName(ir);
        beforePassCallback(selfProfiler, passName.c_str(), irName.c_str());
      });

  pic->registerAfterAnalysisCallback(
      [selfProfiler, afterPassCallback](StringRef pass, llvm::Any ir) {
        afterPassCallback(selfProfiler);
      });
}

namespace LLVMLumenPassBuilderOptLevel {
enum Level {
  O0 = 0,
  O1,
  O2,
  O3,
  Os,
  Oz,
};
}

static PassBuilder::OptimizationLevel
fromRust(LLVMLumenPassBuilderOptLevel::Level level) {
  switch (level) {
  case LLVMLumenPassBuilderOptLevel::O0:
    return PassBuilder::OptimizationLevel::O0;
  case LLVMLumenPassBuilderOptLevel::O1:
    return PassBuilder::OptimizationLevel::O1;
  case LLVMLumenPassBuilderOptLevel::O2:
    return PassBuilder::OptimizationLevel::O2;
  case LLVMLumenPassBuilderOptLevel::O3:
    return PassBuilder::OptimizationLevel::O3;
  case LLVMLumenPassBuilderOptLevel::Os:
    return PassBuilder::OptimizationLevel::Os;
  case LLVMLumenPassBuilderOptLevel::Oz:
    return PassBuilder::OptimizationLevel::Oz;
  default:
    llvm::report_fatal_error("invalid PassBuilder optimization level");
  }
}

namespace OptStage {
enum Stage {
  PreLinkNoLTO,
  PreLinkThinLTO,
  PreLinkFatLTO,
  ThinLTO,
  FatLTO,
};
}

struct SanitizerOptions {
  bool memory;
  bool thread;
  bool address;
  bool recover;
  int memoryTrackOrigins;
};

inline static bool sanitizerEnabled(SanitizerOptions &opts) {
  return opts.memory || opts.thread || opts.address;
}

struct OptimizerConfig {
  const char *passPipeline;
  LLVMLumenPassBuilderOptLevel::Level optLevel;
  OptStage::Stage stage;
  SanitizerOptions sanitizer;
  bool debug;
  bool verify;
  bool useThinLTOBuffers;
  bool disableSimplifyLibCalls;
  bool emitSummaryIndex;
  bool emitModuleHash;
  bool preserveUseListOrder;
  void *profiler;
  LLVMLumenSelfProfileBeforePassCallback beforePass;
  LLVMLumenSelfProfileAfterPassCallback afterPass;
};

extern "C" bool LLVMLumenOptimize(LLVMModuleRef m, LLVMTargetMachineRef tm,
                                  OptimizerConfig *conf, char **errorMessage) {
  TargetMachine *targetMachine = unwrap(tm);
  Module *mod = unwrap(m);
  OptimizerConfig config = *conf;

  auto optLevel = fromRust(config.optLevel);

  llvm::PipelineTuningOptions tuningOpts;
  tuningOpts.LoopInterleaving = false;
  tuningOpts.LoopVectorization = false;
  tuningOpts.SLPVectorization = false;
  tuningOpts.LoopUnrolling = false;
  tuningOpts.Coroutines = false;

  bool debug = config.debug;
  bool verify = config.verify;

  auto pic = std::make_unique<llvm::PassInstrumentationCallbacks>();
  // Populate the analysis managers with their respective passes
  PassBuilder pb(debug, targetMachine, tuningOpts, llvm::None, pic.get());

  // Enable standard instrumentation callbacks
  llvm::StandardInstrumentations si(debug);
  llvm::PassInstrumentationCallbacks &passCallbacks =
      *pb.getPassInstrumentationCallbacks();
  si.registerCallbacks(passCallbacks);

  auto *profiler = config.profiler;
  if (profiler) {
    LLVMSelfProfileInitializeCallbacks(&passCallbacks, profiler,
                                       config.beforePass, config.afterPass);
  }

  LoopAnalysisManager lam(debug);
  FunctionAnalysisManager fam(debug);
  CGSCCAnalysisManager cam(debug);
  ModuleAnalysisManager mam(debug);

  fam.registerPass([&] { return pb.buildDefaultAAPipeline(); });

  auto triple = targetMachine->getTargetTriple();
  std::unique_ptr<llvm::TargetLibraryInfoImpl> tlii(
      new llvm::TargetLibraryInfoImpl(triple));
  if (config.disableSimplifyLibCalls)
    tlii->disableAllFunctions();
  fam.registerPass([&] { return llvm::TargetLibraryAnalysis(*tlii); });

  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cam, mam);

  // We manually collect pipeline callbacks so we can apply them at O0, where
  // the PassBuilder does not create a pipeline.
  std::vector<
      std::function<void(ModulePassManager &, PassBuilder::OptimizationLevel)>>
      pipelineStartEPCallbacks;
  std::vector<
      std::function<void(ModulePassManager &, PassBuilder::OptimizationLevel)>>
      optimizerLastEPCallbacks;

  if (verify) {
    pipelineStartEPCallbacks.push_back(
        [verify](ModulePassManager &pm, PassBuilder::OptimizationLevel _level) {
          pm.addPass(llvm::VerifierPass());
        });
  }

  auto sanitizer = config.sanitizer;
  if (sanitizerEnabled(sanitizer)) {
    if (sanitizer.memory) {
      llvm::MemorySanitizerOptions mso(sanitizer.memoryTrackOrigins,
                                       sanitizer.recover,
                                       /*compilerKernel=*/false);
      pipelineStartEPCallbacks.push_back(
          [mso](ModulePassManager &pm, PassBuilder::OptimizationLevel _level) {
            pm.addPass(llvm::MemorySanitizerPass(mso));
          });
      optimizerLastEPCallbacks.push_back(
          [mso](ModulePassManager &pm, PassBuilder::OptimizationLevel level) {
            pm.addPass(llvm::createModuleToFunctionPassAdaptor(
                llvm::MemorySanitizerPass(mso)));
          });
    }
    if (sanitizer.thread) {
      pipelineStartEPCallbacks.push_back(
          [](ModulePassManager &pm, PassBuilder::OptimizationLevel _level) {
            pm.addPass(llvm::ThreadSanitizerPass());
          });
      optimizerLastEPCallbacks.push_back(
          [](ModulePassManager &pm, PassBuilder::OptimizationLevel level) {
            pm.addPass(llvm::ThreadSanitizerPass());
            pm.addPass(llvm::createModuleToFunctionPassAdaptor(
                llvm::ThreadSanitizerPass()));
          });
    }
    if (sanitizer.address) {
      pipelineStartEPCallbacks.push_back(
          [&](ModulePassManager &pm, PassBuilder::OptimizationLevel _level) {
            pm.addPass(
                llvm::RequireAnalysisPass<llvm::ASanGlobalsMetadataAnalysis,
                                          Module>());
          });
      optimizerLastEPCallbacks.push_back(
          [sanitizer](ModulePassManager &pm,
                      PassBuilder::OptimizationLevel level) {
            pm.addPass(llvm::createModuleToFunctionPassAdaptor(
                llvm::AddressSanitizerPass(
                    /*compileKernel=*/false, sanitizer.recover,
                    /*useAfterScope=*/true)));
          });
      pipelineStartEPCallbacks.push_back(
          [sanitizer](ModulePassManager &pm,
                      PassBuilder::OptimizationLevel _level) {
            pm.addPass(llvm::ModuleAddressSanitizerPass(
                /*compileKernel=*/false, sanitizer.recover));
          });
    }
  }

  ModulePassManager mpm(debug);

  // Add passes to generate safepoints/stack maps
  // mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::PlaceSafepoints()));
  mpm.addPass(llvm::RewriteStatepointsForGC());

  // If there is a pipeline provided, parse it and populate the pass manager
  // with it
  if (config.passPipeline) {
    std::string error;
    if (auto err = pb.parsePassPipeline(mpm, config.passPipeline)) {
      error = "unable to parse pass pipeline description '" +
              std::string(config.passPipeline) +
              "': " + llvm::toString(std::move(err));
      *errorMessage = strdup(error.c_str());
      return true;
    }
  } else if (optLevel == PassBuilder::OptimizationLevel::O0) {
    for (const auto &c : pipelineStartEPCallbacks)
      c(mpm, optLevel);

    if (!optimizerLastEPCallbacks.empty()) {
      for (const auto &c : optimizerLastEPCallbacks)
        c(mpm, optLevel);
    }

    mpm.addPass(llvm::AlwaysInlinerPass(/*insertLifetimeIntrinsics=*/false));
  } else {
    for (const auto &c : pipelineStartEPCallbacks)
      pb.registerPipelineStartEPCallback(c);
    if (config.stage != OptStage::PreLinkThinLTO) {
      for (const auto &c : optimizerLastEPCallbacks)
        pb.registerOptimizerLastEPCallback(c);
    }

    switch (config.stage) {
    case OptStage::PreLinkNoLTO:
      mpm = pb.buildPerModuleDefaultPipeline(optLevel, /*ltoPreLink=*/false);
      break;
    case OptStage::PreLinkThinLTO:
      mpm = pb.buildThinLTOPreLinkDefaultPipeline(optLevel);
      break;
    case OptStage::PreLinkFatLTO:
      mpm = pb.buildLTOPreLinkDefaultPipeline(optLevel);
      break;
    case OptStage::ThinLTO:
      mpm = pb.buildThinLTODefaultPipeline(optLevel, nullptr);
      break;
    case OptStage::FatLTO:
      mpm = pb.buildLTODefaultPipeline(optLevel, nullptr);
      break;
    }

    if (config.useThinLTOBuffers) {
      mpm.addPass(llvm::CanonicalizeAliasesPass());
      mpm.addPass(llvm::NameAnonGlobalPass());
    }

    mpm.run(*mod, mam);
  }

  return false;
}
