//===--- Driver.h - Swift compiler driver -----------------------*- compilation++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// This source file is part of the polarphp.org open source project
//
// Copyright (c) 2017 - 2019 polarphp software foundation
// Copyright (c) 2017 - 2019 zzu_softboy <zzu_softboy@163.com>
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://polarphp.org/LICENSE.txt for license information
// See https://polarphp.org/CONTRIBUTORS.txt for the list of polarphp project authors
//
// Created by polarboy on 2019/11/26.

#ifndef POLARPHP_DRIVER_DRIVER_H
#define POLARPHP_DRIVER_DRIVER_H

#include "polarphp/ast/IRGenOptions.h"
#include "polarphp/basic/FileTypes.h"
#include "polarphp/basic/LLVM.h"
#include "polarphp/basic/OptionSet.h"
#include "polarphp/basic/OutputFileMap.h"
#include "polarphp/basic/Sanitizers.h"
#include "polarphp/driver/Utils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <memory>
#include <string>

namespace llvm::opt {
class Arg;
class argList;
class OptTable;
class InputArgList;
class DerivedArgList;
} // llvm::opt

namespace polar::ast {
class DiagnosticEngine;
} // polar::ast

namespace polar::sys {
class TaskQueue;
} // polar::sys

namespace polar::driver {

using polar::ast::DiagnosticEngine;
using llvm::opt::InputArgList;
using llvm::opt::DerivedArgList;
using polar::sys::TaskQueue;
using polar::basic::OptionSet;
using polar::basic::SanitizerKind;
using polar::basic::TypeToPathMap;
using polar::basic::OutputFileMap;
using polar::ast::IRGenDebugInfoLevel;
using polar::ast::IRGenDebugInfoFormat;

class Action;
class CommandOutput;
class Compilation;
class Job;
class JobAction;
class ToolChain;

/// A class encapsulating information about the outputs the driver
/// is expected to generate.
class OutputInfo
{
public:
   enum class Mode
   {
      /// A standard compilation, using multiple frontend invocations and
      /// -primary-file.
      StandardCompile,

      /// A compilation using a single frontend invocation without -primary-file.
      SingleCompile,

      /// A single process that batches together multiple StandardCompile Jobs.
      ///
      /// Note: this is a transient value to use _only_ for the individual
      /// BatchJobs that are the temporary containers for multiple StandardCompile
      /// Jobs built by ToolChain::constructBatchJob.
      ///
      /// In particular, the driver treats a batch-mode-enabled Compilation as
      /// having OutputInfo::compilerMode == StandardCompile, with the
      /// Compilation::BatchModeEnabled flag set to true, _not_ as a
      /// BatchModeCompile Compilation. The top-level OutputInfo::compilerMode for
      /// a Compilation should never be BatchModeCompile.
      BatchModeCompile,

      /// Invoke the REPL
      REPL,

      /// Compile and execute the inputs immediately
      Immediate,
   };

   /// The mode in which the driver should invoke the frontend.
   Mode compilerMode = Mode::StandardCompile;

   /// The output type which should be used for compile actions.
   filetypes::FileTypeId compilerOutputType = filetypes::FileTypeId::TY_INVALID;

   /// Describes if and how the output of compile actions should be
   /// linked together.
   LinkKind linkAction = LinkKind::None;

   /// Returns true if the linker will be invoked at all.
   bool shouldLink() const
   {
      return linkAction != LinkKind::None;
   }

   /// Whether or not the output should contain debug info.
   // FIXME: Eventually this should be replaced by dSYM generation.
   IRGenDebugInfoLevel debugInfoLevel = IRGenDebugInfoLevel::None;

   /// What kind of debug info to generate.
   IRGenDebugInfoFormat debugInfoFormat = IRGenDebugInfoFormat::None;

   /// Whether or not the driver should generate a module.
   bool shouldGenerateModule = false;

   /// Whether or not the driver should treat a generated module as a top-level
   /// output.
   bool shouldTreatModuleAsTopLevelOutput = false;

