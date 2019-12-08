//===--- CompilationRecord.h ------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
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
// Created by polarboy on 2019/12/02.

#ifndef POLARPHP_DRIVER_COMPILATION_RECORD_H
#define POLARPHP_DRIVER_COMPILATION_RECORD_H

#include "polarphp/driver/Action.h"

namespace polar::driver::compilationrecord {

/// Compilation record files (-master.swiftdeps files) are YAML files composed
/// of these top-level keys.
enum class TopLevelKey
{
   /// The key for the Polarphp compiler version used to produce the compilation
   /// record.
   Version,
   /// The key for the list of arguments passed to the Polarphp compiler when
   /// producing the compilation record.
   Options,
   /// The key for the time at which the build that produced the compilation
   /// record started.
   BuildTime,
   /// The key for the list of inputs to the compilation that produced the
   /// compilation record.
   Inputs,
};

/// \returns A string representation of the given key.
inline static StringRef get_name(TopLevelKey Key)
{
   switch (Key) {
   case TopLevelKey::Version: return "version";
   case TopLevelKey::Options: return "options";
   case TopLevelKey::BuildTime: return "build_time";
   case TopLevelKey::Inputs: return "inputs";
   }

   // Work around MSVC warning: not all control paths return a value
   llvm_unreachable("All switch cases are covered");
}

/// \returns The string identifier used to represent the given status in a
/// compilation record file (.swiftdeps file).
///
/// \note Not every InputInfo::status has a unique identifier. For example,
/// both NewlyAdded and NeedsCascadingBuild are represented as "!dirty".
/// Therefore, this will not cleanly round-trip between InputInfo::status and
/// string identifiers.
inline static StringRef
get_identifier_for_input_info_status(CompileJobAction::InputInfo::Status status)
{
   switch (status) {
   case CompileJobAction::InputInfo::UpToDate:
      return "";
   case CompileJobAction::InputInfo::NewlyAdded:
   case CompileJobAction::InputInfo::NeedsCascadingBuild:
      return "!dirty";
   case CompileJobAction::InputInfo::NeedsNonCascadingBuild:
      return "!private";
   }

   // Work around MSVC warning: not all control paths return a value
   llvm_unreachable("All switch cases are covered");
}

/// \returns The status corresponding to the string identifier used in a
/// compilation record file (.swiftdeps file).
inline static Optional<CompileJobAction::InputInfo::Status>
get_info_statu_for_identifier(StringRef identifier)
{
   return llvm::StringSwitch<Optional<CompileJobAction::InputInfo::Status>>(identifier)
      .Case("", CompileJobAction::InputInfo::UpToDate)
      .Case("!dirty", CompileJobAction::InputInfo::NeedsCascadingBuild)
      .Case("!private", CompileJobAction::InputInfo::NeedsNonCascadingBuild)
      .Default(None);
}

} // polar::driver

#endif // POLARPHP_DRIVER_COMPILATION_RECORD_H