//===--- TBDGen.cpp - Swift TBD Generation --------------------------------===//
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
//
//  This file implements the entrypoints into TBD file generation.
//
//===----------------------------------------------------------------------===//

#include "polarphp/tbdgen/TBDGen.h"
#include "polarphp/tbdgen/internal/TBDGenVisitor.h"

#include "polarphp/ast/Availability.h"
#include "polarphp/ast/AstMangler.h"
#include "polarphp/ast/AstVisitor.h"
#include "polarphp/ast/DiagnosticsFrontend.h"
#include "polarphp/ast/Module.h"
#include "polarphp/ast/ParameterList.h"
#include "polarphp/ast/PropertyWrappers.h"
#include "polarphp/basic/LLVM.h"
#include "polarphp/clangimporter/ClangImporter.h"
#include "polarphp/irgen/IRGenPublic.h"
#include "polarphp/irgen/Linking.h"
#include "polarphp/pil/lang/FormalLinkage.h"
#include "polarphp/pil/lang/PILDeclRef.h"
#include "polarphp/pil/lang/PILModule.h"
#include "polarphp/pil/lang/PILVTableVisitor.h"
#include "polarphp/pil/lang/PILWitnessTable.h"
#include "polarphp/pil/lang/PILWitnessVisitor.h"
#include "polarphp/pil/lang/TypeLowering.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/TextAPI/MachO/InterfaceFile.h"
#include "llvm/TextAPI/MachO/TextAPIReader.h"
#include "llvm/TextAPI/MachO/TextAPIWriter.h"

using namespace polar;
using namespace polar::irgen;
using namespace polar::tbdgen;
using StringSet = llvm::StringSet<>;
using SymbolKind = llvm::MachO::SymbolKind;

static bool isGlobalOrStaticVar(VarDecl *VD) {
   return VD->isStatic() || VD->getDeclContext()->isModuleScopeContext();
}

void TBDGenVisitor::addSymbol(StringRef name, SymbolKind kind) {
   // The linker expects to see mangled symbol names in TBD files, so make sure
   // to mangle before inserting the symbol.
   SmallString<32> mangled;
   llvm::Mangler::getNameWithPrefix(mangled, name, DataLayout);

   Symbols.addSymbol(kind, mangled, Targets);

   if (StringSymbols && kind == SymbolKind::GlobalSymbol) {
      auto isNewValue = StringSymbols->insert(mangled).second;
      (void)isNewValue;
      assert(isNewValue && "symbol appears twice");
   }
}

void TBDGenVisitor::addSymbol(PILDeclRef declRef) {
   auto linkage = effectiveLinkageForClassMember(
      declRef.getLinkage(ForDefinition),
      declRef.getSubclassScope());
   if (linkage == PILLinkage::Public)
      addSymbol(declRef.mangle());
}

void TBDGenVisitor::addSymbol(LinkEntity entity) {
   auto linkage =
      LinkInfo::get(UniversalLinkInfo, SwiftModule, entity, ForDefinition);

   auto externallyVisible =
      llvm::GlobalValue::isExternalLinkage(linkage.getLinkage()) &&
      linkage.getVisibility() != llvm::GlobalValue::HiddenVisibility;

   if (externallyVisible)
      addSymbol(linkage.getName());
}

void TBDGenVisitor::addDispatchThunk(PILDeclRef declRef) {
   auto entity = LinkEntity::forDispatchThunk(declRef);
   addSymbol(entity);
}

void TBDGenVisitor::addMethodDescriptor(PILDeclRef declRef) {
   auto entity = LinkEntity::forMethodDescriptor(declRef);
   addSymbol(entity);
}

void TBDGenVisitor::addInterfaceRequirementsBaseDescriptor(InterfaceDecl *proto) {
   auto entity = LinkEntity::forInterfaceRequirementsBaseDescriptor(proto);
   addSymbol(entity);
}