   /// Whether the compiler picked the current module name, rather than the user.
   bool moduleNameIsFallback = false;

   /// The number of threads for multi-threaded compilation.
   unsigned numThreads = 0;

   /// Returns true if multi-threading is enabled.
   bool isMultiThreading() const
   {
      return numThreads > 0;
   }

   /// The name of the module which we are building.
   std::string moduleName;

   /// The path to the SDK against which to build.
   /// (If empty, this implies no SDK.)
   std::string sdkPath;

   OptionSet<SanitizerKind> selectedSanitizers;

   /// Might this sort of compile have explicit primary inputs?
   /// When running a single compile for the whole module (in other words
   /// "whole-module-optimization" mode) there must be no -primary-input's and
   /// nothing in a (preferably non-existent) -primary-filelist. Left to its own
   /// devices, the driver would forget to omit the primary input files, so
   /// return a flag here.
   bool mightHaveExplicitPrimaryInputs(const CommandOutput &output) const;
};

class Driver
{
public:
   /// DriverKind determines how later arguments are parsed, as well as the
   /// allowable OutputInfo::Mode values.
   enum class DriverKind
   {
      Interactive,     // polarphp
      Batch,           // polarphpc
      AutolinkExtract, // polarphp-autolink-extract
      SwiftFormat      // polarphp-format
   };

   class InputInfoMap;

public:
   Driver(StringRef m_driverExecutable, StringRef m_name,
          ArrayRef<const char *> args, DiagnosticEngine &diags);
   ~Driver();

   const llvm::opt::OptTable &getOpts() const
   {
      return *m_opts;
   }

   const DiagnosticEngine &getDiags() const
   {
      return m_diags;
   }

   const std::string &getPolarphpProgramPath() const
   {
      return m_driverExecutable;
   }

   ArrayRef<std::string> getPolarphpProgramArgs() const
   {
      return m_driverExecutableArgs;
   }

   DriverKind getDriverKind() const
   {
      return m_driverKind;
   }

   ArrayRef<const char *> getArgsWithoutProgramNameAndDriverMode(
         ArrayRef<const char *> args) const;

   bool getCheckInputFilesExist() const
   {
      return m_checkInputFilesExist;
   }

   void setCheckInputFilesExist(bool value)
   {
      m_checkInputFilesExist = value;
   }

   /// Creates an appropriate ToolChain for a given driver, given the target
   /// specified in \p args (or the default target). Sets the value of \c
   /// m_defaultTargetTriple from \p args as a side effect.
   ///
   /// \return A ToolChain, or nullptr if an unsupported target was specified
   /// (in which case a diagnostic error is also signalled).
   ///
   /// This uses a std::unique_ptr instead of returning a toolchain by value
   /// because ToolChain has virtual methods.
   std::unique_ptr<ToolChain>
   buildToolChain(const llvm::opt::InputArgList &argList);

   /// Compute the task queue for this compilation and command line argument
   /// vector.
   ///
   /// \return A TaskQueue, or nullptr if an invalid number of parallel jobs is
   /// specified.  This condition is signalled by a diagnostic.
   std::unique_ptr<sys::TaskQueue> buildTaskQueue(const Compilation &compilation);

   /// Construct a compilation object for a given ToolChain and command line
   /// argument vector.
   ///
   /// \return A Compilation, or nullptr if none was built for the given argument
   /// vector. A null return value does not necessarily indicate an error
   /// condition; the diagnostics should be queried to determine if an error
   /// occurred.
   std::unique_ptr<Compilation>
   buildCompilation(const ToolChain &toolchain,
                    std::unique_ptr<llvm::opt::InputArgList> argList);

   /// Parse the given list of strings into an InputArgList.
   std::unique_ptr<llvm::opt::InputArgList>
   parseArgStrings(ArrayRef<const char *> args);

   /// Resolve path arguments if \p workingDirectory is non-empty, and translate
   /// inputs from -- arguments into a DerivedArgList.
   llvm::opt::DerivedArgList *
   translateInputAndPathArgs(const llvm::opt::InputArgList &argList,
                             StringRef workingDirectory) const;

