// Copyright 2024 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "XCLBinGen.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>

#include "AMDAIETargets.h"
#include "aie/Passes.h"
#include "air/Conversion/AIRToAIEPass.h"
#include "iree-amd-aie/Transforms/Passes.h"
#include "iree-dialects/Dialect/LinalgTransform/Passes.h"
#include "iree/compiler/Utils/ToolUtils.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/ToolOutputFile.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVMPass.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Dialect/Vector/Transforms/VectorTransforms.h"
#include "mlir/IR/AsmState.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/Passes.h"

#define DEBUG_TYPE "amdaie-xclbingen"

extern int iree_aie_bootgen_main(int argc, const char *argv[]);

// https://stackoverflow.com/a/60198074
using namespace std::placeholders;
using namespace llvm;
using namespace mlir;
using namespace xilinx;
using Path = std::filesystem::path;

namespace mlir::iree_compiler::AMDAIE {
namespace detail {

// Peano's `opt` program optimizes llvm-ir (.ll files). We run it with a system
// call. This functions constructs the flags to pass to `opt`. There are some
// default flags, most of which are copied from llvm-aie. See
//
// clang-format off
// https://github.com/nod-ai/iree-amd-aie/pull/622
// https://github.com/Xilinx/llvm-aie/blob/0be095354faa49985cd031661853f6d9b9b787f2/clang/lib/Driver/ToolChains/AIE.cpp#L97-L121
// clang-format on
//
// There are also additional flags which have been passed down from the user,
// `additionalPeanoOptFlags`. This function appends these user specific flags,
// and checks that they are valid. If they are not, it returns failure.
FailureOr<std::vector<std::string>> makePeanoOptArgs(
    const std::string &filenameIrIn, const std::string &filenameIrOut,
    const std::string &additionalPeanoOptFlags) {
  std::vector<std::string> args{
      // peano has no proper vectorization cost model for AIE
      "-vectorize-loops=false",
      //
      "-vectorize-slp=false",
      // An if-then-else cascade requires at least 5 delay slots for
      // evaluating the condition and 5 delay slots for one of the
      // branches, thus speculating 10 instructions should be fine
      "--two-entry-phi-node-folding-threshold=10",
      // Make sure to perform most optimizations before mandatory
      // inlinings, otherwise noalias attributes can get lost and
      // hurt AA results.
      "-mandatory-inlining-before-opt=false",
      // complete AA analysis on phi nodes.
      "-basic-aa-full-phi-analysis=true",
      // Extend the max limit of the search depth in BasicAA
      "-basic-aa-max-lookup-search-depth=10",
      //
      "-O3",
      //
      "--inline-threshold=10",
      // missing from libc
      "--disable-builtin=memset",
      // Source file, IR to optimize
      "-S", filenameIrIn,
      // Output file, optimized IR
      "-o", filenameIrOut};

  if (additionalPeanoOptFlags.empty()) return args;

  // Check that additionalPeanoOptFlags is of the form "-flag1 -flag2".
  // i.e. that it starts and ends with ".
  if (additionalPeanoOptFlags.size() < 2 ||
      additionalPeanoOptFlags.front() != '"' ||
      additionalPeanoOptFlags.back() != '"') {
    llvm::errs()
        << "additional peano opt flags must be of the form "
           "\"-flag1 -flag2 ...\". Specifically it must start and end with \".";
    return failure();
  }

  // TODO(newling) use string_view, shouldn't need to copy the string here.
  std::string stripped =
      additionalPeanoOptFlags.substr(1, additionalPeanoOptFlags.size() - 2);

  // Split the additional flags on whitespace, and then add to the default args.
  std::istringstream iss(stripped);
  std::vector<std::string> additionalFlags{
      std::istream_iterator<std::string>{iss},
      std::istream_iterator<std::string>{}};

  // Return true if `flag` is an optimization level flag, like -O2.
  auto isOptLevelFlag = [](const std::string &flag) {
    bool isOptFlag = flag.size() == 3 && flag[0] == '-' && flag[1] == 'O';
    return isOptFlag;
  };

  // Return true if flags `a` and `b` cannot coexist when passed to `opt`.
  auto isContention = [&](const std::string &a, const std::string &b) {
    // If both flags are optimization level flags, they cannot coexist, because
    // llvm-opt will fail to run if it sees two different optimization levels.
    if (isOptLevelFlag(a) && isOptLevelFlag(b)) return true;
    return false;
  };

  // Append the additional flags, unless they conflict with an existing flag,
  // in which case replace the existing flag.
  args.reserve(args.size() + additionalFlags.size());
  for (const auto &flag : additionalFlags) {
    auto iter = std::find_if(args.begin(), args.end(),
                             std::bind(isContention, _1, flag));
    if (iter == args.end()) {
      args.push_back(flag);
    } else {
      *iter = flag;
    }
  }
  return args;
}
}  // namespace detail
}  // namespace mlir::iree_compiler::AMDAIE

