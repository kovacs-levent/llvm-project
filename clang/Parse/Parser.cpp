//===--- Parser.cpp - C Language Family Parser ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Chris Lattner and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements the Parser interfaces.
//
//===----------------------------------------------------------------------===//

#include "clang/Parse/Parser.h"
#include "clang/Parse/SemaDeclSpec.h"
#include "clang/Parse/Scope.h"
using namespace llvm;
using namespace clang;

Parser::Parser(Preprocessor &pp, Action &actions)
  : PP(pp), Actions(actions), Diags(PP.getDiagnostics()) {
  Tok.setKind(tok::eof);
  CurScope = 0;
  
  ParenCount = BracketCount = BraceCount = 0;
}

///  Out-of-line virtual destructor to provide home for Action class.
Action::~Action() {}


void Parser::Diag(SourceLocation Loc, unsigned DiagID,
                  const std::string &Msg) {
  Diags.Report(Loc, DiagID, Msg);
}

/// MatchRHSPunctuation - For punctuation with a LHS and RHS (e.g. '['/']'),
/// this helper function matches and consumes the specified RHS token if
/// present.  If not present, it emits the specified diagnostic indicating
/// that the parser failed to match the RHS of the token at LHSLoc.  LHSName
/// should be the name of the unmatched LHS token.
SourceLocation Parser::MatchRHSPunctuation(tok::TokenKind RHSTok,
                                           SourceLocation LHSLoc) {
  
  if (Tok.getKind() == RHSTok)
    return ConsumeAnyToken();
    
  SourceLocation R = Tok.getLocation();
  const char *LHSName = "unknown";
  diag::kind DID = diag::err_parse_error;
  switch (RHSTok) {
  default: break;
  case tok::r_paren : LHSName = "("; DID = diag::err_expected_rparen; break;
  case tok::r_brace : LHSName = "{"; DID = diag::err_expected_rbrace; break;
  case tok::r_square: LHSName = "["; DID = diag::err_expected_rsquare; break;
  }
  Diag(Tok, DID);
  Diag(LHSLoc, diag::err_matching, LHSName);
  SkipUntil(RHSTok);
  return R;
}

/// ExpectAndConsume - The parser expects that 'ExpectedTok' is next in the
/// input.  If so, it is consumed and false is returned.
///
/// If the input is malformed, this emits the specified diagnostic.  Next, if
/// SkipToTok is specified, it calls SkipUntil(SkipToTok).  Finally, true is
/// returned.
bool Parser::ExpectAndConsume(tok::TokenKind ExpectedTok, unsigned DiagID,
                              const char *Msg, tok::TokenKind SkipToTok) {
  if (Tok.getKind() == ExpectedTok) {
    ConsumeAnyToken();
    return false;
  }
  
  Diag(Tok, DiagID, Msg);
  if (SkipToTok != tok::unknown)
    SkipUntil(SkipToTok);
  return true;
}

//===----------------------------------------------------------------------===//
// Error recovery.
//===----------------------------------------------------------------------===//

