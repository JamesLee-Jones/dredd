// Copyright 2022 The Dredd Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libdredd/mutate_visitor.h"

#include <cassert>
#include <cstddef>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "libdredd/mutation_remove_statement.h"
#include "libdredd/mutation_replace_binary_operator.h"
#include "libdredd/mutation_replace_expr.h"
#include "libdredd/mutation_replace_unary_operator.h"
#include "libdredd/util.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Casting.h"

namespace dredd {

MutateVisitor::MutateVisitor(const clang::CompilerInstance& compiler_instance)
    : compiler_instance_(compiler_instance) {}

bool MutateVisitor::IsTypeSupported(const clang::QualType qual_type) {
  const auto* builtin_type = qual_type->getAs<clang::BuiltinType>();
  return builtin_type == nullptr ||
         !(builtin_type->isInteger() || builtin_type->isFloatingPoint());
}

bool MutateVisitor::IsInFunction() {
  // Walk up the next of enclosing declarations
  for (int index = static_cast<int>(enclosing_decls_.size()) - 1; index >= 0;
       index--) {
    const auto* decl = enclosing_decls_[static_cast<size_t>(index)];
    if (llvm::dyn_cast<clang::FunctionDecl>(decl) != nullptr) {
      // A function declaration has been found, either directly or only by going
      // via variable declarations. Thus the point of visitation is in a
      // function without any other intervening constructs.
      return true;
    }
    // It is OK if the visitation point is inside a variable declaration, as
    // long as that declaration turns out to be inside a function.
    if (llvm::dyn_cast<clang::VarDecl>(decl) == nullptr) {
      // The visitation point is inside some other declaration (e.g. a class).
      return false;
    }
  }
  // Global scope was reached without hitting a function, so the declaration is
  // not in a function.
  return false;
}

bool MutateVisitor::TraverseDecl(clang::Decl* decl) {
  if (llvm::dyn_cast<clang::TranslationUnitDecl>(decl) != nullptr) {
    // This is the top-level translation unit declaration, so descend into it.
    return RecursiveASTVisitor::TraverseDecl(decl);
  }
  auto source_range_in_main_file =
      GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(), *decl);
  if (source_range_in_main_file.isInvalid()) {
    // This declaration is not wholly contained in the main file, so do not
    // consider it for mutation.
    return true;
  }
  clang::BeforeThanCompare<clang::SourceLocation> comparator(
      compiler_instance_.getSourceManager());
  if (start_location_of_first_decl_in_source_file_.isInvalid() ||
      comparator(source_range_in_main_file.getBegin(),
                 start_location_of_first_decl_in_source_file_)) {
    // This is the first declaration wholly contained in the main file that has
    // been encountered so far: record it so that the dredd prelude can be
    // inserted before it.
    //
    // The order in which declarations appear in the source file may not exactly
    // match the order they are visited in the AST (for example, a typedef
    // declaration is visited after the associated type declaration, even though
    // the 'typedef' keyword occurs first in the AST), thus this location is
    // updated each time a declaration that appears earlier is encountered.
    start_location_of_first_decl_in_source_file_ =
        source_range_in_main_file.getBegin();
  }
  if (llvm::dyn_cast<clang::StaticAssertDecl>(decl) != nullptr) {
    // It does not make sense to mutate static assertions, as (a) this will
    // very likely lead to compile-time failures due to the assertion not
    // holding, (b) if compilation succeeds then the assertion is not actually
    // present at runtime so there is no notion of killing the mutant, and (c)
    // dynamic instrumentation of the mutation operator will break the rules
    // associated with static assertions anyway.
    return true;
  }
  if (const auto* var_decl = llvm::dyn_cast<clang::VarDecl>(decl)) {
    if (var_decl->isConstexpr()) {
      // Because Dredd's mutations occur dynamically, they cannot be applied to
      // C++ constexprs, which require compile-time evaluation.
      return true;
    }
    if (!compiler_instance_.getLangOpts().CPlusPlus &&
        var_decl->isStaticLocal()) {
      // In C, static local variables can only be initialized using constant
      // expressions, which require compile-time evaluation.
      return true;
    }
  }

  enclosing_decls_.push_back(decl);
  // Consider the declaration for mutation.
  RecursiveASTVisitor::TraverseDecl(decl);
  enclosing_decls_.pop_back();

  return true;
}

bool MutateVisitor::TraverseCaseStmt(clang::CaseStmt* case_stmt) {
  // Do not traverse the expression associated with this switch case: switch
  // case expressions need to be constant, which rules out the kinds of
  // mutations that Dredd performs.
  return TraverseStmt(case_stmt->getSubStmt());
}

bool MutateVisitor::TraverseConstantArrayTypeLoc(
    clang::ConstantArrayTypeLoc constant_array_type_loc) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameter.
  (void)this;
  (void)constant_array_type_loc;
  // Changing a constant-sized array to a non-constant-sized array is
  // problematic in C if the array has an initializer, and in C++ lambdas cannot
  // be used in array size expressions. For simplicity, don't try to mutate
  // constant array sizes.
  return true;
}

bool MutateVisitor::TraverseVariableArrayTypeLoc(
    clang::VariableArrayTypeLoc variable_array_type_loc) {
  if (compiler_instance_.getLangOpts().CPlusPlus) {
    // In C++, lambdas cannot appear in array sizes, so avoid mutating here.
    return true;
  }
  return RecursiveASTVisitor::TraverseVariableArrayTypeLoc(
      variable_array_type_loc);
}

