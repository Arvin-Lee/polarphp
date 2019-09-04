// This source file is part of the polarphp.org open source project
//
// Copyright (c) 2017 - 2019 polarphp software foundation
// Copyright (c) 2017 - 2019 zzu_softboy <zzu_softboy@163.com>
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://polarphp.org/LICENSE.txt for license information
// See https://polarphp.org/CONTRIBUTORS.txt for the list of polarphp project authors
//
// Created by polarboy on 2019/09/04.

#include <map>

#include "polarphp/syntax/internal/CollectionSyntaxNodeExtraFuncs.h"
#include "polarphp/syntax/SyntaxKindEnumDefs.h"

namespace polar::syntax::internal {
namespace {
using CollectionElementTypeChoicesMap = const std::map<SyntaxKind, std::set<SyntaxKind>>;
static const std::map<SyntaxKind, std::set<SyntaxKind>> scg_collectionElementTypeChoicesMap{
    // decl syntax colloection
    // expr syntax collection
    // stmt syntax collection
};
} // anonymous namespace

std::set<SyntaxKind> retrive_collection_syntax_element_type_choices(SyntaxKind kind)
{
    CollectionElementTypeChoicesMap::const_iterator iter = scg_collectionElementTypeChoicesMap.find(kind);
    assert(iter != scg_collectionElementTypeChoicesMap.cend() && "unknown collection syntax kind");
    return iter->second;
}

} // polar::syntax::internal