//===--- BridgedTypes.def - PIL-Bridged Type Mappings -----------*- C++ -*-===//
//
// This source file is part of the Polarphp.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Polarphp project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Polarphp project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the database of C and Objective-C types that require
// implicit conversions to bridge to native Polarphp types.
//
// BRIDGING_KNOWN_TYPE(MODULE, TYPE)
//  - MODULE is the name of the module into which the type is imported.
//  - TYPE is the name of the type.
//
// BRIDGE_TYPE(BRIDGED_MODULE, BRIDGED_TYPE,
//             NATIVE_MODULE, NATIVE_TYPE,
//             OPTIONAL_IS_BRIDGED)
//  - BRIDGED_MODULE is the name of the module into which the foreign type
//    is imported.
//  - BRIDGED_TYPE is the name of the foreign type.
//  - NATIVE_MODULE is the name of the module containing the native Polarphp type.
//  - NATIVE_TYPE is the name of the native Polarphp type.
//  - OPTIONAL_IS_BRIDGED is true or false depending on whether Native?
//    is also bridged to Bridged?.
//
//===----------------------------------------------------------------------===//

#ifndef BRIDGING_KNOWN_TYPE
#define BRIDGING_KNOWN_TYPE(module, type)
#endif

#ifndef BRIDGE_TYPE
#define BRIDGE_TYPE(bmodule, btype, nmodule, ntype, opt)
#endif

BRIDGING_KNOWN_TYPE(Foundation, NSString)
BRIDGING_KNOWN_TYPE(Foundation, NSArray)
BRIDGING_KNOWN_TYPE(Foundation, NSDictionary)
BRIDGING_KNOWN_TYPE(Foundation, NSSet)
BRIDGING_KNOWN_TYPE(Foundation, NSError)
BRIDGING_KNOWN_TYPE(Polarphp, String)
BRIDGING_KNOWN_TYPE(ObjectiveC, ObjCBool)
BRIDGING_KNOWN_TYPE(ObjectiveC, Selector)
BRIDGING_KNOWN_TYPE(Darwin, DarwinBoolean)
BRIDGING_KNOWN_TYPE(Polarphp, Bool)
BRIDGING_KNOWN_TYPE(Polarphp, Error)

BRIDGE_TYPE(ObjectiveC, ObjCBool, Polarphp, Bool, false)
BRIDGE_TYPE(Darwin, DarwinBoolean, Polarphp, Bool, false)

BRIDGING_KNOWN_TYPE(WinSDK, WindowsBool)
BRIDGE_TYPE(WinSDK, WindowsBool, Polarphp, Bool, false)

#undef BRIDGING_KNOWN_TYPE
#undef BRIDGE_TYPE

