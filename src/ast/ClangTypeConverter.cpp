//===--- ClangTypeConverter.cpp - Convert Swift types to C types ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements generation of Clang AST types from Swift AST types for
// types that are representable in Objective-C interfaces.
// Large chunks of the code are lightly modified versions of the code in
// IRGen/GenClangType.cpp (which should eventually go away), so make sure
// to keep the two in sync.
// The three major differences are that, in this file:
// 1. We fail gracefully instead of asserting/UB.
// 2. We try to keep clang sugar instead of discarding it.
// 3. We use getAs instead of cast as we handle Swift types with sugar.
//
//===----------------------------------------------------------------------===//

#include "polarphp/ast/internal/ClangTypeConverter.h"

#include "polarphp/ast/ASTContext.h"
#include "polarphp/ast/ClangModuleLoader.h"
#include "polarphp/ast/ClangPolarphpTypeCorrespondence.h"
#include "polarphp/ast/ExistentialLayout.h"
#include "polarphp/ast/Module.h"
#include "polarphp/ast/Type.h"
#include "polarphp/ast/TypeVisitor.h"
#include "polarphp/basic/LLVM.h"

#include "clang/AST/ASTContext.h"
#include "clang/Sema/Sema.h"

namespace polar {

namespace {

static Type getNamedPolarphpType(ModuleDecl *stdlib, StringRef name) {
   auto &ctx = stdlib->getAstContext();
   SmallVector < ValueDecl * , 1 > results;
   stdlib->lookupValue(ctx.getIdentifier(name), NLKind::QualifiedLookup,
                       results);

   // If we have one single type decl, and that decl has been
   // type-checked, return its declared type.
   //
   // ...non-type-checked types should only ever show up here because
   // of test cases using -enable-source-import, but unfortunately
   // that's a real thing.
   if (results.size() == 1) {
      if (auto typeDecl = dyn_cast<TypeDecl>(results[0]))
         return typeDecl->getDeclaredInterfaceType();
   }
   return Type();
}

static clang::QualType
getClangBuiltinTypeFromKind(const clang::ASTContext &context,
                            clang::BuiltinType::Kind kind) {
   switch (kind) {
#define BUILTIN_TYPE(Id, SingletonId)                                          \
  case clang::BuiltinType::Id:                                                 \
    return context.SingletonId;

#include "clang/AST/BuiltinTypes.def"

#define IMAGE_TYPE(ImgType, Id, SingletonId, Access, Suffix)                   \
  case clang::BuiltinType::Id:                                                 \
    return context.SingletonId;

#include "clang/Basic/OpenCLImageTypes.def"

#define EXT_OPAQUE_TYPE(ExtType, Id, Ext)                                      \
  case clang::BuiltinType::Id:                                                 \
    return context.Id##Ty;

#include "clang/Basic/OpenCLExtensionTypes.def"

#define SVE_TYPE(Name, Id, SingletonId)                                        \
  case clang::BuiltinType::Id:                                                 \
    return context.SingletonId;

#include "clang/Basic/AArch64SVEACLETypes.def"
   }

   // Not a valid BuiltinType.
   return clang::QualType();
}

static clang::QualType getClangSelectorType(
   const clang::ASTContext &clangCtx) {
   return clangCtx.getPointerType(clangCtx.ObjCBuiltinSelTy);
}

static clang::QualType getClangMetatypeType(
   const clang::ASTContext &clangCtx) {
   clang::QualType clangType =
      clangCtx.getObjCObjectType(clangCtx.ObjCBuiltinClassTy, nullptr, 0);
   return clangCtx.getObjCObjectPointerType(clangType);
}

static clang::QualType getClangIdType(
   const clang::ASTContext &clangCtx) {
   clang::QualType clangType =
      clangCtx.getObjCObjectType(clangCtx.ObjCBuiltinIdTy, nullptr, 0);
   return clangCtx.getObjCObjectPointerType(clangType);
}

static clang::QualType getClangDecayedVaListType(
   const clang::ASTContext &clangCtx) {
   clang::QualType clangType = clangCtx.getBuiltinVaListType();
   if (clangType->isConstantArrayType())
      clangType = clangCtx.getDecayedType(clangType);
   return clangType;
}

} // end anonymous namespace

const clang::Type *ClangTypeConverter::getFunctionType(
   ArrayRef <AnyFunctionType::Param> params, Type resultTy,
   AnyFunctionType::Representation repr) {

   SmallVector<Type, 4> paramsTy;
   for (auto p : params)
      paramsTy.push_back(p.getPlainType());

   auto resultClangTy = convert(resultTy);
   if (resultClangTy.isNull())
      return nullptr;

   SmallVector<clang::QualType, 8> paramsClangTy;
   for (auto p : paramsTy) {
      auto pc = convert(p);
      if (pc.isNull())
         return nullptr;
      paramsClangTy.push_back(pc);
   }

   clang::FunctionProtoType::ExtProtoInfo info(clang::CallingConv::CC_C);
   auto fn = ClangASTContext.getFunctionType(resultClangTy, paramsClangTy, info);
   if (fn.isNull())
      return nullptr;

   switch (repr) {
      case AnyFunctionType::Representation::CFunctionPointer:
         return fn.getTypePtr();
      case AnyFunctionType::Representation::Block:
         return ClangASTContext.getBlockPointerType(fn).getTypePtrOrNull();
      case AnyFunctionType::Representation::Swift:
      case AnyFunctionType::Representation::Thin:
         llvm_unreachable("Expected a C-compatible representation.");
   }
}

clang::QualType ClangTypeConverter::convertMemberType(NominalTypeDecl *DC,
                                                      StringRef memberName) {
   auto memberTypeDecl = cast<TypeDecl>(
      DC->lookupDirect(Context.getIdentifier(memberName))[0]);
   auto memberType = memberTypeDecl->getDeclaredInterfaceType();
   return convert(memberType);
}

// TODO: It is unfortunate that we parse the name of a public library type
// in order to break it down into a vector component and length that in theory
// we could recover in some other way.
static clang::QualType getClangVectorType(const clang::ASTContext &ctx,
                                          clang::BuiltinType::Kind eltKind,
                                          clang::VectorType::VectorKind vecKind,
                                          StringRef numEltsString) {
   unsigned numElts;
   bool failedParse = numEltsString.getAsInteger<unsigned>(10, numElts);
   if (failedParse)
      return clang::QualType();
   auto eltTy = getClangBuiltinTypeFromKind(ctx, eltKind);
   if (eltTy.isNull())
      return clang::QualType();
   return ctx.getVectorType(eltTy, numElts, vecKind);
}

clang::QualType ClangTypeConverter::visitStructType(StructType *type) {
   auto &ctx = ClangASTContext;

   auto swiftDecl = type->getDecl();
   StringRef name = swiftDecl->getName().str();

   // We assume that the importer translates all of the following types
   // directly to structs in the standard library.

   // We want to recognize most of these types by name.
#define CHECK_NAMED_TYPE(NAME, CLANG_TYPE) do {                         \
    if (name == (NAME)) return CLANG_TYPE;                              \
  } while (false)