void TBDGenVisitor::addAssociatedTypeDescriptor(AssociatedTypeDecl *assocType) {
   auto entity = LinkEntity::forAssociatedTypeDescriptor(assocType);
   addSymbol(entity);
}

void TBDGenVisitor::addAssociatedConformanceDescriptor(
   AssociatedConformance conformance) {
   auto entity = LinkEntity::forAssociatedConformanceDescriptor(conformance);
   addSymbol(entity);
}

void TBDGenVisitor::addBaseConformanceDescriptor(
   BaseConformance conformance) {
   auto entity = LinkEntity::forBaseConformanceDescriptor(conformance);
   addSymbol(entity);
}

void TBDGenVisitor::addConformances(DeclContext *DC) {
   for (auto conformance : DC->getLocalConformances(
      ConformanceLookupKind::NonInherited)) {
      auto interface = conformance->getInterface();
      auto needsWTable =
         lowering::TypeConverter::interfaceRequiresWitnessTable(interface);
      if (!needsWTable)
         continue;

      // Only root conformances get symbols; the others get any public symbols
      // from their parent conformances.
      auto rootConformance = dyn_cast<RootInterfaceConformance>(conformance);
      if (!rootConformance) {
         continue;
      }

      addSymbol(LinkEntity::forInterfaceWitnessTable(rootConformance));
      addSymbol(LinkEntity::forInterfaceConformanceDescriptor(rootConformance));

      // FIXME: the logic around visibility in extensions is confusing, and
      // sometimes witness thunks need to be manually made public.

      auto conformanceIsFixed = PILWitnessTable::conformanceIsSerialized(
         rootConformance);
      auto addSymbolIfNecessary = [&](ValueDecl *requirementDecl,
                                      ValueDecl *witnessDecl) {
         auto witnessLinkage = PILDeclRef(witnessDecl).getLinkage(ForDefinition);
         if (conformanceIsFixed &&
             (isa<SelfInterfaceConformance>(rootConformance) ||
              fixmeWitnessHasLinkageThatNeedsToBePublic(witnessLinkage))) {
            mangle::AstMangler Mangler;
            addSymbol(
               Mangler.mangleWitnessThunk(rootConformance, requirementDecl));
         }
      };

      rootConformance->forEachValueWitness(
         [&](ValueDecl *valueReq, Witness witness) {
            auto witnessDecl = witness.getDecl();
            if (isa<AbstractFunctionDecl>(valueReq)) {
               addSymbolIfNecessary(valueReq, witnessDecl);
            } else if (auto *storage = dyn_cast<AbstractStorageDecl>(valueReq)) {
               auto witnessStorage = cast<AbstractStorageDecl>(witnessDecl);
               storage->visitOpaqueAccessors([&](AccessorDecl *reqtAccessor) {
                  auto witnessAccessor =
                     witnessStorage->getSynthesizedAccessor(
                        reqtAccessor->getAccessorKind());
                  addSymbolIfNecessary(reqtAccessor, witnessAccessor);
               });
            }
         });
   }
}

/// Determine whether dynamic replacement should be emitted for the allocator or
/// the initializer given a decl.
/// The rule is that structs and convenience init of classes emit a
/// dynamic replacement for the allocator.
/// Designated init of classes emit a dynamic replacement for the intializer.
/// This is because the super class init call is emitted to the initializer and
/// needs to be dynamic.
static bool shouldUseAllocatorMangling(const AbstractFunctionDecl *afd) {
   auto constructor = dyn_cast<ConstructorDecl>(afd);
   if (!constructor)
      return false;
   return constructor->getParent()->getSelfClassDecl() == nullptr ||
          constructor->isConvenienceInit();
}