namespace {
namespace uuid {
static std::random_device rd;
static std::mt19937 gen(rd());
static std::uniform_int_distribution<> dis(0, 15);
static std::uniform_int_distribution<> dis2(8, 11);

std::string getUUIDString() {
  std::stringstream ss;
  int i;
  ss << std::hex;
  for (i = 0; i < 8; i++) {
    ss << dis(gen);
  }
  ss << "-";
  for (i = 0; i < 4; i++) {
    ss << dis(gen);
  }
  ss << "-4";
  for (i = 0; i < 3; i++) {
    ss << dis(gen);
  }
  ss << "-";
  ss << dis2(gen);
  for (i = 0; i < 3; i++) {
    ss << dis(gen);
  }
  ss << "-";
  for (i = 0; i < 12; i++) {
    ss << dis(gen);
  };
  return ss.str();
}
}  // namespace uuid

// This is a string that contains the wrapped chess intrinsics (see top of the
// included file for deeper explanation).
static const std::string _CHESS_INTRINSIC_WRAPPER_CPP{
#include "chess_intrinsic_wrapper.cpp"
};

// This is a string that contains a mm kernel for npu1.
static const std::string _MM_NPU1_CC{
#include "mm_npu1.cc"
};
// This is a string that contains a mm kernel for npu4.
static const std::string _MM_NPU4_CC{
#include "mm_npu4.cc"
};

FailureOr<std::string> getTargetDir(const std::string &npuVersion) {
  if (npuVersion == "npu1") return std::string{"target_aie_ml"};
  if (npuVersion == "npu4") return std::string{"target_aie2p"};
  llvm::errs() << "unsupported NPUVersion: " << npuVersion;
  return failure();
}

// Apply the pass manager specific options of the XCLBinGenConfig to the pass
// manager. These control when (if ever) and what IR gets printed between
// passes, and whether the pass manager uses multi-theading.
void applyConfigToPassManager(PassManager &pm, bool printIRBeforeAll,
                              bool printIRAfterAll, bool printIRModuleScope,
                              bool timing) {
  auto shouldPrintBeforePass = [printIRBeforeAll](Pass *, Operation *) {
    return printIRBeforeAll;
  };

  auto shouldPrintAfterPass = [printIRAfterAll](Pass *, Operation *) {
    return printIRAfterAll;
  };

  pm.enableIRPrinting(shouldPrintBeforePass, shouldPrintAfterPass,
                      printIRModuleScope);

  if (timing) pm.enableTiming();
}

FailureOr<Path> findVitis(std::optional<Path> &vitisDir,
                          const std::string &npuVersion) {
  if (!vitisDir) {
    const char *envVitis = ::getenv("VITIS");
    if (!envVitis) {
      if (auto vpp = sys::findProgramByName("v++")) {
        SmallString<64> realVpp;
        std::error_code err = sys::fs::real_path(vpp.get(), realVpp);
        if (!err) {
          sys::path::remove_filename(realVpp);
          sys::path::remove_filename(realVpp);
          vitisDir = realVpp.str().str();
          LLVM_DEBUG(dbgs() << "Found Vitis at " << realVpp.c_str() << "\n");
        }
      }
    }
  }
  if (!vitisDir) {
    llvm::errs() << "ERROR: couldn't find vitis directory\n";
    return failure();
  }

  const char *licenseFile = ::getenv("XILINXD_LICENSE_FILE");
  if (!licenseFile) {
    licenseFile = ::getenv("LM_LICENSE_FILE");
    if (!licenseFile) {
      llvm::errs() << "ERROR: either XILINXD_LICENSE_FILE or LM_LICENSE_FILE "
                      "must be set\n";
      return failure();
    }
    if (!std::filesystem::exists(licenseFile)) {
      llvm::errs() << "ERROR: license file" << licenseFile << " does not exist";
      return failure();
    }
  }

  Path aieToolsPath = *vitisDir / "aietools";
  if (!std::filesystem::exists(aieToolsPath)) {
    llvm::errs() << "ERROR: couldn't find aietools directory\n";
    return failure();
  }

  Path chessccPath = aieToolsPath / "tps" / "lnx64" /
                     *getTargetDir(npuVersion) / "bin" / "LNa64bin";

  if (!std::filesystem::exists(chessccPath / "chess-clang")) {
    llvm::errs() << "ERROR: couldn't find chess-clang\n";
    return failure();
  }
  if (!std::filesystem::exists(chessccPath / "chess-llvm-link")) {
    llvm::errs() << "ERROR: couldn't find chess-llvm-link\n";
    return failure();
  }

  return *vitisDir;
}

FailureOr<Path> findAMDAIETool(std::string toolName,
                               const Path &amdAIEInstallDir) {
#if defined(_WIN32)
  toolName += ".exe";
#endif  // _WIN32
  Path toolBinExe;
  if (!amdAIEInstallDir.empty()) {
    toolBinExe = amdAIEInstallDir / toolName;
    if (std::filesystem::exists(toolBinExe)) return toolBinExe;

    toolBinExe = amdAIEInstallDir / "bin" / toolName;
    if (std::filesystem::exists(toolBinExe)) return toolBinExe;

    toolBinExe = amdAIEInstallDir / "tools" / toolName;
    if (std::filesystem::exists(toolBinExe)) return toolBinExe;
  }

  toolBinExe = mlir::iree_compiler::findTool(toolName);
  if (std::filesystem::exists(toolBinExe)) return toolBinExe;

  llvm::errs() << "Could not find " << toolName
               << ". Check your --iree-amd-aie-install-dir flag\n";
  return failure();
}

std::pair<std::string, std::vector<std::string>> makeChessArgs(
    Path &vitisDir, Path &tempDir, const std::string &npuVersion,
    bool verbose) {
  std::string archVersion;
  std::string modelDir;
  if (npuVersion == "npu1") {
    archVersion = "20";
    modelDir = "aie_ml";
  } else if (npuVersion == "npu4") {
    archVersion = "21";
    modelDir = "aie2p";
  } else {
    llvm::errs() << "unsupported NPU version: " << npuVersion;
    llvm::report_fatal_error("unsupported NPU version");
  }

  Path aieToolsDir = vitisDir / "aietools";
  std::vector<std::string> flags{
      // -j <threads> : parallel compilation (function + file level)
      "-j1",
      // -p <name> : processor
      "-pme",
      // -P <dir> : processor model directory
      "-P" + (aieToolsDir / "data" / modelDir / "lib").string(),
      // -f : use LLVM frontend (chess-clang)
      "-f",
      // -C <cfg> : configuration (for chess-clang)
      "-CRelease_LLVM",
      // +w <dir> : work directory
      "+w" + tempDir.string(),
      // for adf headers
      "-D__AIENGINE__",
      // for aie_api headers
      "-D__AIE_ARCH__=" + archVersion, "-D__AIEARCH__=" + archVersion,
      // for aie_api headers
      "-I" + (aieToolsDir / "include").string()};
  // disassemble output
  if (verbose) flags.emplace_back("-d");
  return {(aieToolsDir / "bin" / "unwrapped" / "lnx64.o" / "xchesscc").string(),
          flags};
}

std::vector<std::string> makeChessEnv(Path &vitisDir,
                                      const std::string &npuVersion) {
  Path aieToolsPath = vitisDir / "aietools";
  Path chessccPath = aieToolsPath / "tps" / "lnx64" /
                     *getTargetDir(npuVersion) / "bin" / "LNa64bin";
  Path path(::getenv("PATH"));
  Path lnx64o = aieToolsPath / "lib" / "lnx64.o";
  Path dotLib = aieToolsPath / "lnx64" / "tools" / "dot" / "lib";
  Path ldLibraryPath;
  if (char *ldLibraryPath_ = ::getenv("LD_LIBRARY_PATH")) {
    ldLibraryPath = ldLibraryPath_;
  }
  std::string pathEnv = "PATH=" + chessccPath.string() +
                        std::string{sys::EnvPathSeparator} + path.string();
  std::string ldLibEnv = "LD_LIBRARY_PATH=" + lnx64o.string() +
                         std::string{sys::EnvPathSeparator} + dotLib.string() +
                         std::string{sys::EnvPathSeparator} +
                         ldLibraryPath.string();
  std::string rdiDataEnv = "RDI_DATADIR=" + (aieToolsPath / "data").string();
  const char *licenseFile = ::getenv("XILINXD_LICENSE_FILE");
  if (!licenseFile) licenseFile = ::getenv("LM_LICENSE_FILE");
  std::string licenseFileEnv =
      "XILINXD_LICENSE_FILE=" + std::string(licenseFile);
  return {pathEnv, ldLibEnv, rdiDataEnv, licenseFileEnv};
}

std::optional<std::string> dumpStrToDisk(const std::string &payload,
                                         const std::string &outputPath) {
  std::string errorMessage;
  std::unique_ptr<llvm::ToolOutputFile> outputFile =
      openOutputFile(outputPath, &errorMessage);
  if (!outputFile) return errorMessage;
  outputFile->os() << payload;
  outputFile->keep();
  return {};
}

bool hasEnding(std::string const &fullString, std::string const &ending) {
  if (fullString.length() >= ending.length()) {
    return fullString.compare(fullString.length() - ending.length(),
                              ending.length(), ending) == 0;
  }
  return false;
}

LogicalResult runTool(
    const std::string &program_, const std::vector<std::string> &args,
    bool verbose, std::optional<std::vector<std::string>> env = std::nullopt) {
  std::string program = program_;
#if defined(_WIN32)
  if (!hasEnding(program_, ".exe")) program = program_ + ".exe";
#endif  // _WIN32
  if (verbose) {
    llvm::outs() << "\nRun: ";
    if (env)
      for (auto &s : *env) llvm::outs() << " " << s;
    llvm::outs() << " " << program;
    for (auto &s : args) llvm::outs() << " " << s;
    llvm::outs() << "\n";
  }

  // Check that 'program' is a valid path, if not, fail immediately.
  if (!std::filesystem::exists(program)) {
    llvm::errs() << "Program " << program << " does not exist\n";
    return failure();
  }

  // Run the program, piping any output to a temporary file (we only want to
  // print to terminal if verbose is true).
  SmallVector<StringRef, 8> pArgs = {program};
  pArgs.append(args.begin(), args.end());
  SmallVector<char> temporaryPath;
  {
    std::string prefix{"tmpRunTool"};
    std::string suffix{"Logging"};
    auto errorCode =
        llvm::sys::fs::createTemporaryFile(prefix, suffix, temporaryPath);
    if (errorCode) {
      llvm::errs() << "Failed to create temporary file: " << errorCode.message()
                   << "\n";
      return failure();
    }
  }

  SmallVector<std::optional<StringRef>> redirects;
#ifdef _WIN32
  redirects = {{}, {}, {}};
  // Explicit type but this never actually constructs an ArrayRef
  std::optional<ArrayRef<StringRef>> envSmallVec = std::nullopt;
#else
  std::string temporaryPathStr =
      std::string(temporaryPath.begin(), temporaryPath.size());
  StringRef temporaryPathRef(temporaryPathStr);
  llvm::SmallVector<llvm::StringRef> envSmallVec;
  if (env) envSmallVec.append(env->begin(), env->end());
  auto tp = std::optional<StringRef>(temporaryPathRef);
  redirects = {tp, tp, tp};
#endif

  bool executionFailed;
  std::string errMsg;
  sys::ProcessStatistics stats;
  std::optional<sys::ProcessStatistics> optStats(stats);
  int result = sys::ExecuteAndWait(program, pArgs, envSmallVec,
                                   /* redirects */ redirects,
                                   /*SecondsToWait*/ 0, /*MemoryLimit*/ 0,
                                   &errMsg, &executionFailed, &optStats);

#ifndef _WIN32
  auto maybeOutputFromFile = [&]() -> std::optional<std::string> {
    std::ifstream t(temporaryPathRef.str());
    std::stringstream buffer;
    if (t.is_open() && t.good()) {
      buffer << t.rdbuf();
      return buffer.str();
    }
    return nullptr;
  }();

  if (!maybeOutputFromFile) {
    llvm::errs() << "Failed to open temporary file " << temporaryPathRef.str()
                 << "\n";
  }
  const std::string &outputFromFile = maybeOutputFromFile.value();
#endif

  if (verbose) {
    float totalTime = std::chrono::duration_cast<std::chrono::duration<float>>(
                          stats.TotalTime)
                          .count();
    std::string exitStatusStr = result == 0 ? "Succeeded" : "Failed";
    llvm::outs() << "\n"
                 << exitStatusStr << " in totalTime " << totalTime
                 << " [s]. Exit code=" << result << "\n";
#ifndef _WIN32
    llvm::outs() << outputFromFile << "\n";
#endif
  }

  if (result) {
    llvm::errs() << "Failed to run tool: " << program << ". Error: '" << errMsg
                 << "'\n";
#ifndef _WIN32
    llvm::errs() << outputFromFile;
#endif
    return failure();
  }

  return success();
}

LogicalResult assembleFileUsingChess(const std::string &inputFile,
                                     const std::string &outputFile,
                                     const std::vector<std::string> &extraArgs,
                                     Path &tempDir, Path &vitisDir,
                                     const std::string &npuVersion,
                                     bool verbose) {
  auto [xChessCCExe, args] =
      makeChessArgs(vitisDir, tempDir, npuVersion, verbose);
  args.reserve(args.size() + extraArgs.size());
  args.insert(args.end(), extraArgs.begin(), extraArgs.end());
  args.emplace_back("-c");
  args.emplace_back(inputFile);
  args.emplace_back("-o");
  args.emplace_back(outputFile);
  std::vector<std::string> env = makeChessEnv(vitisDir, npuVersion);
  return runTool(xChessCCExe, args, verbose, env);
}

using FileAssemblerT = std::function<decltype(assembleFileUsingChess)>;

FailureOr<Path> assembleStringUsing(
    const FileAssemblerT &assembler, const std::string &inputFileStr,
    const std::string &inputFileName, const std::string &outputFileName,
    Path &outputDir, const std::vector<std::string> &extraArgs, Path &workDir,
    Path &toolDir, const std::string &npuVersion, bool verbose = false) {
  Path inputFile = workDir / inputFileName;
  if (auto maybeErr = dumpStrToDisk(inputFileStr, inputFile.string());
      maybeErr.has_value()) {
    llvm::errs() << "Failed to dump to disk " << inputFile.string()
                 << " because: " << maybeErr;
    return failure();
  }

  Path outputFile;
  if (!sys::path::is_absolute(outputFileName)) {
    outputFile = Path(outputDir) / outputFileName;
  } else {
    outputFile = outputFileName;
  }
  if (failed(assembler(inputFile.string(), outputFile.string(), extraArgs,
                       workDir, toolDir, npuVersion, verbose))) {
    llvm::errs() << "Failed to assemble " << outputFileName << ".o";
    return failure();
  }
  return outputFile;
}

static auto assembleStringUsingChess =
    std::bind(assembleStringUsing, assembleFileUsingChess, _1, _2, _3, _4, _5,
              _6, _7, _8, _9);

// Generate the elf files for the core
LogicalResult generateCoreElfFiles(AIE::DeviceOp deviceOp,
                                   const std::string &objFile, Path &tempDir,
                                   bool useChess, std::optional<Path> vitisDir,
                                   const std::string &targetArch, bool verbose,
                                   Path peanoDir, const std::string &npuVersion,
                                   const std::optional<std::string> &ukernel) {
  auto tileOps = deviceOp.getOps<AIE::TileOp>();
  std::string errorMessage;

  std::string ukernelFileContent;
  std::string ukernelFileName;
  std::string ukernelObjectName;
  if (npuVersion == "npu1") {
    ukernelFileContent = _MM_NPU1_CC;
    ukernelFileName = "mm_npu1.cc";
    ukernelObjectName = "mm_npu1.o";
  } else if (npuVersion == "npu4") {
    ukernelFileContent = _MM_NPU4_CC;
    ukernelFileName = "mm_npu4.cc";
    ukernelObjectName = "mm_npu4.o";
  } else {
    llvm::errs() << "unsupported NPU version: " << npuVersion;
    return failure();
  }

  for (AIE::TileOp tileOp : tileOps) {
    int col = tileOp.getCol();
    int row = tileOp.getRow();
    auto coreOp = AIE::getCoreOp(tileOp);
    if (!coreOp) continue;

    std::string elfFileName;
    if (auto fileAttr = coreOp.getElfFileAttr()) {
      elfFileName = std::string(fileAttr.getValue());
    } else {
      elfFileName = std::string("core_") + std::to_string(col) + "_" +
                    std::to_string(row) + ".elf";
      coreOp.setElfFile(elfFileName);
    }

    Path elfFile = tempDir / elfFileName;

    Path cwd = std::filesystem::current_path();
    FailureOr<Path> mmObjectFilePath;
    if (ukernel && (ukernel == "mm" || ukernel == "all")) {
      FailureOr<Path> maybeVitisDir = findVitis(vitisDir, npuVersion);
      if (failed(maybeVitisDir)) {
        llvm::errs() << "compiling ukernels currently requires chess (even if "
                        "you're using peano)";
        return failure();
      }
      if (!std::filesystem::exists(cwd / ukernelObjectName)) {
        mmObjectFilePath = assembleStringUsingChess(
            /*inputFileStr=*/ukernelFileContent,
            /*inputFileName=*/ukernelFileName,
            /*outputFileName=*/ukernelObjectName,
            /*outputDir=*/cwd,
            /*extraArgs*/ std::vector<std::string>{},
            /*workDir=*/tempDir,
            /*vitisDir=*/*maybeVitisDir,
            /*npuVersion*/ npuVersion, verbose);
        if (failed(mmObjectFilePath)) return failure();
      } else {
        mmObjectFilePath = cwd / ukernelObjectName;
      }
    }

    if (useChess) {
      FailureOr<Path> maybeVitisDir = findVitis(vitisDir, npuVersion);
      if (failed(maybeVitisDir)) return failure();
      FailureOr<Path> chessIntrinsicsObjFile;
      if (!std::filesystem::exists(cwd / "chess_intrinsic_wrapper.o")) {
        chessIntrinsicsObjFile = assembleStringUsingChess(
            /*inputFileStr=*/_CHESS_INTRINSIC_WRAPPER_CPP,
            /*inputFileName=*/"chess_intrinsic_wrapper.cpp",
            /*outputFileName=*/"chess_intrinsic_wrapper.o",
            /*outputDir=*/tempDir,
            /*extraArgs*/ std::vector<std::string>{},
            /*workDir=*/tempDir,
            /*vitisDir=*/*maybeVitisDir,
            /*npuVersion*/ npuVersion, verbose);
        if (failed(chessIntrinsicsObjFile)) return failure();
      } else {
        chessIntrinsicsObjFile = cwd / "chess_intrinsic_wrapper.o";
      }

      // Use xbridge (to remove any peano dependency with use-chess option)
      Path bcfPath = tempDir / (elfFileName + ".bcf");

      {
        auto bcfOutput = openOutputFile(bcfPath.string(), &errorMessage);
        if (!bcfOutput) {
          llvm::errs() << "failed to open bcf file because: " << errorMessage;
          return failure();
        }

        if (failed(mlir::iree_compiler::AMDAIE::AIETranslateToBCF(
                deviceOp, bcfOutput->os(), col, row))) {
          llvm::errs() << "Failed to generate BCF";
          return failure();
        }
        bcfOutput->keep();
      }

      auto [xChessCCExe, chessArgs] =
          makeChessArgs(*vitisDir, tempDir, npuVersion, verbose);
      chessArgs.emplace_back(objFile);
      chessArgs.emplace_back(chessIntrinsicsObjFile->string());
      if (ukernel && (ukernel == "mm" || ukernel == "all")) {
        chessArgs.emplace_back(mmObjectFilePath->string());
      }
      chessArgs.emplace_back("+l");
      chessArgs.emplace_back(bcfPath.string());
      chessArgs.emplace_back("-o");
      chessArgs.emplace_back(elfFile.string());
      std::vector<std::string> env = makeChessEnv(*vitisDir, npuVersion);
      if (failed(runTool(xChessCCExe, chessArgs, verbose, env))) {
        return deviceOp.emitOpError() << "failed to generate elf for core: ("
                                      << col << ", " << row << ")";
      }
    } else {
      Path ldscriptPath = tempDir / (elfFileName + ".ld");
      {
        auto ldscriptOutput =
            openOutputFile(ldscriptPath.string(), &errorMessage);
        if (!ldscriptOutput) {
          llvm::errs() << "Failed to open ldscript file because: "
                       << errorMessage;
          return failure();
        }
        if (failed(mlir::iree_compiler::AMDAIE::AIETranslateToLdScript(
                deviceOp, ldscriptOutput->os(), col, row))) {
          return failure();
        }
        ldscriptOutput->keep();
      }

      std::string targetLower = StringRef(targetArch).lower();
      std::vector<std::string> flags;
      flags.emplace_back(objFile);
      if (ukernel && (ukernel == "mm" || ukernel == "all")) {
        flags.emplace_back(mmObjectFilePath->string());
      }
      flags.emplace_back("--target=" + targetLower + "-none-unknown-elf");
      flags.emplace_back("-Wl,--gc-sections");
      flags.emplace_back("-Wl,--orphan-handling=error");
      flags.emplace_back("-Wl,-T," + ldscriptPath.string());
      flags.emplace_back("-o");
      flags.emplace_back(elfFile.string());
      if (verbose) flags.emplace_back("-v");
      // we run clang (ie cc) so that libc, libm, crt0/1 paths are injected
      // automatically into the ld.lld invocation
      if (failed(
              runTool((peanoDir / "bin" / "clang").string(), flags, verbose))) {
        return failure();
      }
    }
  }
  return success();
}

LogicalResult generateCDO(MLIRContext *context, AIE::DeviceOp deviceOp,
                          const Path &tempDir) {
  auto copy = cast<ModuleOp>(deviceOp.getParentOp()->clone());
  deviceOp = *copy.getOps<AIE::DeviceOp>().begin();
  if (failed(mlir::iree_compiler::AMDAIE::AIETranslateToCDODirect(
          deviceOp, tempDir.string()))) {
    llvm::errs() << "failed to emit CDO";
    return failure();
  }
  copy->erase();
  return success();
}

json::Object makeKernelJSON(const std::string &name, const std::string &id,
                            const std::string &instance) {
  return json::Object{
      {"name", name},
      {"type", "dpu"},
      {"extended-data",
       json::Object{
           {"subtype", "DPU"}, {"functional", "0"}, {"dpu_kernel_id", id}}},
      {"arguments", json::Array{json::Object{{"name", "opcode"},
                                             {"address-qualifier", "SCALAR"},
                                             {"type", "uint64_t"},
                                             {"offset", "0x00"}},
                                json::Object{{"name", "instr"},
                                             {"memory-connection", "SRAM"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "char *"},
                                             {"offset", "0x08"}},
                                json::Object{{"name", "ninstr"},
                                             {"address-qualifier", "SCALAR"},
                                             {"type", "uint32_t"},
                                             {"offset", "0x10"}},
                                json::Object{{"name", "bo0"},
                                             {"memory-connection", "HOST"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "void*"},
                                             {"offset", "0x14"}},
                                json::Object{{"name", "bo1"},
                                             {"memory-connection", "HOST"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "void*"},
                                             {"offset", "0x1c"}},
                                json::Object{{"name", "bo2"},
                                             {"memory-connection", "HOST"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "void*"},
                                             {"offset", "0x24"}},
                                json::Object{{"name", "bo3"},
                                             {"memory-connection", "HOST"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "void*"},
                                             {"offset", "0x2c"}},
                                json::Object{{"name", "bo4"},
                                             {"memory-connection", "HOST"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "void*"},
                                             {"offset", "0x34"}},
                                json::Object{{"name", "bo5"},
                                             {"memory-connection", "HOST"},
                                             {"address-qualifier", "GLOBAL"},
                                             {"type", "void*"},
                                             {"offset", "0x3c"}}}},
      {"instances", json::Array{json::Object{{"name", instance}}}}};
}

LogicalResult generatePDI(const std::string &Output, const Path &tempDir) {
  std::string errorMessage;
  // Create design.bif.
  Path designBifFile = tempDir / "design.bif";
  {
    auto designBifOut = openOutputFile(designBifFile.string(), &errorMessage);
    if (!designBifOut) {
      llvm::errs() << "failed to open design.bif because: " << errorMessage;
      return failure();
    }

    designBifOut->os() << "all:\n"
                       << "{\n"
                       << "  id_code = 0x14ca8093\n"
                       << "  extended_id_code = 0x01\n"
                       << "  image\n"
                       << "  {\n"
                       << "    name=aie_image, id=0x1c000000\n"
                       << "    { type=cdo\n"
                       << "      file=" << tempDir.string()
                       << "/aie_cdo_elfs.bin\n"
                       << "      file=" << tempDir.string()
                       << "/aie_cdo_init.bin\n"
                       << "      file=" << tempDir.string()
                       << "/aie_cdo_enable.bin\n"
                       << "    }\n"
                       << "  }\n"
                       << "}";
    designBifOut->keep();
  }

  // Execute the bootgen command.
  {
    // first element is empty string because iree_aie_bootgen_main
    // is the main of bootgen.exe (and argv[0] is typically the name of the exe)
    std::vector<std::string> flags = {
        "",   "-arch", "versal", "-image", designBifFile.string(),
        "-o", Output,  "-w"};
    std::vector<char *> cstrings;
    cstrings.reserve(flags.size());
    for (const auto &inputFlag : flags) {
      cstrings.push_back(const_cast<char *>(inputFlag.c_str()));
    }
    if (iree_aie_bootgen_main(cstrings.size(),
                              const_cast<const char **>(&cstrings[0]))) {
      llvm::errs() << "failed to execute bootgen";
      return failure();
    }
  }

  return success();
}

LogicalResult generateXCLBin(const std::string &Output, const Path &tempDir,
                             const std::string &xclBinKernelID,
                             const std::string &xclBinKernelName,
                             const std::string &xclBinInstanceName,
                             const Path &amdAIEInstallDir, bool verbose,
                             const std::optional<std::string> &inputXclbin) {
  std::string errorMessage;
  // Create mem_topology.json.
  Path memTopologyJsonFile = tempDir / "mem_topology.json";
  {
    std::string memTopologyData = R"({
      "mem_topology": {
          "m_count": "2",
          "m_mem_data": [
              {
                  "m_type": "MEM_DRAM",
                  "m_used": "1",
                  "m_sizeKB": "0x10000",
                  "m_tag": "HOST",
                  "m_base_address": "0x4000000"
              },
              {
                  "m_type": "MEM_DRAM",
                  "m_used": "1",
                  "m_sizeKB": "0xc000",
                  "m_tag": "SRAM",
                  "m_base_address": "0x4000000"
              }
          ]
      }
    })";
    if (auto maybeErr =
            dumpStrToDisk(memTopologyData, memTopologyJsonFile.string());
        maybeErr.has_value()) {
      llvm::errs() << "failed to dump to disk mem_topology.json because: "
                   << *maybeErr;
      return failure();
    }
  }

  // Create aie_partition.json.
  Path aiePartitionJsonFile = tempDir / "aie_partition.json";
  {
    std::string uuidStr = uuid::getUUIDString();
    std::string aiePartitionJsonData = R"(
      {
        "aie_partition": {
          "name": "QoS",
          "operations_per_cycle": "2048",
          "inference_fingerprint": "23423",
          "pre_post_fingerprint": "12345",
          "partition": {
            "column_width": 4,
            "start_columns": [1]
          },
          "PDIs": [
            {
              "uuid": ")" + uuidStr + R"(",
              "file_name": "./design.pdi",
              "cdo_groups": [
                {
                  "name": "DPU",
                  "type": "PRIMARY",
                  "pdi_id": "0x01",
                  "dpu_kernel_ids": [
                    ")" + xclBinKernelID +
                                       R"("
                  ],
                  "pre_cdo_groups": [
                    "0xC1"
                  ]
                }
              ]
            }
          ]
        }
      }
    )";
    if (auto maybeErr =
            dumpStrToDisk(aiePartitionJsonData, aiePartitionJsonFile.string());
        maybeErr.has_value()) {
      llvm::errs() << "failed to dump to disk aie_partition.json because: "
                   << *maybeErr;
      return failure();
    }
  }

  Path kernelsJsonFile = tempDir / "kernels.json";
  {
    // TODO: Support for multiple kernels
    json::Object kernelsData{
        {"ps-kernels",
         json::Object{{"kernels", json::Array{makeKernelJSON(
                                      xclBinKernelName, xclBinKernelID,
                                      xclBinInstanceName)}}}}};

    auto kernelStr =
        llvm::formatv("{0:2}", json::Value(std::move(kernelsData)));
    if (auto maybeErr = dumpStrToDisk(kernelStr, kernelsJsonFile.string());
        maybeErr.has_value()) {
      llvm::errs() << "failed to dump to disk kernels.json because: "
                   << *maybeErr;
      return failure();
    }
  }

  if (failed(generatePDI((tempDir / "design.pdi").string(), tempDir))) {
    return failure();
  }

  std::vector<std::string> flags;
  // Execute the xclbinutil command.
  std::string memArg = "MEM_TOPOLOGY:JSON:" + memTopologyJsonFile.string();
  std::string partArg = "AIE_PARTITION:JSON:" + aiePartitionJsonFile.string();
  FailureOr<Path> xclbinutilBin =
      findAMDAIETool("iree-aie-xclbinutil", amdAIEInstallDir);

  if (failed(xclbinutilBin)) return failure();

  if (!inputXclbin) {
    flags.insert(flags.end(), {"--add-replace-section", memArg});
  } else {
    // Create aie_partition.json.
    Path aieInputPartitionJsonFile = tempDir / "aie_input_partition.json";
    std::string inputPartArg =
        "AIE_PARTITION:JSON:" + aieInputPartitionJsonFile.string();
    std::vector<std::string> inputFlags{"--dump-section", inputPartArg,
                                        "--force", "--input", *inputXclbin};

    if (failed(runTool(xclbinutilBin.value().string(), inputFlags, verbose))) {
      llvm::errs() << "failed to execute xclbinutil";
      return failure();
    }
    auto aieInputPartitionOut =
        openInputFile(aieInputPartitionJsonFile.string(), &errorMessage);
    if (!aieInputPartitionOut) {
      llvm::errs() << "failed to open aie_input_partition.json because: "
                   << errorMessage;
      return failure();
    }
    Expected<json::Value> aieInputPartitionOutValue =
        llvm::json::parse(aieInputPartitionOut->getBuffer());
    json::Array *aieInputPartionPDIs;
    aieInputPartionPDIs = aieInputPartitionOutValue->getAsObject()
                              ->getObject("aie_partition")
                              ->getArray("PDIs");
    auto aiePartitionOut =
        openInputFile(aiePartitionJsonFile.string(), &errorMessage);
    if (!aiePartitionOut) {
      llvm::errs() << "failed to open aie aie_input_partition.json for "
                      "output because: "
                   << errorMessage;
      return failure();
    }
    llvm::Expected<llvm::json::Value> aiePartitionOutValue =
        llvm::json::parse(aiePartitionOut->getBuffer());
    json::Array *aiePartionPDIs;
    aiePartionPDIs = aiePartitionOutValue->getAsObject()
                         ->getObject("aie_partition")
                         ->getArray("PDIs");
    aieInputPartionPDIs->insert(aieInputPartionPDIs->end(),
                                aiePartionPDIs->begin(), aiePartionPDIs->end());
    // rewrite aie partion json file
    if (auto maybeErr =
            dumpStrToDisk(formatv("{0:2}", *aieInputPartitionOutValue),
                          aiePartitionJsonFile.string());
        maybeErr.has_value()) {
      llvm::errs()
          << "failed to dump to disk aie_input_partition.json because: "
          << errorMessage;
      return failure();
    }
    flags.insert(flags.end(), {"--input", *inputXclbin});
  }
  flags.insert(flags.end(), {"--add-kernel", kernelsJsonFile.string(),
                             "--add-replace-section", partArg, "--force",
                             "--output", std::string(Output)});

  return runTool(xclbinutilBin.value().string(), flags, verbose);
}

