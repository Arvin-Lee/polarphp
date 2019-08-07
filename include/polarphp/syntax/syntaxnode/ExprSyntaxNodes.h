// This source file is part of the polarphp.org open source project
//
// Copyright (c) 2017 - 2019 polarphp software foundation
// Copyright (c) 2017 - 2019 zzu_softboy <zzu_softboy@163.com>
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://polarphp.org/LICENSE.txt for license information
// See https://polarphp.org/CONTRIBUTORS.txt for the list of polarphp project authors
//
// Created by polarboy on 2019/05/08.

#ifndef POLARPHP_SYNTAX_SYNTAX_NODE_EXPR_NODES_H
#define POLARPHP_SYNTAX_SYNTAX_NODE_EXPR_NODES_H

#include "polarphp/syntax/Syntax.h"
#include "polarphp/syntax/TokenSyntax.h"
#include "polarphp/syntax/UnknownSyntax.h"
#include "polarphp/syntax/syntaxnode/CommonSyntaxNodes.h"
#include "polarphp/syntax/syntaxnode/ExprSyntaxNodesFwd.h"

namespace polar::syntax {

class NullExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      NullKeyword,
   };
public:
   NullExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getNullKeyword();
   NullExprSyntax withNullKeyword(std::optional<TokenSyntax> keyword);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::NullExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class NullExprSyntaxBuilder;
   void validate();
};

///
/// brace_decorated_expr_clause:
///   '{' expr '}'
///
class BraceDecoratedExprClauseSyntax final : public Syntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 3;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 3;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: TokenSyntax
      /// optional: false
      ///
      LeftBrace,
      ///
      /// type: ExprSyntax
      /// optional: false
      ///
      Expr,
      ///
      /// type: TokenSyntax
      /// optional: false
      ///
      RightBrace
   };

public:
   BraceDecoratedExprClauseSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : Syntax(root, data)
   {
      validate();
   }

   TokenSyntax getLeftBrace();
   ExprSyntax getExpr();
   TokenSyntax getRightBrace();

   BraceDecoratedExprClauseSyntax withLeftBrace(std::optional<TokenSyntax> leftBrace);
   BraceDecoratedExprClauseSyntax withExpr(std::optional<ExprSyntax> expr);
   BraceDecoratedExprClauseSyntax withRightBrace(std::optional<TokenSyntax> rightBrace);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::BraceDecoratedExprClause;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }
private:
   friend class BraceDecoratedExprClauseSyntaxBuilder;
   void validate();
};

///
/// expr:
///   '$' '{' expr '}'
///
class BraceDecoratedVariableExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 2;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 2;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: TokenSyntax
      /// optional: false
      ///
      DollarSign,
      ///
      /// type: BraceDecoratedExprClauseSyntax
      /// optional: false
      ///
      DecoratedExpr
   };

public:
   BraceDecoratedVariableExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getDollarSign();
   BraceDecoratedExprClauseSyntax getDecoratedExpr();

   BraceDecoratedVariableExprSyntax withDollarSign(std::optional<TokenSyntax> dollarSign);
   BraceDecoratedVariableExprSyntax withDecoratedExpr(std::optional<BraceDecoratedExprClauseSyntax> decoratedExpr);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::BraceDecoratedVariableExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class BraceDecoratedVariableExprSyntaxBuilder;
   void validate();
};

///
/// array_key_value_pair_item:
///   expr T_DOUBLE_ARROW expr
/// | expr
/// | expr T_DOUBLE_ARROW '&' variable
/// | '&' variable
///
class ArrayKeyValuePairItemSyntax final : public Syntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 4;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: ExprSyntax
      /// optional: true
      ///
      KeyExpr,
      ///
      /// type: TokenSyntax (T_DOUBLE_ARROW)
      /// optional: true
      ///
      DoubleArrowToken,
      ///
      /// type: TokenSyntax (T_AMPERSAND)
      /// optional: true
      ///
      ReferenceToken,
      ///
      /// type: ExprSyntax
      /// optional: false
      /// node choices: true
      /// --------------------------------
      /// node choice: VariableExprSyntax
      /// --------------------------------
      /// node choice: ExprSyntax
      ///
      Value
   };

#ifdef POLAR_DEBUG_BUILD
   const static NodeChoicesType CHILD_NODE_CHOICES;
#endif