void TBDGenVisitor::visitDefaultArguments(ValueDecl *VD, ParameterList *PL) {
   auto publicDefaultArgGenerators = SwiftModule->isTestingEnabled() ||
                                     SwiftModule->arePrivateImportsEnabled();
   if (!publicDefaultArgGenerators)
      return;

   // In Swift 3 (or under -enable-testing), default arguments (of public
   // functions) are public symbols, as the default values are computed at the
   // call site.
   auto index = 0;
   for (auto *param : *PL) {
      if (param->isDefaultArgument())
         addSymbol(PILDeclRef::getDefaultArgGenerator(VD, index));
      index++;
   }
}

void TBDGenVisitor::visitAbstractFunctionDecl(AbstractFunctionDecl *AFD) {
   // A @_silgen_name("...") function without a body only exists
   // to forward-declare a symbol from another library.
   if (!AFD->hasBody() && AFD->getAttrs().hasAttribute<PILGenNameAttr>()) {
      return;
   }

   addSymbol(PILDeclRef(AFD));

   // Add the global function pointer for a dynamically replaceable function.
   if (AFD->isNativeDynamic()) {
      bool useAllocator = shouldUseAllocatorMangling(AFD);
      addSymbol(LinkEntity::forDynamicallyReplaceableFunctionVariable(
         AFD, useAllocator));
      addSymbol(
         LinkEntity::forDynamicallyReplaceableFunctionKey(AFD, useAllocator));
   }
   if (AFD->getDynamicallyReplacedDecl()) {
      bool useAllocator = shouldUseAllocatorMangling(AFD);
      addSymbol(LinkEntity::forDynamicallyReplaceableFunctionVariable(
         AFD, useAllocator));
      addSymbol(
         LinkEntity::forDynamicallyReplaceableFunctionImpl(AFD, useAllocator));
   }

   if (AFD->getAttrs().hasAttribute<CDeclAttr>()) {
      // A @_cdecl("...") function has an extra symbol, with the name from the
      // attribute.
      addSymbol(PILDeclRef(AFD).asForeign());
   }

   visitDefaultArguments(AFD, AFD->getParameters());
}