/// SkipUntil - Read tokens until we get to the specified token, then consume
/// it (unless DontConsume is false).  Because we cannot guarantee that the
/// token will ever occur, this skips to the next token, or to some likely
/// good stopping point.  If StopAtSemi is true, skipping will stop at a ';'
/// character.
/// 
/// If SkipUntil finds the specified token, it returns true, otherwise it
/// returns false.  
bool Parser::SkipUntil(tok::TokenKind T, bool StopAtSemi, bool DontConsume) {
  // We always want this function to skip at least one token if the first token
  // isn't T and if not at EOF.
  bool isFirstTokenSkipped = true;
  while (1) {
    // If we found the token, stop and return true.
    if (Tok.getKind() == T) {
      if (DontConsume) {
        // Noop, don't consume the token.
      } else {
        ConsumeAnyToken();
      }
      return true;
    }
    
    switch (Tok.getKind()) {
    case tok::eof:
      // Ran out of tokens.
      return false;
      
    case tok::l_paren:
      // Recursively skip properly-nested parens.
      ConsumeParen();
      SkipUntil(tok::r_paren, false);
      break;
    case tok::l_square:
      // Recursively skip properly-nested square brackets.
      ConsumeBracket();
      SkipUntil(tok::r_square, false);
      break;
    case tok::l_brace:
      // Recursively skip properly-nested braces.
      ConsumeBrace();
      SkipUntil(tok::r_brace, false);
      break;
      
    // Okay, we found a ']' or '}' or ')', which we think should be balanced.
    // Since the user wasn't looking for this token (if they were, it would
    // already be handled), this isn't balanced.  If there is a LHS token at a
    // higher level, we will assume that this matches the unbalanced token
    // and return it.  Otherwise, this is a spurious RHS token, which we skip.
    case tok::r_paren:
      if (ParenCount && !isFirstTokenSkipped)
        return false;  // Matches something.
      ConsumeParen();
      break;
    case tok::r_square:
      if (BracketCount && !isFirstTokenSkipped)
        return false;  // Matches something.
      ConsumeBracket();
      break;
    case tok::r_brace:
      if (BraceCount && !isFirstTokenSkipped)
        return false;  // Matches something.
      ConsumeBrace();
      break;
      
    case tok::string_literal:
    case tok::wide_string_literal:
      ConsumeStringToken();
      break;
    case tok::semi:
      if (StopAtSemi)
        return false;
      // FALL THROUGH.
    default:
      // Skip this token.
      ConsumeToken();
      break;
    }
    isFirstTokenSkipped = false;
  }  
}

//===----------------------------------------------------------------------===//
// Scope manipulation
//===----------------------------------------------------------------------===//

/// ScopeCache - Cache scopes to avoid malloc traffic.
static SmallVector<Scope*, 16> ScopeCache;

/// EnterScope - Start a new scope.
void Parser::EnterScope(unsigned ScopeFlags) {
  if (!ScopeCache.empty()) {
    Scope *N = ScopeCache.back();
    ScopeCache.pop_back();
    N->Init(CurScope, ScopeFlags);
    CurScope = N;
  } else {
    CurScope = new Scope(CurScope, ScopeFlags);
  }
}

/// ExitScope - Pop a scope off the scope stack.
void Parser::ExitScope() {
  assert(CurScope && "Scope imbalance!");

  // Inform the actions module that this scope is going away.
  Actions.PopScope(Tok.getLocation(), CurScope);
  
  Scope *Old = CurScope;
  CurScope = Old->getParent();
  
  if (ScopeCache.size() == 16)
    delete Old;
  else
    ScopeCache.push_back(Old);
}




//===----------------------------------------------------------------------===//
// C99 6.9: External Definitions.
//===----------------------------------------------------------------------===//

Parser::~Parser() {
  // If we still have scopes active, delete the scope tree.
  delete CurScope;
  
  // Free the scope cache.
  while (!ScopeCache.empty()) {
    delete ScopeCache.back();
    ScopeCache.pop_back();
  }
}

/// Initialize - Warm up the parser.
///
void Parser::Initialize() {
  // Prime the lexer look-ahead.
  ConsumeToken();
  
  // Create the global scope, install it as the current scope.
  assert(CurScope == 0 && "A scope is already active?");
  EnterScope(0);
  
  
  // Install builtin types.
  // TODO: Move this someplace more useful.
  {
    //__builtin_va_list
    DeclSpec DS;
    DS.StorageClassSpec = DeclSpec::SCS_typedef;
    
    // TODO: add a 'TST_builtin' type?
    DS.TypeSpecType = DeclSpec::TST_typedef;
    
    Declarator D(DS, Declarator::FileContext);
    D.SetIdentifier(PP.getIdentifierInfo("__builtin_va_list"),SourceLocation());
    Actions.ParseDeclarator(CurScope, D, 0, 0);
  }
  
  if (Tok.getKind() == tok::eof)  // Empty source file is an extension.
    Diag(diag::ext_empty_source_file);
}

/// ParseTopLevelDecl - Parse one top-level declaration, return whatever the
/// action tells us to.  This returns true if the EOF was encountered.
bool Parser::ParseTopLevelDecl(DeclTy*& Result) {
  Result = 0;
  if (Tok.getKind() == tok::eof) return true;
  
  Result = ParseExternalDeclaration();
  return false;
}