   /// Construct the list of inputs and their types from the given arguments.
   ///
   /// \param toolchain The current tool chain.
   /// \param args The input arguments.
   /// \param[out] inputs The list in which to store the resulting compilation
   /// inputs.
   void buildInputs(const ToolChain &toolchain, const llvm::opt::DerivedArgList &args,
                    InputFileList &inputs) const;

   /// Construct the OutputInfo for the driver from the given arguments.
   ///
   /// \param toolchain The current tool chain.
   /// \param args The input arguments.
   /// \param batchMode Whether the driver has been explicitly or implicitly
   /// instructed to use batch mode.
   /// \param inputs The inputs to the driver.
   /// \param[out] outputInfo The OutputInfo in which to store the resulting output
   /// information.
   void buildOutputInfo(const ToolChain &toolchain,
                        const llvm::opt::DerivedArgList &args,
                        const bool batchMode, const InputFileList &inputs,
                        OutputInfo &outputInfo) const;

   /// Construct the list of Actions to perform for the given arguments,
   /// which are only done for a single architecture.
   ///
   /// \param[out] topLevelActions The main Actions to build Jobs for.
   /// \param toolchain the default host tool chain.
   /// \param outputInfo The OutputInfo for which Actions should be generated.
   /// \param outOfDateMap If present, information used to decide which files
   /// need to be rebuilt.
   /// \param compilation The Compilation to which Actions should be added.
   void buildActions(SmallVectorImpl<const Action *> &topLevelActions,
                     const ToolChain &toolchain, const OutputInfo &outputInfo,
                     const InputInfoMap *outOfDateMap,
                     Compilation &compilation) const;

   /// Construct the OutputFileMap for the driver from the given arguments.
   Optional<OutputFileMap>
   buildOutputFileMap(const llvm::opt::DerivedArgList &args,
                      StringRef workingDirectory) const;

   /// Add top-level Jobs to Compilation \p compilation for the given \p Actions and
   /// OutputInfo.
   ///
   /// \param topLevelActions The main Actions to build Jobs for.
   /// \param outputInfo The OutputInfo for which Jobs should be generated.
   /// \param OFM The OutputFileMap for which Jobs should be generated.
   /// \param workingDirectory If non-empty, used to resolve any generated paths.
   /// \param toolchain The ToolChain to build Jobs with.
   /// \param compilation The Compilation containing the Actions for which Jobs should be
   /// created.
   void buildJobs(ArrayRef<const Action *> topLevelActions, const OutputInfo &outputInfo,
                  const OutputFileMap *OFM, StringRef workingDirectory,
                  const ToolChain &toolchain, Compilation &compilation) const;

   /// A map for caching Jobs for a given Action/ToolChain pair
   using JobCacheMap =
   llvm::DenseMap<std::pair<const Action *, const ToolChain *>, Job *>;

   /// Create a Job for the given Action \p A, including creating any necessary
   /// input Jobs.
   ///
   /// \param compilation The Compilation which this Job will eventually be part of
   /// \param jobAction The Action for which a Job should be created
   /// \param OFM The OutputFileMap for which a Job should be created
   /// \param AtTopLevel indicates whether or not this is a top-level Job
   /// \param JobCache maps existing Action/ToolChain pairs to Jobs
   ///
   /// \returns a Job for the given Action/ToolChain pair
   Job *buildJobsForAction(Compilation &compilation, const JobAction *jobAction,
                           const OutputFileMap *OFM,
                           StringRef workingDirectory,
                           bool AtTopLevel, JobCacheMap &JobCache) const;

   /// Handle any arguments which should be treated before building actions or
   /// binding tools.
   ///
   /// \return Whether any compilation should be built for this invocation
   bool handleImmediateArgs(const llvm::opt::argList &args, const ToolChain &toolchain);

   /// Print the list of Actions in a Compilation.
   void printActions(const Compilation &compilation) const;

   /// Print the driver version.
   void printVersion(const ToolChain &toolchain, raw_ostream &OS) const;