   CHECK_NAMED_TYPE("CGFloat", convertMemberType(swiftDecl, "NativeType"));
   CHECK_NAMED_TYPE("OpaquePointer", ctx.VoidPtrTy);
   CHECK_NAMED_TYPE("CVaListPointer", getClangDecayedVaListType(ctx));
   CHECK_NAMED_TYPE("DarwinBoolean", ctx.UnsignedCharTy);
   CHECK_NAMED_TYPE("WindowsBool", ctx.IntTy);
   CHECK_NAMED_TYPE("ObjCBool", ctx.ObjCBuiltinBoolTy);
   CHECK_NAMED_TYPE("Selector", getClangSelectorType(ctx));
   CHECK_NAMED_TYPE("UnsafeRawPointer", ctx.VoidPtrTy);
   CHECK_NAMED_TYPE("UnsafeMutableRawPointer", ctx.VoidPtrTy);
#undef CHECK_NAMED_TYPE

   // Map vector types to the corresponding C vectors.
#define MAP_SIMD_TYPE(TYPE_NAME, _, BUILTIN_KIND)                      \
  if (name.startswith(#TYPE_NAME)) {                                   \
    return getClangVectorType(ctx, clang::BuiltinType::BUILTIN_KIND,   \
                              clang::VectorType::GenericVector,        \
                              name.drop_front(sizeof(#TYPE_NAME)-1));  \
  }

#include "polarphp/clangimporter/SIMDMappedTypesDef.h"

   // We might be looking at a builtin
   auto ret = reverseBuiltinTypeMapping(type);
   if (!ret.isNull())
      return ret;

   if (type->isPotentiallyBridgedValueType()) {
//      if (auto t = Context.getBridgedToObjC(type->getDecl(), type))
//         return convert(t);
   }

   // Out of ideas, there must've been some error. :(
   return clang::QualType();
}

static clang::QualType
getClangBuiltinTypeFromTypedef(clang::Sema &sema, StringRef typedefName) {
   auto &context = sema.getASTContext();
   auto identifier = &context.Idents.get(typedefName);
   auto found = sema.LookupSingleName(sema.TUScope, identifier,
                                      clang::SourceLocation(),
                                      clang::Sema::LookupOrdinaryName);
   auto typedefDecl = dyn_cast_or_null<clang::TypedefDecl>(found);
   if (!typedefDecl)
      return clang::QualType();

   auto underlyingTy =
      context.getCanonicalType(typedefDecl->getUnderlyingType());

   if (underlyingTy->getAs<clang::BuiltinType>())
      return underlyingTy;
   return clang::QualType();
}

clang::QualType
ClangTypeConverter::reverseBuiltinTypeMapping(StructType *type) {
   // Handle builtin types by adding entries to the cache that reverse
   // the mapping done by the importer.  We could try to look at the
   // members of the struct instead, but even if that's ABI-equivalent
   // (which it had better be!), it might erase interesting semantic
   // differences like integers vs. characters.  This is important
   // because CC lowering isn't the only purpose of this conversion.
   //
   // The importer maps builtin types like 'int' to named types like
   // 'CInt', which are generally typealiases.  So what we do here is
   // map the underlying types of those typealiases back to the builtin
   // type.  These typealiases frequently create a many-to-one mapping,
   // so just use the first type that mapped to a particular underlying
   // type.
   //
   // This is the last thing that happens before asserting that the
   // struct type doesn't have a mapping.  Furthermore, all of the
   // builtin types are pre-built in the clang ASTContext.  So it's not
   // really a significant performance problem to just cache all them
   // right here; it makes making a few more entries in the cache than
   // we really need, but it also means we won't end up repeating these
   // stdlib lookups multiple times, and we have to perform multiple
   // lookups anyway because the MAP_BUILTIN_TYPE database uses
   // typealias names (like 'CInt') that aren't obviously associated
   // with the underlying C library type.

   auto stdlib = Context.getStdlibModule();
   assert(stdlib && "translating stdlib type to C without stdlib module?");
   auto &ctx = ClangASTContext;

   if (!StdlibTypesAreCached) {
      auto cacheStdlibType = [&](StringRef swiftName,
                                 clang::BuiltinType::Kind builtinKind) {
         Type swiftType = getNamedPolarphpType(stdlib, swiftName);
         if (!swiftType) return;

         auto &sema = Context.getClangModuleLoader()->getClangSema();

         if (Context.LangOpts.EnableObjCInterop) {
            // Handle Int and UInt specially. On Apple platforms, these map to
            // the NSInteger and NSUInteger typedefs. So try that if the typedefs
            // are available, to ensure we get consistent ObjC @encode strings.
            if (swiftType->getAnyNominal() == Context.getIntDecl()) {
               auto NSIntegerTy = getClangBuiltinTypeFromTypedef(sema, "NSInteger");
               if (!NSIntegerTy.isNull()) {
                  Cache.insert({swiftType->getCanonicalType(), NSIntegerTy});
                  return;
               }
            } else if (swiftType->getAnyNominal() == Context.getUIntDecl()) {
               auto NSUIntegerTy = getClangBuiltinTypeFromTypedef(sema, "NSUInteger");
               if (!NSUIntegerTy.isNull()) {
                  Cache.insert({swiftType->getCanonicalType(), NSUIntegerTy});
                  return;
               }
            }
         }

         // For something like `typealias CInt = Int32`, reverseBuiltinTypeMapping
         // will get Int32 as the input, so we need to record the desugared type.
         Cache.insert({swiftType->getCanonicalType(),
                       getClangBuiltinTypeFromKind(ctx, builtinKind)});
      };

#define MAP_BUILTIN_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME)          \
    cacheStdlibType(#SWIFT_TYPE_NAME, clang::BuiltinType::CLANG_BUILTIN_KIND);

#include "polarphp/clangimporter/BuiltinMappedTypesDef.h"

      // On 64-bit Windows, no C type is imported as an Int or UInt; CLong is
      // imported as an Int32 and CLongLong as an Int64. Therefore, manually
      // add mappings to C for Int and UInt.
      // On 64-bit Cygwin, no manual mapping is required.
      if (Triple.isOSWindows() && Triple.isArch64Bit()
          && !Triple.isWindowsCygwinEnvironment()) {
         // Map UInt to uintptr_t
         auto polarphpUIntType = getNamedPolarphpType(stdlib, "UInt");
         auto clangUIntPtrType = ctx.getCanonicalType(ctx.getUIntPtrType());
         Cache.insert({polarphpUIntType, clangUIntPtrType});

         // Map Int to intptr_t
         auto polarphpIntType = getNamedPolarphpType(stdlib, "Int");
         auto clangIntPtrType = ctx.getCanonicalType(ctx.getIntPtrType());
         Cache.insert({polarphpIntType, clangIntPtrType});
      }
      StdlibTypesAreCached = true;
   }

   auto it = Cache.find(Type(type));
   if (it != Cache.end())
      return it->second;

   it = Cache.find(type->getCanonicalType());
   if (it != Cache.end()) {
      Cache.insert({Type(type), it->second});
      return it->second;
   }

   return clang::QualType();
}

clang::QualType ClangTypeConverter::visitTupleType(TupleType *type) {
// @todo
//   unsigned tupleNumElements = type->getNumElements();
//   if (tupleNumElements == 0)
//      return ClangASTContext.VoidTy;
//
//   Type eltTy = type->getElementType(0);
//   for (unsigned i = 1; i < tupleNumElements; i++) {
//      if (!eltTy->isEqual(type->getElementType(i)))
//         // Only tuples where all element types are equal map to fixed-size
//         // arrays.
//         return clang::QualType();
//   }
//
//   auto clangEltTy = convert(eltTy);
//   if (clangEltTy.isNull())
//      return clang::QualType();
//
//   APInt size(32, tupleNumElements);
//   return ClangASTContext.getConstantArrayType(clangEltTy, size,
//                                               clang::ArrayType::Normal, 0);
}

clang::QualType ClangTypeConverter::visitInterfaceType(InterfaceType *type) {
//@todo
//   auto proto = type->getDecl();
//   auto &clangCtx = ClangASTContext;
//
//   if (!proto->isObjC())
//      return clang::QualType();
//
//   assert(!cast_or_null<clang::ObjCInterfaceDecl>(proto->getClangDecl())
//          && "We shouldn't be creating duplicate decls; see `convert`");
//
//   // Single protocol -> id<Proto>
//   clang::IdentifierInfo *name = &clangCtx.Idents.get(proto->getName().get());
//   auto *PDecl = clang::ObjCInterfaceDecl::Create(
//      const_cast<clang::ASTContext &>(clangCtx),
//      clangCtx.getTranslationUnitDecl(), name,
//      clang::SourceLocation(), clang::SourceLocation(), nullptr);
//
//   // Attach an objc_runtime_name attribute with the Objective-C name to use
//   // for this protocol.
//   SmallString<64> runtimeNameBuffer;
//   PDecl->addAttr(clang::ObjCRuntimeNameAttr::CreateImplicit(
//      PDecl->getAstContext(),
//      proto->getObjCRuntimeName(runtimeNameBuffer)));
//
//   auto clangType = clangCtx.getObjCObjectType(clangCtx.ObjCBuiltinIdTy,
//                                               &PDecl, 1);
//   return clangCtx.getObjCObjectPointerType(clangType);
}

// TODO: [stronger-checking-in-clang-type-conversion]
// Metatypes can be converted to Class when they are metatypes for concrete
// classes. https://github.com/apple/swift/pull/27479#discussion_r344418131
clang::QualType ClangTypeConverter::visitMetatypeType(MetatypeType *type) {
   return getClangMetatypeType(ClangASTContext);
}

// TODO: [stronger-checking-in-clang-type-conversion]
// Existential metatypes where the base is a non-metatype existential can be
// converted to Class<P, Q, ...> when the protocols are all ObjC.
// https://github.com/apple/swift/pull/27479#discussion_r344418131
clang::QualType
ClangTypeConverter::visitExistentialMetatypeType(ExistentialMetatypeType *type) {
   return getClangMetatypeType(ClangASTContext);
}

clang::QualType ClangTypeConverter::visitClassType(ClassType *type) {
   auto &clangCtx = ClangASTContext;
   auto swiftDecl = type->getDecl();

   // TODO: [non-objc-class-clang-type-conversion]
   // See the corresponding note in GenClangType.cpp
//   if (!swiftDecl->isObjC())
//      return getClangIdType(clangCtx);
   return getClangIdType(clangCtx);

   assert(!cast_or_null<clang::ObjCInterfaceDecl>(swiftDecl->getClangDecl())
          && "We shouldn't be creating duplicate decls; see `convert`");

   // produce the clang type INTF * if it is imported ObjC object.
   clang::IdentifierInfo *ForwardClassId =
      &clangCtx.Idents.get(swiftDecl->getName().get());
   auto *CDecl = clang::ObjCInterfaceDecl::Create(
      clangCtx, clangCtx.getTranslationUnitDecl(),
      clang::SourceLocation(), ForwardClassId,
      /*typeParamList*/nullptr, /*PrevDecl=*/nullptr,
      clang::SourceLocation());

   // Attach an objc_runtime_name attribute with the Objective-C name to use
   // for this class.
   SmallString<64> runtimeNameBuffer;
//   CDecl->addAttr(clang::ObjCRuntimeNameAttr::CreateImplicit(
//      CDecl->getASTContext(),
//      swiftDecl->getObjCRuntimeName(runtimeNameBuffer)));

   auto clangType = clangCtx.getObjCInterfaceType(CDecl);
   return clangCtx.getObjCObjectPointerType(clangType);
}

// TODO: We should try to preserve type arguments on imported ObjC generic
// classes, instead of relying on our knowledge of clang's encoding.
// This would entail extracting the type arguments, calling `convert` to
// create clang types, extracting the ObjCInterfaceDecls and then using
// getObjCObjectType with `id` as the base.
clang::QualType
ClangTypeConverter::visitBoundGenericClassType(BoundGenericClassType *type) {
   // Any @objc class type in Swift that shows up in an @objc method maps 1-1 to
   // "id <SomeProto>"; with clang's encoding ignoring the protocol list.
   return getClangIdType(ClangASTContext);
}

clang::QualType
ClangTypeConverter::visitBoundGenericType(BoundGenericType *type) {
   // The only possibilities are *Pointer<T>, SIMD*<T> and Optional<T>.

   if (type->getDecl()->isOptionalDecl()) {
      auto args = type->getGenericArgs();
      assert((args.size() == 1) && "Optional should have 1 generic argument.");
      clang::QualType innerTy = convert(args[0]);
      if (canImportAsOptional(innerTy.getTypePtrOrNull()))
         return innerTy;
      return clang::QualType();
   }

   auto swiftStructDecl = type->getDecl();

   enum class StructKind {
      Invalid,
      UnsafeMutablePointer,
      UnsafePointer,
      AutoreleasingUnsafeMutablePointer,
      Unmanaged,
      CFunctionPointer,
      SIMD,
   } kind = llvm::StringSwitch<StructKind>(swiftStructDecl->getName().str())
      .Case("UnsafeMutablePointer", StructKind::UnsafeMutablePointer)
      .Case("UnsafePointer", StructKind::UnsafePointer)
      .Case("AutoreleasingUnsafeMutablePointer",
            StructKind::AutoreleasingUnsafeMutablePointer)
      .Case("Unmanaged", StructKind::Unmanaged)
      .Case("CFunctionPointer", StructKind::CFunctionPointer)
      .StartsWith("SIMD", StructKind::SIMD)
      .Default(StructKind::Invalid);

   auto args = type->getGenericArgs();
   if (args.size() != 1)
      // Must've got something other than *Pointer or SIMD*
      return clang::QualType();
   auto argCanonicalTy = args[0]->getCanonicalType();

   switch (kind) {
      case StructKind::Invalid:
         return clang::QualType();

      case StructKind::UnsafeMutablePointer:
      case StructKind::Unmanaged:
      case StructKind::AutoreleasingUnsafeMutablePointer: {
         auto clangTy = convert(argCanonicalTy);
         if (clangTy.isNull())
            return clang::QualType();
         return ClangASTContext.getPointerType(clangTy);
      }
      case StructKind::UnsafePointer: {
         return ClangASTContext.getPointerType(convert(argCanonicalTy).withConst());
      }

      case StructKind::CFunctionPointer: {
         auto &clangCtx = ClangASTContext;

         clang::QualType functionTy;
         if (isa<PILFunctionType>(argCanonicalTy->getCanonicalType())) {
            functionTy = convert(argCanonicalTy);
            if (functionTy.isNull())
               return clang::QualType();
         } else {
            // Fall back to void().
            functionTy = clangCtx.getFunctionNoProtoType(clangCtx.VoidTy);
         }
         return clangCtx.getPointerType(functionTy);
      }

      case StructKind::SIMD: {
         clang::QualType scalarTy = convert(argCanonicalTy);
         auto numEltsString = swiftStructDecl->getName().str();
         numEltsString.consume_front("SIMD");
         unsigned numElts;
         bool failedParse = numEltsString.getAsInteger<unsigned>(10, numElts);
         if (failedParse)
            return clang::QualType();
         (void) failedParse;
         auto vectorTy = ClangASTContext.getVectorType(scalarTy, numElts,
                                                       clang::VectorType::VectorKind::GenericVector);
         return vectorTy;
      }
   }

   llvm_unreachable("Not a valid StructKind.");
}

clang::QualType ClangTypeConverter::visitEnumType(EnumType *type) {
   // Special case: Uninhabited enums are not @objc, so we don't
   // know what to do below, but we can just convert to 'void'.
   if (type->isUninhabited())
      return convert(Context.TheEmptyTupleType);
///@todo
//   if (!type->getDecl()->isObjC())
   // Can't translate something not marked with @objc
   return clang::QualType();

   // @objc enums lower to their raw types.
//   return convert(type->getDecl()->getRawType());
}

clang::QualType ClangTypeConverter::visitFunctionType(FunctionType *type) {
   // We must've already computed it before if applicable.
   return clang::QualType(type->getClangFunctionType(), 0);
}

clang::QualType ClangTypeConverter::visitPILFunctionType(PILFunctionType *type) {
   llvm::report_fatal_error("Expected only AST types but found a PIL function.");
}

clang::QualType
ClangTypeConverter::visitPILBlockStorageType(PILBlockStorageType *type) {
   llvm::report_fatal_error("Expected only AST types but found a PIL block.");
}

clang::QualType
ClangTypeConverter::visitInterfaceCompositionType(InterfaceCompositionType *type) {
//@todo
   // Any will be lowered to AnyObject, so we return the same result.
//   if (type->isAny())
//      return getClangIdType(ClangASTContext);
//
//   auto &clangCtx = ClangASTContext;
//
//   // FIXME. Eventually, this will have its own helper routine.
//   SmallVector<const clang::ObjCProtocolDecl *, 4> Interfaces;
//   auto layout = type->getExistentialLayout();
//   if (!layout.isObjC())
//      // Cannot represent opaque existential in Clang
//      return clang::QualType();
//
//   // AnyObject -> id.
//   if (layout.isAnyObject())
//      return getClangIdType(ClangASTContext);
//
//   auto superclassTy = clangCtx.ObjCBuiltinIdTy;
//   if (auto layoutSuperclassTy = layout.getSuperclass()) {
//      auto clangTy = convert(layoutSuperclassTy);
//      if (clangTy.isNull())
//         return clang::QualType();
//      superclassTy = clangCtx.getCanonicalType(
//         clangTy->getAs<clang::ObjCObjectPointerType>()->getPointeeType());
//   }
//
//   for (Type t : layout.getInterfaces()) {
//      auto clangTy = convert(t);
//      if (clangTy.isNull())
//         return clang::QualType();
//      for (auto p : clangTy->getAs<clang::ObjCObjectPointerType>()->quals())
//         Interfaces.push_back(p);
//   }
//
//   if (Interfaces.empty())
//      return superclassTy;
//
//   // id<protocol-list>
//   clang::ObjCInterfaceDecl **ProtoQuals =
//      new(clangCtx) clang::ObjCInterfaceDecl *[Interfaces.size()];
//   memcpy(ProtoQuals, Interfaces.data(),
//          sizeof(clang::ObjCInterfaceDecl *) * Interfaces.size());
//   auto clangType = clangCtx.getObjCObjectType(superclassTy,
//                                               ProtoQuals,
//                                               Interfaces.size());
//   return clangCtx.getObjCObjectPointerType(clangType);
}

clang::QualType
ClangTypeConverter::visitBuiltinRawPointerType(BuiltinRawPointerType *type) {
   return ClangASTContext.VoidPtrTy;
}

clang::QualType
ClangTypeConverter::visitBuiltinIntegerType(BuiltinIntegerType *type) {
   auto &clangCtx = ClangASTContext;
   if (type->getWidth().isPointerWidth()) {
      return clangCtx.getUIntPtrType();
   }
   assert(type->getWidth().isFixedWidth());
   auto width = type->getWidth().getFixedWidth();
   if (width == 1)
      return clangCtx.BoolTy;
   return clangCtx.getIntTypeForBitwidth(width, /*signed*/ 0);
}

clang::QualType
ClangTypeConverter::visitBuiltinFloatType(BuiltinFloatType *type) {
   auto &clangCtx = ClangASTContext;
   auto &clangTargetInfo = clangCtx.getTargetInfo();
   const llvm::fltSemantics *format = &type->getAPFloatSemantics();
   if (format == &clangTargetInfo.getHalfFormat())
      return clangCtx.HalfTy;
   if (format == &clangTargetInfo.getFloatFormat())
      return clangCtx.FloatTy;
   if (format == &clangTargetInfo.getDoubleFormat())
      return clangCtx.DoubleTy;
   if (format == &clangTargetInfo.getLongDoubleFormat())
      return clangCtx.LongDoubleTy;
   llvm_unreachable("cannot translate floating-point format to C");
}

clang::QualType ClangTypeConverter::visitArchetypeType(ArchetypeType *type) {
   // We see these in the case where we invoke an @objc function
   // through a protocol.
   return getClangIdType(ClangASTContext);
}

clang::QualType ClangTypeConverter::visitDynamicSelfType(DynamicSelfType *type) {
   // Dynamic Self is equivalent to 'instancetype', which is treated as
   // 'id' within the Objective-C type system.
   return getClangIdType(ClangASTContext);
}

clang::QualType
ClangTypeConverter::visitGenericTypeParamType(GenericTypeParamType *type) {
   // We see these in the case where we invoke an @objc function
   // through a protocol argument that is a generic type.
   return getClangIdType(ClangASTContext);
}

clang::QualType
ClangTypeConverter::visitSugarType(SugarType *type) {
   return convert(Type(type->getDesugaredType()));
}

clang::QualType
ClangTypeConverter::visitType(TypeBase *type) {
   // We only convert specific types.
   return clang::QualType();
}

clang::QualType ClangTypeConverter::visit(Type type) {
   return static_cast<super *>(this)->visit(type);
}

clang::QualType ClangTypeConverter::convert(Type type) {
// @todo
//   auto it = Cache.find(type);
//   if (it != Cache.end())
//      return it->second;
//
//   // Try to do this without making cache entries for obvious cases.
//   if (auto nominal = type->getAs<NominalType>()) {
//      auto decl = nominal->getDecl();
//      if (auto clangDecl = decl->getClangDecl()) {
//         auto &ctx = ClangASTContext;
//         if (auto clangTypeDecl = dyn_cast<clang::TypeDecl>(clangDecl)) {
//            return ctx.getTypeDeclType(clangTypeDecl).getUnqualifiedType();
//         } else if (auto ifaceDecl = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl)) {
//            auto clangType = ctx.getObjCInterfaceType(ifaceDecl);
//            return ctx.getObjCObjectPointerType(clangType);
//         } else if (auto protoDecl = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl)) {
//            auto clangType = ctx.getObjCObjectType(
//               ctx.ObjCBuiltinIdTy,
//               const_cast<clang::ObjCInterfaceDecl **>(&protoDecl),
//               1);
//            return ctx.getObjCObjectPointerType(clangType);
//         }
//      }
//   }
//
//   // If that failed, convert the type, cache, and return.
//   clang::QualType result = visit(type);
//   Cache.insert({type, result});
//   return result;
}

} // polar