/// Finalize - Shut down the parser.
///
void Parser::Finalize() {
  ExitScope();
  assert(CurScope == 0 && "Scope imbalance!");
}

/// ParseTranslationUnit:
///       translation-unit: [C99 6.9]
///         external-declaration 
///         translation-unit external-declaration 
void Parser::ParseTranslationUnit() {
  Initialize();
  
  DeclTy *Res;
  while (!ParseTopLevelDecl(Res))
    /*parse them all*/;
  
  Finalize();
}

/// ParseExternalDeclaration:
///       external-declaration: [C99 6.9]
///         function-definition        [TODO]
///         declaration                [TODO]
/// [EXT]   ';'
/// [GNU]   asm-definition
/// [GNU]   __extension__ external-declaration     [TODO]
/// [OBJC]  objc-class-definition
/// [OBJC]  objc-class-declaration
/// [OBJC]  objc-alias-declaration
/// [OBJC]  objc-protocol-definition
/// [OBJC]  objc-method-definition
/// [OBJC]  @end
///
/// [GNU] asm-definition:
///         simple-asm-expr ';'
///
Parser::DeclTy *Parser::ParseExternalDeclaration() {
  switch (Tok.getKind()) {
  case tok::semi:
    Diag(diag::ext_top_level_semi);
    ConsumeToken();
    // TODO: Invoke action for top-level semicolon.
    return 0;
  case tok::kw_asm:
    ParseSimpleAsm();
    ExpectAndConsume(tok::semi, diag::err_expected_semi_after,
                     "top-level asm block");
    // TODO: Invoke action for top-level asm.
    return 0;
  case tok::at:
    ParseObjCAtDirectives();
    return 0;
  case tok::minus:
    ParseObjCInstanceMethodDeclaration();
    return 0;
  case tok::plus:
    ParseObjCClassMethodDeclaration();
    return 0;
  default:
    // We can't tell whether this is a function-definition or declaration yet.
    return ParseDeclarationOrFunctionDefinition();
  }
}

/// ParseDeclarationOrFunctionDefinition - Parse either a function-definition or
/// a declaration.  We can't tell which we have until we read up to the
/// compound-statement in function-definition.
///
///       function-definition: [C99 6.9.1]
///         declaration-specifiers[opt] declarator declaration-list[opt] 
///                 compound-statement                           [TODO]
///       declaration: [C99 6.7]
///         declaration-specifiers init-declarator-list[opt] ';' [TODO]
/// [!C99]  init-declarator-list ';'                             [TODO]
/// [OMP]   threadprivate-directive                              [TODO]
///
Parser::DeclTy *Parser::ParseDeclarationOrFunctionDefinition() {
  // Parse the common declaration-specifiers piece.
  DeclSpec DS;
  ParseDeclarationSpecifiers(DS);

  // C99 6.7.2.3p6: Handle "struct-or-union identifier;", "enum { X };"
  // declaration-specifiers init-declarator-list[opt] ';'
  if (Tok.getKind() == tok::semi) {
    // TODO: emit error on 'int;' or 'const enum foo;'.
    // if (!DS.isMissingDeclaratorOk()) Diag(...);
    
    ConsumeToken();
    // TODO: Return type definition.
    return 0;
  }
  
  // Parse the first declarator.
  Declarator DeclaratorInfo(DS, Declarator::FileContext);
  ParseDeclarator(DeclaratorInfo);
  // Error parsing the declarator?
  if (DeclaratorInfo.getIdentifier() == 0) {
    // If so, skip until the semi-colon or a }.
    SkipUntil(tok::r_brace, true);
    if (Tok.getKind() == tok::semi)
      ConsumeToken();
    return 0;
  }

  // If the declarator is the start of a function definition, handle it.
  if (Tok.getKind() == tok::equal ||  // int X()=  -> not a function def
      Tok.getKind() == tok::comma ||  // int X(),  -> not a function def
      Tok.getKind() == tok::semi  ||  // int X();  -> not a function def
      Tok.getKind() == tok::kw_asm || // int X() __asm__ -> not a fn def
      Tok.getKind() == tok::kw___attribute) {// int X() __attr__ -> not a fn def
    // FALL THROUGH.
  } else if (DeclaratorInfo.isFunctionDeclarator() &&
             (Tok.getKind() == tok::l_brace ||  // int X() {}
              isDeclarationSpecifier())) {      // int X(f) int f; {}
    return ParseFunctionDefinition(DeclaratorInfo);
  } else {
    if (DeclaratorInfo.isFunctionDeclarator())
      Diag(Tok, diag::err_expected_fn_body);
    else
      Diag(Tok, diag::err_expected_after_declarator);
    SkipUntil(tok::semi);
    return 0;
  }

  // Parse the init-declarator-list for a normal declaration.
  return ParseInitDeclaratorListAfterFirstDeclarator(DeclaratorInfo);
}