bool MutateVisitor::TraverseTemplateArgumentLoc(
    clang::TemplateArgumentLoc template_argument_loc) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameter.
  (void)this;
  (void)template_argument_loc;
  // C++ template arguments typically need to be compile-time constants, and so
  // should not be mutated.
  return true;
}

bool MutateVisitor::VisitUnaryOperator(clang::UnaryOperator* unary_operator) {
  if (!IsInFunction()) {
    // Only consider mutating unary expressions that occur inside functions.
    return true;
  }

  // Check that the unary expression and its arguments have source
  // ranges that are part of the main file. In particular, this avoids
  // mutating expressions that directly involve the use of macros (though it
  // is OK if sub-expressions of arguments use macros).
  if (GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(),
                               *unary_operator)
          .isInvalid() ||
      GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(),
                               *unary_operator->getSubExpr())
          .isInvalid()) {
    return true;
  }

  // Don't mutate unary plus as this is unlikely to lead to a mutation that
  // differs from inserting a unary operator
  if (unary_operator->getOpcode() == clang::UnaryOperatorKind::UO_Plus) {
    return true;
  }

  // Check that the result type is supported
  if (IsTypeSupported(unary_operator->getType())) {
    return true;
  }
  if (IsTypeSupported(unary_operator->getSubExpr()->getType())) {
    return true;
  }

  // As it is not possible to pass bit-fields by reference, mutation of
  // bit-field l-values is not supported.
  if (unary_operator->getSubExpr()->refersToBitField()) {
    return true;
  }

  mutations_.push_back(
      std::make_unique<MutationReplaceUnaryOperator>(*unary_operator));
  return true;
}

bool MutateVisitor::VisitBinaryOperator(
    clang::BinaryOperator* binary_operator) {
  if (!IsInFunction()) {
    // Only consider mutating binary expressions that occur inside functions.
    return true;
  }

  // Check that the binary expression and its arguments have source
  // ranges that are part of the main file. In particular, this avoids
  // mutating expressions that directly involve the use of macros (though it
  // is OK if sub-expressions of arguments use macros).
  if (GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(),
                               *binary_operator)
          .isInvalid() ||
      GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(),
                               *binary_operator->getLHS())
          .isInvalid() ||
      GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(),
                               *binary_operator->getRHS())
          .isInvalid()) {
    return true;
  }

  // We only want to change operators for binary operations on basic types.
  // In particular, we do not want to mess with pointer arithmetic.
  // Check that the result type is supported
  if (IsTypeSupported(binary_operator->getType())) {
    return true;
  }
  if (IsTypeSupported(binary_operator->getLHS()->getType())) {
    return true;
  }
  if (IsTypeSupported(binary_operator->getRHS()->getType())) {
    return true;
  }

  // As it is not possible to pass bit-fields by reference, mutation of
  // bit-field l-values is not supported.
  if (binary_operator->getLHS()->refersToBitField()) {
    return true;
  }

  if (binary_operator->isCommaOp()) {
    // The comma operator is so versatile that it does not make a great deal of
    // sense to try to rewrite it.
    return true;
  }

  if (!compiler_instance_.getLangOpts().CPlusPlus) {
    if (binary_operator->isLogicalOp()) {
      // Due to problems with short-circuit evaluation, logical operators are
      // not mutated when handling C.
      return true;
    }
  }

  mutations_.push_back(
      std::make_unique<MutationReplaceBinaryOperator>(*binary_operator));
  return true;
}

bool MutateVisitor::VisitExpr(clang::Expr* expr) {
  if (!IsInFunction()) {
    // Only consider mutating expressions that occur inside functions.
    return true;
  }

  // Unary and binary operators are intercepted separately.
  if (llvm::dyn_cast<clang::BinaryOperator>(expr) != nullptr ||
      llvm::dyn_cast<clang::UnaryOperator>(expr) != nullptr) {
    return true;
  }

  // There is no useful way to mutate L-Value boolean expressions.
  if (expr->isLValue() && expr->getType()->isBooleanType()) {
    return true;
  }

  // We only want to mutate L-Values in C++ mode.
  if (expr->isLValue() &&
      !compiler_instance_.getASTContext().getLangOpts().CPlusPlus) {
    return true;
  }

  if (!(expr->getType()->isBooleanType() || expr->getType()->isIntegerType() ||
        expr->getType()->isFloatingType())) {
    return true;
  }

  mutations_.push_back(std::make_unique<MutationReplaceExpr>(*expr));
  return true;
}

bool MutateVisitor::VisitCompoundStmt(clang::CompoundStmt* compound_stmt) {
  for (auto* stmt : compound_stmt->body()) {
    if (GetSourceRangeInMainFile(compiler_instance_.getPreprocessor(), *stmt)
            .isInvalid() ||
        llvm::dyn_cast<clang::NullStmt>(stmt) != nullptr ||
        llvm::dyn_cast<clang::DeclStmt>(stmt) != nullptr ||
        llvm::dyn_cast<clang::SwitchCase>(stmt) != nullptr ||
        llvm::dyn_cast<clang::LabelStmt>(stmt) != nullptr) {
      // Wrapping switch cases, labels and null statements in conditional code
      // has no effect. Declarations cannot be wrapped in conditional code
      // without risking breaking compilation.
      continue;
    }
    assert(!enclosing_decls_.empty() &&
           "Statements can only be removed if they are nested in some "
           "declaration.");
    mutations_.push_back(std::make_unique<MutationRemoveStatement>(*stmt));
  }
  return true;
}

}  // namespace dredd