void addLowerToLLVMPasses(OpPassManager &pm) {
  pm.addPass(createFinalizeMemRefToLLVMConversionPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  ConvertFuncToLLVMPassOptions opts;
  opts.useBarePtrCallConv = true;
  pm.addPass(createConvertFuncToLLVMPass(opts));
  pm.addPass(createArithToLLVMConversionPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  pm.addPass(createConvertControlFlowToLLVMPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
}

LogicalResult generateUnifiedObject(
    MLIRContext *context, AIE::DeviceOp deviceOp, const std::string &outputFile,
    bool printIRBeforeAll, bool printIRAfterAll, bool printIRModuleScope,
    bool timing, bool useChess, bool verbose, Path &tempDir,
    std::optional<Path> vitisDir, const std::string &targetArch, Path &peanoDir,
    const std::string &npuVersion, const std::string &additionalPeanoOptFlags) {
  assert(deviceOp->getParentOp() && isa<ModuleOp>(deviceOp->getParentOp()) &&
         "DeviceOp must be in a module parent");

  PassManager pm(context, ModuleOp::getOperationName());
  applyConfigToPassManager(pm, printIRBeforeAll, printIRAfterAll,
                           printIRModuleScope, timing);

  mlir::iree_compiler::AMDAIE::AMDAIECoreToStandardOptions options;
  options.lowerToChess = useChess;
  pm.addPass(
      mlir::iree_compiler::AMDAIE::createAMDAIECoreToStandardPass(options));
  addLowerToLLVMPasses(pm);

  if (verbose) {
    llvm::outs() << "\nRunning: ";
    pm.printAsTextualPipeline(llvm::outs());
    llvm::outs() << "\n";
  }

  ModuleOp moduleOpCopy = cast<ModuleOp>(deviceOp->getParentOp()).clone();
  if (failed(pm.run(moduleOpCopy))) {
    llvm::errs() << "Failed to lower to LLVM";
    return failure();
  }

  llvm::LLVMContext llvmContext;
  std::unique_ptr<llvm::Module> llvmModule =
      translateModuleToLLVMIR(moduleOpCopy, llvmContext);
  if (!llvmModule) {
    llvm::errs() << "Failed to translate module to LLVMIR";
    return failure();
  }

  std::string inputLLStr;
  {
    llvm::raw_string_ostream rso(inputLLStr);
    llvmModule->print(rso, nullptr);
  }

  std::string errorMessage;
  if (useChess) {
    FailureOr<Path> maybeVitisDir = findVitis(vitisDir, npuVersion);
    if (failed(maybeVitisDir)) return failure();
    FailureOr<Path> chessIntrinsicsObjFile = assembleStringUsingChess(
        /*inputFileStr=*/inputLLStr,
        /*inputFileName=*/"input.ll",
        /*outputFileName=*/outputFile,
        /*outputDir=*/tempDir,
        /*extraArgs*/ std::vector<std::string>{},
        /*workDir=*/tempDir,
        /*vitisDir=*/*maybeVitisDir,
        /*npuVersion*/ npuVersion,
        /*verbose=*/verbose);
    if (failed(chessIntrinsicsObjFile)) {
      return failure();
    }
  } else {
    std::string LLVMIRFile = (tempDir / "input.ll").string();
    if (auto maybeErr = dumpStrToDisk(inputLLStr, LLVMIRFile);
        maybeErr.has_value()) {
      llvm::errs() << "Failed to dump to disk input.ll"
                   << " because: " << maybeErr;
      return failure();
    }
    Path peanoOptBin = peanoDir / "bin" / "opt";
    Path peanoLLCBin = peanoDir / "bin" / "llc";

    std::string OptLLVMIRFile = (tempDir / "input.opt.ll").string();

    FailureOr<std::vector<std::string>> peanoArgs =
        mlir::iree_compiler::AMDAIE::detail::makePeanoOptArgs(
            LLVMIRFile, OptLLVMIRFile, additionalPeanoOptFlags);
    if (failed(peanoArgs)) {
      llvm::errs() << "Failed to make peano opt args";
      return failure();
    }

    if (failed(runTool(peanoOptBin.string(), peanoArgs.value(), verbose))) {
      llvm::errs() << "Failed to optimize ll with peano";
      return failure();
    }

    if (failed(runTool(
            peanoLLCBin.string(),
            {OptLLVMIRFile, "-O2", "--march=" + StringRef(targetArch).lower(),
             "--function-sections", "--filetype=obj", "-o",
             std::string(outputFile)},
            verbose))) {
      llvm::errs() << "Failed to assemble ll with peano\n";
      return failure();
    }
  }

  moduleOpCopy->erase();
  return success();
}

/// Assume the ELF files have already been generated and are stored in the
/// `tempDirPath`. This function converts `xilinx::aie::device` to
/// `amdaie.npu.control_packets` by calling the
/// `AMDAIEConvertDeviceToControlPacketsPass`.
LogicalResult generateControlPackets(MLIRContext *context,
                                     AIE::DeviceOp deviceOp,
                                     const Path &tempDirPath,
                                     bool printIRBeforeAll,
                                     bool printIRAfterAll,
                                     bool printIRModuleScope, bool timing) {
  assert(deviceOp->getParentOp() && isa<ModuleOp>(deviceOp->getParentOp()) &&
         "DeviceOp must be in a module parent");
  PassManager pm(context, ModuleOp::getOperationName());
  applyConfigToPassManager(pm, printIRBeforeAll, printIRAfterAll,
                           printIRModuleScope, timing);
  mlir::iree_compiler::AMDAIE::AMDAIEConvertDeviceToControlPacketsOptions
      options;
  options.pathToElfs = tempDirPath.string();
  pm.addPass(mlir::iree_compiler::AMDAIE::
                 createAMDAIEConvertDeviceToControlPacketsPass(options));
  pm.addPass(
      mlir::iree_compiler::AMDAIE::createAMDAIESplitControlPacketDataPass());
  return pm.run(deviceOp->getParentOp());
}

}  // namespace

namespace mlir::iree_compiler::AMDAIE {
LogicalResult emitNpuInstructions(AIE::DeviceOp deviceOp,
                                  const std::string &outputNPU) {
  MLIRContext *ctx = deviceOp.getContext();
  mlir::Attribute maybeNpuInstructions = deviceOp->getAttr("npu_instructions");
  if (!maybeNpuInstructions) {
    return emitError(UnknownLoc::get(ctx),
                     "Expected npu_instructions attribute on aie.device");
  }

  DenseUI32ResourceElementsAttr npuInstructions =
      dyn_cast<DenseUI32ResourceElementsAttr>(maybeNpuInstructions);
  if (!npuInstructions) {
    return emitError(
        UnknownLoc::get(ctx),
        "Failed to cast npu_instructions to DenseUI32ResourceElementsAttr");
  }

  std::optional<ArrayRef<uint32_t>> maybeArrayRef =
      npuInstructions.tryGetAsArrayRef();
  assert(maybeArrayRef &&
         "Failed getting values for npu_instructions in tryGetAsArrayRef");
  std::string errorMessage;
  std::unique_ptr<llvm::ToolOutputFile> output =
      openOutputFile(outputNPU, &errorMessage);
  if (!output) {
    llvm::errs() << "Failed to open npu_instructions.txt for writing because: "
                 << errorMessage << "\n";
    return failure();
  }
  output->keep();

  for (int i = 0; i < maybeArrayRef->size() - 1; ++i) {
    output->os() << llvm::format("%08X\n", maybeArrayRef->operator[](i));
  }
  // don't emit empty line at the end
  output->os() << llvm::format("%08X", maybeArrayRef->back());

  return success();
}

LogicalResult aie2xclbin(
    MLIRContext *ctx, AIE::DeviceOp deviceOp,
    const std::optional<std::string> &outputNPU, bool emitCtrlPkt,
    const std::string &artifactPath, bool printIRBeforeAll,
    bool printIRAfterAll, bool printIRModuleScope, bool timing,
    const std::string &tempDir, bool useChess, bool verbose,
    const std::optional<std::string> &vitisDir, const std::string &targetArch,
    const std::string &npuVersion, const std::string &peanoDir,
    const mlir::iree_compiler::AMDAIE::AMDAIEOptions::DeviceHAL deviceHal,
    const std::string &xclBinKernelID, const std::string &xclBinKernelName,
    const std::string &xclBinInstanceName, const std::string &amdAIEInstallDir,
    const std::optional<std::string> &InputXCLBin,
    const std::optional<std::string> &ukernel,
    const std::string &additionalPeanoOptFlags) {
  if (outputNPU.has_value() &&
      failed(emitNpuInstructions(deviceOp, outputNPU.value()))) {
    return failure();
  }

  Path tempDirPath{tempDir};
  tempDirPath.make_preferred();
  Path peanoDirPath{peanoDir};
  peanoDirPath.make_preferred();
  std::optional<Path> vitisDirPath{vitisDir};
  if (vitisDirPath) vitisDirPath->make_preferred();

  Path unifiedObj = tempDirPath / "input.o";
  if (failed(generateUnifiedObject(
          ctx, deviceOp, unifiedObj.string(), printIRBeforeAll, printIRAfterAll,
          printIRModuleScope, timing, useChess, verbose, tempDirPath,
          vitisDirPath, targetArch, peanoDirPath, npuVersion,
          additionalPeanoOptFlags))) {
    llvm::errs() << "Failed to generate unified object\n";
    return failure();
  }

  if (failed(generateCoreElfFiles(deviceOp, unifiedObj.string(), tempDirPath,
                                  useChess, vitisDirPath, targetArch, verbose,
                                  peanoDir, npuVersion, ukernel))) {
    llvm::errs() << "Failed to generate core ELF file(s)\n";
    return failure();
  }

  if (emitCtrlPkt && failed(generateControlPackets(
                         ctx, deviceOp, tempDirPath, printIRBeforeAll,
                         printIRAfterAll, printIRModuleScope, timing))) {
    llvm::errs() << "Failed to generate control packets MLIR file\n";
    return failure();
  }

  if (failed(generateCDO(ctx, deviceOp, tempDirPath))) {
    llvm::errs() << "Failed to generate CDO\n";
    return failure();
  }

  Path pdiPath = tempDirPath / "design.pdi";
  if (failed(generatePDI(pdiPath.string(), tempDirPath))) {
    llvm::errs() << "Failed to generate PDI\n";
    return failure();
  }

  if (deviceHal == AMDAIEOptions::DeviceHAL::XRT_LITE) {
    std::error_code ec;
    if (!std::filesystem::copy_file(
            pdiPath, artifactPath,
            std::filesystem::copy_options::overwrite_existing, ec)) {
      llvm::errs() << "Failed to copy file because: " << ec.message() << "\n";
      return failure();
    }
    return success();
  }

  assert(deviceHal == AMDAIEOptions::DeviceHAL::XRT &&
         "generating XCLBin for non-XRT HAL");
  if (failed(generateXCLBin(artifactPath, tempDirPath, xclBinKernelID,
                            xclBinKernelName, xclBinInstanceName,
                            amdAIEInstallDir, verbose, InputXCLBin))) {
    llvm::errs() << "Failed to generate XCLBin\n";
    return failure();
  }

  return success();
}

}  // namespace mlir::iree_compiler::AMDAIE