   /// Print the help text.
   ///
   /// \param ShowHidden Show hidden options.
   void printHelp(bool ShowHidden) const;

private:
   void computeMainOutput(Compilation &compilation, const JobAction *jobAction,
                          const OutputFileMap *OFM, bool AtTopLevel,
                          SmallVectorImpl<const Action *> &InputActions,
                          SmallVectorImpl<const Job *> &InputJobs,
                          const TypeToPathMap *outputMap,
                          StringRef workingDirectory,
                          StringRef BaseInput,
                          StringRef PrimaryInput,
                          llvm::SmallString<128> &buf,
                          CommandOutput *output) const;

   void choosePolarphpModuleOutputPath(Compilation &compilation,
                                    const TypeToPathMap *outputMap,
                                    StringRef workingDirectory,
                                    CommandOutput *output) const;

   void choosePolarphpModuleDocOutputPath(Compilation &compilation,
                                       const TypeToPathMap *outputMap,
                                       StringRef workingDirectory,
                                       CommandOutput *output) const;

   void chooseParseableInterfacePath(Compilation &compilation, const JobAction *jobAction,
                                     StringRef workingDirectory,
                                     llvm::SmallString<128> &buffer,
                                     CommandOutput *output) const;

   void chooseRemappingOutputPath(Compilation &compilation, const TypeToPathMap *outputMap,
                                  CommandOutput *output) const;

   void chooseSerializedDiagnosticsPath(Compilation &compilation, const JobAction *jobAction,
                                        const TypeToPathMap *outputMap,
                                        StringRef workingDirectory,
                                        CommandOutput *output) const;

   void chooseDependenciesOutputPaths(Compilation &compilation,
                                      const TypeToPathMap *outputMap,
                                      StringRef workingDirectory,
                                      llvm::SmallString<128> &buf,
                                      CommandOutput *output) const;

   void chooseOptimizationRecordPath(Compilation &compilation,
                                     StringRef workingDirectory,
                                     llvm::SmallString<128> &buf,
                                     CommandOutput *output) const;

   void chooseLoadedModuleTracePath(Compilation &compilation,
                                    StringRef workingDirectory,
                                    llvm::SmallString<128> &buf,
                                    CommandOutput *output) const;

   void chooseTBDPath(Compilation &compilation, const TypeToPathMap *outputMap,
                      StringRef workingDirectory, llvm::SmallString<128> &buf,
                      CommandOutput *output) const;

private:
   /// Parse the driver kind.
   ///
   /// \param args The arguments passed to the driver (excluding the path to the
   /// driver)
   void parseDriverKind(ArrayRef<const char *> args);

   /// Examine potentially conficting arguments and warn the user if
   /// there is an actual conflict.
   /// \param args The input arguments.
   /// \param inputs The inputs to the driver.
   /// \param batchModeOut An out-parameter flag that indicates whether to
   /// batch the jobs of the resulting \c Mode::StandardCompile compilation.
   OutputInfo::Mode computeCompilerMode(const llvm::opt::DerivedArgList &args,
                                        const InputFileList &inputs,
                                        bool &batchModeOut) const;

private:
   std::unique_ptr<llvm::opt::OptTable> m_opts;

   DiagnosticEngine &m_diags;

   /// The name the driver was invoked as.
   std::string m_name;

   /// The original path to the executable.
   std::string m_driverExecutable;

   // Extra args to pass to the driver executable
   SmallVector<std::string, 2> m_driverExecutableArgs;

   DriverKind m_driverKind = DriverKind::Interactive;

   /// Default target triple.
   std::string m_defaultTargetTriple;

   /// Indicates whether the driver should print bindings.
   bool m_driverPrintBindings;

   /// Indicates whether the driver should suppress the "no input files" error.
   bool m_suppressNoInputFilesError = false;

   /// Indicates whether the driver should check that the input files exist.
   bool m_checkInputFilesExist = true;
};

} // polar::driver

#endif // POLARPHP_DRIVER_DRIVER_H