public:
   ArrayKeyValuePairItemSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : Syntax(root, data)
   {
      validate();
   }

   std::optional<ExprSyntax> getKeyExpr();
   std::optional<TokenSyntax> getDoubleArrowToken();
   std::optional<TokenSyntax> getReferenceToken();
   ExprSyntax getValue();

   ArrayKeyValuePairItemSyntax withKeyExpr(std::optional<ExprSyntax> keyExpr);
   ArrayKeyValuePairItemSyntax withDoubleArrowToken(std::optional<TokenSyntax> doubleArrowToken);
   ArrayKeyValuePairItemSyntax withReferenceToken(std::optional<TokenSyntax> referenceToken);
   ArrayKeyValuePairItemSyntax withValue(std::optional<ExprSyntax> value);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::ArrayKeyValuePairItem;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class ArrayKeyValuePairItemSyntaxBuilder;
   void validate();
};

///
/// array_unpack_pair_item:
///   T_ELLIPSIS expr
///
class ArrayUnpackPairItemSyntax final : public Syntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 2;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 2;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: TokenSyntax (T_ELLIPSIS)
      /// optional: false
      ///
      EllipsisToken,
      ///
      /// type: ExprSyntax
      /// optional: false
      ///
      UnpackExpr
   };

public:
   ArrayUnpackPairItemSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : Syntax(root, data)
   {
      validate();
   }

   TokenSyntax getEllipsisToken();
   ExprSyntax getUnpackExpr();

   ArrayUnpackPairItemSyntax withEllipsisToken(std::optional<TokenSyntax> ellipsisToken);
   ArrayUnpackPairItemSyntax withUnpackExpr(std::optional<ExprSyntax> unpackExpr);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::ArrayUnpackPairItem;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class ArrayUnpackPairItemSyntaxBuilder;
   void validate();
};

///
/// array_pair_item:
///   array_key_value_pair_item
/// | array_unpack_pair_item
///
class ArrayPairItemSyntax final : public Syntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {

   };
};

///
/// simple_variable:
///   T_VARIABLE
/// | brace_decorated_variable_expr
/// | '$' simple_variable
///
class SimpleVariableExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 2;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: TokenSyntax
      /// optional: true
      ///
      DollarSign,
      ///
      /// type: Syntax
      /// optional: false
      /// node choices: true
      /// ----------------------------------------------
      /// node choice: TokenSyntax (T_VARIABLE)
      /// ----------------------------------------------
      /// node choice: BraceDecoratedVariableExprSyntax
      /// ----------------------------------------------
      /// node choice: SimpleVariableExprSyntax
      ///
      Variable,
   };

#ifdef POLAR_DEBUG_BUILD
   const static NodeChoicesType CHILD_NODE_CHOICES;
#endif

public:
   SimpleVariableExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   std::optional<TokenSyntax> getDollarSign();
   Syntax getVariable();

   SimpleVariableExprSyntax withDollarSign(std::optional<TokenSyntax> dollarSign);
   SimpleVariableExprSyntax withVariable(std::optional<Syntax> variable);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::SimpleVariableExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class SimpleVariableExprSyntaxBuilder;
   void validate();
};

///
/// array_expr:
///   T_ARRAY '(' array_pair_list ')'
///
class ArrayExprSyntax final : public ExprSyntax
{

};

///
/// simplified_array_expr:
/// '[' array_pair_list ']'
///
class SimplifiedArrayExprSyntax final : public ExprSyntax
{

};

class ClassRefParentExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      ParentKeyword,
   };

public:
   ClassRefParentExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getParentKeyword();
   ClassRefParentExprSyntax withParentKeyword(std::optional<TokenSyntax> parentKeyword);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::ClassRefParentExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class ClassRefParentExprBuilder;
   void validate();
};

class ClassRefSelfExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      SelfKeyword,
   };

public:
   ClassRefSelfExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getSelfKeyword();
   ClassRefSelfExprSyntax withSelfKeyword(std::optional<TokenSyntax> selfKeyword);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::ClassRefSelfExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class ClassRefSelfExprBuilder;
   void validate();
};

class ClassRefStaticExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      StaticKeyword,
   };
public:
   ClassRefStaticExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getStaticKeyword();
   ClassRefStaticExprSyntax withStaticKeyword(std::optional<TokenSyntax> staticKeyword);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::ClassRefStaticExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class ClassRefStaticExprBuilder;
   void validate();
};

class IntegerLiteralExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      Digits,
   };
public:
   IntegerLiteralExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getDigits();
   IntegerLiteralExprSyntax withDigits(std::optional<TokenSyntax> digits);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::IntegerLiteralExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class IntegerLiteralExprSyntaxBuilder;
   void validate();
};

class FloatLiteralExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      FloatDigits,
   };

public:
   FloatLiteralExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getFloatDigits();
   FloatLiteralExprSyntax withFloatDigits(std::optional<TokenSyntax> digits);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::FloatLiteralExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class FloatLiteralExprSyntaxBuilder;
   void validate();
};

///
/// string_literal:
///   '"' T_CONSTANT_ENCAPSED_STRING '"'
/// | '\'' T_CONSTANT_ENCAPSED_STRING '\''
///
class StringLiteralExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 3;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 3;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: TokenSyntax (T_SINGLE_QUOTE|T_DOUBLE_QUOTE)
      /// optional: false
      ///
      LeftQuote,
      ///
      /// type: TokenSyntax (T_CONSTANT_ENCAPSED_STRING)
      /// optional: false
      ///
      Text,
      ///
      /// type: TokenSyntax (T_SINGLE_QUOTE|T_DOUBLE_QUOTE)
      /// optional: false
      ///
      RightQuote
   };

#ifdef POLAR_DEBUG_BUILD
   const static TokenChoicesType CHILD_TOKEN_CHOICES;
#endif

public:
   StringLiteralExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getLeftQuote();
   TokenSyntax getText();
   TokenSyntax getRightQuote();

   StringLiteralExprSyntax withLeftQuote(std::optional<TokenSyntax> leftQuote);
   StringLiteralExprSyntax withText(std::optional<TokenSyntax> text);
   StringLiteralExprSyntax withRightQuote(std::optional<TokenSyntax> rightQuote);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::StringLiteralExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class StringLiteralExprSyntaxBuilder;
   void validate();
};

class BooleanLiteralExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      Boolean,
   };

#ifdef POLAR_DEBUG_BUILD
   ///
   /// Child name: Boolean
   /// Choices:
   /// TokenKindType::T_TRUE
   /// TokenKindType::T_FALSE
   ///
   static const TokenChoicesType CHILD_TOKEN_CHOICES;
#endif

public:
   BooleanLiteralExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getBooleanValue();

   /// Returns a copy of the receiver with its `Boolean` replaced.
   /// - param newChild: The new `Boolean` to replace the node's
   ///                   current `Boolean`, if present.
   BooleanLiteralExprSyntax withBooleanValue(std::optional<TokenSyntax> booleanValue);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::BooleanLiteralExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class BooleanLiteralExprSyntaxBuilder;
   void validate();
};

class TernaryExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 5;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 5;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: ExprSyntax
      /// optional: false
      ConditionExpr,
      /// type: TokenSyntax
      /// optional: false
      QuestionMark,
      /// type: ExprSyntax
      /// optional: false
      FirstChoice,
      /// type: TokenSyntax
      /// optional: false
      ColonMark,
      /// type: ExprSyntax
      /// optional: false
      SecondChoice
   };

public:
   TernaryExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   ExprSyntax getConditionExpr();
   TokenSyntax getQuestionMark();
   ExprSyntax getFirstChoice();
   TokenSyntax getColonMark();
   ExprSyntax getSecondChoice();

   /// Returns a copy of the receiver with its `ConditionExpr` replaced.
   /// - param newChild: The new `ConditionExpr` to replace the node's
   ///                   current `ConditionExpr`, if present.
   TernaryExprSyntax withConditionExpr(std::optional<ExprSyntax> conditionExpr);

   /// Returns a copy of the receiver with its `QuestionMark` replaced.
   /// - param newChild: The new `QuestionMark` to replace the node's
   ///                   current `QuestionMark`, if present.
   TernaryExprSyntax withQuestionMark(std::optional<TokenSyntax> questionMark);

   /// Returns a copy of the receiver with its `FirstChoice` replaced.
   /// - param newChild: The new `FirstChoice` to replace the node's
   ///                   current `FirstChoice`, if present.
   TernaryExprSyntax withFirstChoice(std::optional<ExprSyntax> firstChoice);

   /// Returns a copy of the receiver with its `ColonMark` replaced.
   /// - param newChild: The new `ColonMark` to replace the node's
   ///                   current `ColonMark`, if present.
   TernaryExprSyntax withColonMark(std::optional<TokenSyntax> colonMark);

   /// Returns a copy of the receiver with its `SecondChoice` replaced.
   /// - param newChild: The new `SecondChoice` to replace the node's
   ///                   current `SecondChoice`, if present.
   TernaryExprSyntax withSecondChoice(std::optional<ExprSyntax> secondChoice);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::TernaryExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class TernaryExprSyntaxBuilder;
   void validate();
};

class AssignmentExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      AssignToken
   };
public:
   AssignmentExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getAssignToken();

   /// Returns a copy of the receiver with its `AssignToken` replaced.
   /// - param newChild: The new `AssignToken` to replace the node's
   ///                   current `AssignToken`, if present.
   AssignmentExprSyntax withAssignToken(std::optional<TokenSyntax> assignToken);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::AssignmentExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class AssignmentExprSyntaxBuilder;
   void validate();
};

class SequenceExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: ExprListSyntax
      /// is_syntax_collection: true
      /// optional: false
      Elements
   };

public:
   SequenceExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   ExprListSyntax getElements();

   /// Adds the provided `Expr` to the node's `Elements`
   /// collection.
   /// - param element: The new `Expr` to add to the node's
   ///                  `Elements` collection.
   /// - returns: A copy of the receiver with the provided `Expr`
   ///            appended to its `Elements` collection.
   SequenceExprSyntax addElement(ExprSyntax expr);

   /// Returns a copy of the receiver with its `Elements` replaced.
   /// - param newChild: The new `Elements` to replace the node's
   ///                   current `Elements`, if present.
   SequenceExprSyntax withElements(std::optional<ExprListSyntax> elements);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::SequenceExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class SequenceExprSyntaxBuilder;
   void validate();
};

class PrefixOperatorExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 2;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: true
      OperatorToken,
      /// type: ExprSyntax
      /// optional: false
      Expr
   };

public:
   PrefixOperatorExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   std::optional<TokenSyntax> getOperatorToken();
   ExprSyntax getExpr();
   PrefixOperatorExprSyntax withOperatorToken(std::optional<TokenSyntax> operatorToken);
   PrefixOperatorExprSyntax withExpr(std::optional<TokenSyntax> expr);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::PrefixOperatorExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class PrefixOperatorExprSyntaxBuilder;
   void validate();
};

class PostfixOperatorExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 2;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 2;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: Expr
      /// optional: false
      Expr,
      /// type: TokenSyntax
      /// optional: false
      OperatorToken
   };

public:
   PostfixOperatorExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   ExprSyntax getExpr();
   TokenSyntax getOperatorToken();

   PostfixOperatorExprSyntax withExpr(std::optional<ExprSyntax> expr);
   PostfixOperatorExprSyntax withOperatorToken(std::optional<TokenSyntax> operatorToken);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::PostfixOperatorExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class PostfixOperatorExprSyntaxBuilder;
   void validate();
};

class BinaryOperatorExprSyntax final : public ExprSyntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 1;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      /// type: TokenSyntax
      /// optional: false
      OperatorToken
   };
public:
   BinaryOperatorExprSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : ExprSyntax(root, data)
   {
      validate();
   }

   TokenSyntax getOperatorToken();
   BinaryOperatorExprSyntax withOperatorToken(std::optional<TokenSyntax> operatorToken);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::BinaryOperatorExpr;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }

private:
   friend class BinaryOperatorExprSyntaxBuilder;
   void validate();
};

///
/// lexical_var:
///   T_VARIABLE
/// | '&' T_VARIABLE
///
class LexicalVarItemSyntax final : public Syntax
{
public:
   constexpr static std::uint8_t CHILDREN_COUNT = 2;
   constexpr static std::uint8_t REQUIRED_CHILDREN_COUNT = 1;
   enum Cursor : SyntaxChildrenCountType
   {
      ///
      /// type: TokenSyntax
      /// optional: true
      ///
      ReferenceToken,
      ///
      /// type: TokenSyntax
      /// optional: false
      ///
      Variable
   };
public:
   LexicalVarItemSyntax(const RefCountPtr<SyntaxData> root, const SyntaxData *data)
      : Syntax(root, data)
   {
      validate();
   }

   std::optional<TokenSyntax> getReferenceToken();
   TokenSyntax getVariable();
   LexicalVarItemSyntax withReferenceToken(std::optional<TokenSyntax> referenceToken);
   LexicalVarItemSyntax withVariable(std::optional<TokenSyntax> variable);

   static bool kindOf(SyntaxKind kind)
   {
      return kind == SyntaxKind::LexicalVarItem;
   }

   static bool classOf(const Syntax *syntax)
   {
      return kindOf(syntax->getKind());
   }
private:
   friend class LexicalVarItemSyntaxBuilder;
   void validate();
};

} // polar::syntax

#endif // POLARPHP_SYNTAX_SYNTAX_NODE_EXPR_NODES_H