/// ParseFunctionDefinition - We parsed and verified that the specified
/// Declarator is well formed.  If this is a K&R-style function, read the
/// parameters declaration-list, then start the compound-statement.
///
///         declaration-specifiers[opt] declarator declaration-list[opt] 
///                 compound-statement                           [TODO]
///
Parser::DeclTy *Parser::ParseFunctionDefinition(Declarator &D) {
  const DeclaratorTypeInfo &FnTypeInfo = D.getTypeObject(0);
  assert(FnTypeInfo.Kind == DeclaratorTypeInfo::Function &&
         "This isn't a function declarator!");

  // FIXME: Enter a scope for the arguments.
  //EnterScope(Scope::FnScope);
  
  
  
  // If this declaration was formed with a K&R-style identifier list for the
  // arguments, parse declarations for all of the args next.
  // int foo(a,b) int a; float b; {}
  if (!FnTypeInfo.Fun.hasPrototype && !FnTypeInfo.Fun.isEmpty) {
    // Read all the argument declarations.
    while (isDeclarationSpecifier())
      ParseDeclaration(Declarator::KNRTypeListContext);
    
    // Note, check that we got them all.
  } else {
    //if (isDeclarationSpecifier())
    //  Diag('k&r declspecs with prototype?');
    
    // TODO: Install the arguments into the current scope.
  }

  // We should have an opening brace now.
  if (Tok.getKind() != tok::l_brace) {
    Diag(Tok, diag::err_expected_fn_body);

    // Skip over garbage, until we get to '{'.  Don't eat the '{'.
    SkipUntil(tok::l_brace, true, true);
    
    // If we didn't find the '{', bail out.
    if (Tok.getKind() != tok::l_brace)
      return 0;
  }
  
  // Parse the function body as a compound stmt.
  StmtResult FnBody = ParseCompoundStatement();
  if (FnBody.isInvalid) return 0;

  // FIXME: Leave the argument scope.
  // ExitScope();

  // TODO: Pass argument information.
  return Actions.ParseFunctionDefinition(CurScope, D, FnBody.Val);
}

/// ParseAsmStringLiteral - This is just a normal string-literal, but is not
/// allowed to be a wide string, and is not subject to character translation.
///
/// [GNU] asm-string-literal:
///         string-literal
///
void Parser::ParseAsmStringLiteral() {
  if (!isTokenStringLiteral()) {
    Diag(Tok, diag::err_expected_string_literal);
    return;
  }
  
  ExprResult Res = ParseStringLiteralExpression();
  if (Res.isInvalid) return;
  
  // TODO: Diagnose: wide string literal in 'asm'
}

/// ParseSimpleAsm
///
/// [GNU] simple-asm-expr:
///         'asm' '(' asm-string-literal ')'
///
void Parser::ParseSimpleAsm() {
  assert(Tok.getKind() == tok::kw_asm && "Not an asm!");
  ConsumeToken();
  
  if (Tok.getKind() != tok::l_paren) {
    Diag(Tok, diag::err_expected_lparen_after, "asm");
    return;
  }
  
  SourceLocation Loc = ConsumeParen();
  
  ParseAsmStringLiteral();
  
  MatchRHSPunctuation(tok::r_paren, Loc);
}