void TBDGenVisitor::visitFuncDecl(FuncDecl *FD) {
   // If there's an opaque return type, its descriptor is exported.
   if (auto opaqueResult = FD->getOpaqueResultTypeDecl()) {
      addSymbol(LinkEntity::forOpaqueTypeDescriptor(opaqueResult));
      assert(opaqueResult->getNamingDecl() == FD);
      if (FD->isNativeDynamic()) {
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessor(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorImpl(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorKey(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorVar(opaqueResult));
      }
      if (FD->getDynamicallyReplacedDecl()) {
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessor(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorVar(opaqueResult));
      }
   }
   visitAbstractFunctionDecl(FD);
}

void TBDGenVisitor::visitAccessorDecl(AccessorDecl *AD) {
   llvm_unreachable("should not see an accessor here");
}

void TBDGenVisitor::visitAbstractStorageDecl(AbstractStorageDecl *ASD) {
   // Add the property descriptor if the decl needs it.
   if (ASD->exportsPropertyDescriptor()) {
      addSymbol(LinkEntity::forPropertyDescriptor(ASD));
   }

   // ...and the opaque result decl if it has one.
   if (auto opaqueResult = ASD->getOpaqueResultTypeDecl()) {
      addSymbol(LinkEntity::forOpaqueTypeDescriptor(opaqueResult));
      assert(opaqueResult->getNamingDecl() == ASD);
      if (ASD->hasAnyNativeDynamicAccessors()) {
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessor(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorImpl(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorKey(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorVar(opaqueResult));
      }
      if (ASD->getDynamicallyReplacedDecl()) {
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessor(opaqueResult));
         addSymbol(LinkEntity::forOpaqueTypeDescriptorAccessorVar(opaqueResult));
      }
   }

   // Explicitly look at each accessor here: see visitAccessorDecl.
   ASD->visitEmittedAccessors([&](AccessorDecl *accessor) {
      visitFuncDecl(accessor);
   });
}

void TBDGenVisitor::visitVarDecl(VarDecl *VD) {
   // Variables inside non-resilient modules have some additional symbols.
   if (!VD->isResilient()) {
      // Non-global variables might have an explicit initializer symbol, in
      // non-resilient modules.
      if (VD->getAttrs().hasAttribute<HasInitialValueAttr>() &&
          !isGlobalOrStaticVar(VD)) {
         auto declRef = PILDeclRef(VD, PILDeclRef::Kind::StoredPropertyInitializer);
         // Stored property initializers for public properties are currently
         // public.
         addSymbol(declRef);
      }

      // statically/globally stored variables have some special handling.
      if (VD->hasStorage() &&
          isGlobalOrStaticVar(VD)) {
         if (getDeclLinkage(VD) == FormalLinkage::PublicUnique) {
            // The actual variable has a symbol.
            mangle::AstMangler mangler;
            addSymbol(mangler.mangleEntity(VD, false));
         }

         if (VD->isLazilyInitializedGlobal())
            addSymbol(PILDeclRef(VD, PILDeclRef::Kind::GlobalAccessor));
      }

      // Wrapped non-static member properties may have a backing initializer.
      if (auto wrapperInfo = VD->getPropertyWrapperBackingPropertyInfo()) {
         if (wrapperInfo.initializeFromOriginal && !VD->isStatic()) {
            addSymbol(
               PILDeclRef(VD, PILDeclRef::Kind::PropertyWrapperBackingInitializer));
         }
      }
   }

   visitAbstractStorageDecl(VD);
}

void TBDGenVisitor::visitNominalTypeDecl(NominalTypeDecl *NTD) {
   auto declaredType = NTD->getDeclaredType()->getCanonicalType();

   addSymbol(LinkEntity::forNominalTypeDescriptor(NTD));

   // Generic types do not get metadata directly, only through the function.
   if (!NTD->isGenericContext()) {
      addSymbol(LinkEntity::forTypeMetadata(declaredType,
                                            TypeMetadataAddress::AddressPoint));
   }
   addSymbol(LinkEntity::forTypeMetadataAccessFunction(declaredType));

   // There are symbols associated with any interfaces this type conforms to.
   addConformances(NTD);

   for (auto member : NTD->getMembers())
      visit(member);
}

void TBDGenVisitor::visitClassDecl(ClassDecl *CD) {
   if (getDeclLinkage(CD) != FormalLinkage::PublicUnique)
      return;

   auto &ctxt = CD->getAstContext();
   auto isGeneric = CD->isGenericContext();
   auto objCCompatible = ctxt.LangOpts.EnableObjCInterop && !isGeneric;
//   auto isObjC = objCCompatible && CD->isObjC();

   // Metaclasses and ObjC class (duh) are a ObjC thing, and so are not needed in
   // build artifacts/for classes which can't touch ObjC.
   if (objCCompatible) {
      // @todo
      bool addObjCClass = false;
//      if (isObjC) {
//         addObjCClass = true;
//         addSymbol(LinkEntity::forObjCClass(CD));
//      }

      if (CD->getMetaclassKind() == ClassDecl::MetaclassKind::ObjC) {
         addObjCClass = true;
         addSymbol(LinkEntity::forObjCMetaclass(CD));
      } else
         addSymbol(LinkEntity::forSwiftMetaclassStub(CD));

      if (addObjCClass) {
         SmallString<128> buffer;
         addSymbol(CD->getObjCRuntimeName(buffer), SymbolKind::ObjectiveCClass);
      }
   }

   // Some members of classes get extra handling, beyond members of struct/enums,
   // so let's walk over them manually.
   for (auto *var : CD->getStoredProperties())
      addSymbol(LinkEntity::forFieldOffset(var));

   visitNominalTypeDecl(CD);

   bool resilientAncestry = CD->checkAncestry(AncestryFlags::ResilientOther);

   // Types with resilient superclasses have some extra symbols.
   if (resilientAncestry || CD->hasResilientMetadata()) {
      addSymbol(LinkEntity::forClassMetadataBaseOffset(CD));
   }

   auto &Ctx = CD->getAstContext();
   if (Ctx.LangOpts.EnableObjCInterop) {
      if (resilientAncestry) {
         addSymbol(LinkEntity::forObjCResilientClassStub(
            CD, TypeMetadataAddress::AddressPoint));
      }
   }

   // Emit dispatch thunks for every new vtable entry.
   struct VTableVisitor : public PILVTableVisitor<VTableVisitor> {
      TBDGenVisitor &TBD;
      ClassDecl *CD;
      bool FirstTime = true;

   public:
      VTableVisitor(TBDGenVisitor &TBD, ClassDecl *CD)
         : TBD(TBD), CD(CD) {}

      void addMethod(PILDeclRef method) {
         assert(method.getDecl()->getDeclContext() == CD);

         if (CD->hasResilientMetadata()) {
            if (FirstTime) {
               FirstTime = false;

               // If the class is itself resilient and has at least one vtable entry,
               // it has a method lookup function.
               TBD.addSymbol(LinkEntity::forMethodLookupFunction(CD));
            }

            TBD.addDispatchThunk(method);
         }

         TBD.addMethodDescriptor(method);
      }

      void addMethodOverride(PILDeclRef baseRef, PILDeclRef derivedRef) {}

      void addPlaceholder(MissingMemberDecl *) {}

      void doIt() {
         addVTableEntries(CD);
      }
   };

   VTableVisitor(*this, CD).doIt();
}

void TBDGenVisitor::visitConstructorDecl(ConstructorDecl *CD) {
   if (CD->getParent()->getSelfClassDecl()) {
      // Class constructors come in two forms, allocating and non-allocating. The
      // default ValueDecl handling gives the allocating one, so we have to
      // manually include the non-allocating one.
      addSymbol(PILDeclRef(CD, PILDeclRef::Kind::Initializer));
   }
   visitAbstractFunctionDecl(CD);
}

void TBDGenVisitor::visitDestructorDecl(DestructorDecl *DD) {
   // Class destructors come in two forms (deallocating and non-deallocating),
   // like constructors above. This is the deallocating one:
   visitAbstractFunctionDecl(DD);

   auto parentClass = DD->getParent()->getSelfClassDecl();

   // But the non-deallocating one doesn't apply to some @objc classes.
   if (!lowering::usesObjCAllocator(parentClass)) {
      addSymbol(PILDeclRef(DD, PILDeclRef::Kind::Destroyer));
   }
}

void TBDGenVisitor::visitExtensionDecl(ExtensionDecl *ED) {
   if (!isa<InterfaceDecl>(ED->getExtendedNominal())) {
      addConformances(ED);
   }

   for (auto member : ED->getMembers())
      visit(member);
}

#ifndef NDEBUG
static bool isValidInterfaceMemberForTBDGen(const Decl *D) {
   switch (D->getKind()) {
      case DeclKind::TypeAlias:
      case DeclKind::AssociatedType:
      case DeclKind::Var:
      case DeclKind::Subscript:
      case DeclKind::PatternBinding:
      case DeclKind::Func:
      case DeclKind::Accessor:
      case DeclKind::Constructor:
      case DeclKind::Destructor:
      case DeclKind::IfConfig:
      case DeclKind::PoundDiagnostic:
         return true;
      case DeclKind::OpaqueType:
      case DeclKind::Enum:
      case DeclKind::Struct:
      case DeclKind::Class:
      case DeclKind::Interface:
      case DeclKind::GenericTypeParam:
      case DeclKind::Module:
      case DeclKind::Param:
      case DeclKind::EnumElement:
      case DeclKind::Extension:
      case DeclKind::TopLevelCode:
      case DeclKind::Import:
      case DeclKind::PrecedenceGroup:
      case DeclKind::MissingMember:
      case DeclKind::EnumCase:
      case DeclKind::InfixOperator:
      case DeclKind::PrefixOperator:
      case DeclKind::PostfixOperator:
         return false;
   }
   llvm_unreachable("covered switch");
}
#endif

void TBDGenVisitor::visitInterfaceDecl(InterfaceDecl *PD) {
// @todo
//   if (!PD->isObjC()) {
   addSymbol(LinkEntity::forInterfaceDescriptor(PD));

   struct WitnessVisitor : public PILWitnessVisitor<WitnessVisitor> {
      TBDGenVisitor &TBD;
      InterfaceDecl *PD;

   public:
      WitnessVisitor(TBDGenVisitor &TBD, InterfaceDecl *PD)
         : TBD(TBD), PD(PD) {}

      void addMethod(PILDeclRef declRef) {
         if (PD->isResilient()) {
            TBD.addDispatchThunk(declRef);
            TBD.addMethodDescriptor(declRef);
         }
      }

      void addAssociatedType(AssociatedType associatedType) {
         TBD.addAssociatedTypeDescriptor(associatedType.getAssociation());
      }

      void addInterfaceConformanceDescriptor() {
         TBD.addInterfaceRequirementsBaseDescriptor(PD);
      }

      void addOutOfLineBaseInterface(InterfaceDecl *proto) {
         TBD.addBaseConformanceDescriptor(BaseConformance(PD, proto));
      }

      void addAssociatedConformance(AssociatedConformance associatedConf) {
         TBD.addAssociatedConformanceDescriptor(associatedConf);
      }

      void addPlaceholder(MissingMemberDecl *decl) {}

      void doIt() {
         visitInterfaceDecl(PD);
      }
   };

   WitnessVisitor(*this, PD).doIt();

   // Include the self-conformance.
   addConformances(PD);
//   }

#ifndef NDEBUG
   // There's no (currently) relevant information about members of a interface at
   // individual interfaces, each conforming type has to handle them individually
   // (NB. anything within an active IfConfigDecls also appears outside). Let's
   // assert this fact:
   for (auto *member : PD->getMembers()) {
      assert(isValidInterfaceMemberForTBDGen(member) &&
             "unexpected member of interface during TBD generation");
   }
#endif
}

void TBDGenVisitor::visitEnumDecl(EnumDecl *ED) {
   visitNominalTypeDecl(ED);

   if (!ED->isResilient())
      return;
}

void TBDGenVisitor::visitEnumElementDecl(EnumElementDecl *EED) {
   addSymbol(LinkEntity::forEnumCase(EED));
   if (auto *PL = EED->getParameterList())
      visitDefaultArguments(EED, PL);
}

void TBDGenVisitor::addFirstFileSymbols() {
   if (!Opts.ModuleLinkName.empty()) {
      SmallString<32> buf;
      addSymbol(irgen::encodeForceLoadSymbolName(buf, Opts.ModuleLinkName));
   }
}

/// The kind of version being parsed, used for diagnostics.
/// Note: Must match the order in DiagnosticsFrontend.def
enum DylibVersionKind_t: unsigned {
   CurrentVersion,
   CompatibilityVersion
};

/// Converts a version string into a packed version, truncating each component
/// if necessary to fit all 3 into a 32-bit packed structure.
///
/// For example, the version '1219.37.11' will be packed as
///
///  Major (1,219)       Minor (37) Patch (11)
/// ┌───────────────────┬──────────┬──────────┐
/// │ 00001100 11000011 │ 00100101 │ 00001011 │
/// └───────────────────┴──────────┴──────────┘
///
/// If an individual component is greater than the highest number that can be
/// represented in its alloted space, it will be truncated to the maximum value
/// that fits in the alloted space, which matches the behavior of the linker.
static Optional<llvm::MachO::PackedVersion>
parsePackedVersion(DylibVersionKind_t kind, StringRef versionString,
                   AstContext &ctx) {
   if (versionString.empty())
      return None;

   llvm::MachO::PackedVersion version;
   auto result = version.parse64(versionString);
   if (!result.first) {
      ctx.Diags.diagnose(SourceLoc(), diag::tbd_err_invalid_version,
                         (unsigned)kind, versionString);
      return None;
   }
   if (result.second) {
      ctx.Diags.diagnose(SourceLoc(), diag::tbd_warn_truncating_version,
                         (unsigned)kind, versionString);
   }
   return version;
}

static bool isApplicationExtensionSafe(const LangOptions &LangOpts) {
   // Existing linkers respect these flags to determine app extension safety.
   return LangOpts.EnableAppExtensionRestrictions ||
          llvm::sys::Process::GetEnv("LD_NO_ENCRYPT") ||
          llvm::sys::Process::GetEnv("LD_APPLICATION_EXTENSION_SAFE");
}

static void enumeratePublicSymbolsAndWrite(ModuleDecl *M, FileUnit *singleFile,
                                           StringSet *symbols,
                                           llvm::raw_ostream *os,
                                           const TBDGenOptions &opts) {
   auto &ctx = M->getAstContext();
   auto isWholeModule = singleFile == nullptr;
   const auto &triple = ctx.LangOpts.Target;
   UniversalLinkageInfo linkInfo(triple, opts.HasMultipleIGMs, false,
                                 isWholeModule);

   llvm::MachO::InterfaceFile file;
   file.setFileType(llvm::MachO::FileType::TBD_V3);
   file.setApplicationExtensionSafe(
      isApplicationExtensionSafe(M->getAstContext().LangOpts));
   file.setInstallName(opts.InstallName);
   file.setTwoLevelNamespace();
   file.setSwiftABIVersion(irgen::getPolarphpABIVersion());
   file.setInstallAPI(opts.IsInstallAPI);

   if (auto packed = parsePackedVersion(CurrentVersion,
                                        opts.CurrentVersion, ctx)) {
      file.setCurrentVersion(*packed);
   }

   if (auto packed = parsePackedVersion(CompatibilityVersion,
                                        opts.CompatibilityVersion, ctx)) {
      file.setCompatibilityVersion(*packed);
   }

   llvm::MachO::Target target(triple);
   file.addTarget(target);

   auto *clang = static_cast<ClangImporter *>(ctx.getClangModuleLoader());
   TBDGenVisitor visitor(file, {target}, symbols,
                         clang->getTargetInfo().getDataLayout(),
                         linkInfo, M, opts);

   auto visitFile = [&](FileUnit *file) {
      if (file == M->getFiles()[0]) {
         visitor.addFirstFileSymbols();
      }

      SmallVector<Decl *, 16> decls;
      file->getTopLevelDecls(decls);

      visitor.addMainIfNecessary(file);

      for (auto d : decls)
         visitor.visit(d);
   };

   if (singleFile) {
      assert(M == singleFile->getParentModule() && "mismatched file and module");
      visitFile(singleFile);
   } else {
      for (auto *file : M->getFiles()) {
         visitFile(file);
      }
   }

   if (os) {
      llvm::cantFail(llvm::MachO::TextAPIWriter::writeToStream(*os, file),
                     "YAML writing should be error-free");
   }
}

void polar::enumeratePublicSymbols(FileUnit *file, StringSet &symbols,
                                   const TBDGenOptions &opts) {
   enumeratePublicSymbolsAndWrite(file->getParentModule(), file, &symbols,
                                  nullptr, opts);
}
void polar::enumeratePublicSymbols(ModuleDecl *M, StringSet &symbols,
                                   const TBDGenOptions &opts) {
   enumeratePublicSymbolsAndWrite(M, nullptr, &symbols, nullptr, opts);
}
void polar::writeTBDFile(ModuleDecl *M, llvm::raw_ostream &os,
                         const TBDGenOptions &opts) {
   enumeratePublicSymbolsAndWrite(M, nullptr, nullptr, &os, opts);
}
