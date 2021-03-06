#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <sstream>
#include <iostream>

#include "OMPToHClib.h"
#include "OMPDependencies.h"

extern TargetLang target;

#define VERBOSE

/*
 * NOTE: The one thing to be careful about whenever inserting code in an
 * existing function is that the brace inserter is not run before this pass
 * anymore. That means that extra care must be taken to ensure that converting a
 * single line of code to a multi-line block of code does not change the control
 * flow of the application. This should be kept in mind anywhere that an
 * InsertText, ReplaceText, etc API is called.
 *
 * NOTE: In general, each rewriter use is followed by an assertion that it
 * should not fail. Rewrites can fail if the target location is invalid, e.g.
 * the target location is in a macro. So if you start getting these assert
 * failures, it's usually simpler to change the application code.
 */

extern clang::FunctionDecl *curr_func_decl;
extern std::vector<clang::ValueDecl *> globals;

#define ASYNC_SUFFIX "_hclib_async"

static bool isGlobal(std::string varname) {
    for (std::vector<clang::ValueDecl *>::iterator ii = globals.begin(),
            ee = globals.end(); ii != ee; ii++) {
        clang::ValueDecl *var = *ii;
        if (var->getNameAsString() == varname) {
            return true;
        }
    }
    return false;
}

void OMPToHClib::setParent(const clang::Stmt *child,
        const clang::Stmt *parent) {
    assert(parentMap.find(child) == parentMap.end());
    parentMap[child] = parent;
}

const clang::Stmt *OMPToHClib::getParent(const clang::Stmt *s) {
    assert(parentMap.find(s) != parentMap.end());
    return parentMap.at(s);
}

std::string OMPToHClib::stmtToString(const clang::Stmt *stmt) {
    // std::string s;
    // llvm::raw_string_ostream stream(s);
    // stmt->printPretty(stream, NULL, Context->getPrintingPolicy());
    // stream.flush();
    // return s;
    return rewriter->getRewrittenText(stmt->getSourceRange());
}

void OMPToHClib::replaceAllReferencesTo(const clang::Stmt *stmt,
        std::vector<OMPVarInfo> *shared) {
    if (const clang::DeclRefExpr *ref = clang::dyn_cast<clang::DeclRefExpr>(stmt)) {
        if (sharedVarsReplaced.find(ref) == sharedVarsReplaced.end()) {
            for (std::vector<OMPVarInfo>::iterator i = shared->begin(),
                    e = shared->end(); i != e; i++) {
                OMPVarInfo info = *i;
                if (info.getDecl() == ref->getDecl() && !info.checkIsGlobal()) {
                    clang::PresumedLoc presumedStart = SM->getPresumedLoc(ref->getLocation());
                    std::stringstream loc;
                    loc << std::string(presumedStart.getFilename()) <<
                        ":" << presumedStart.getLine() << ":" <<
                        presumedStart.getColumn();

                    const bool failed = rewriter->ReplaceText(ref->getSourceRange(),
                            "(*(ctx->" + info.getDecl()->getNameAsString() +
                            "_ptr))");
                    if (failed) {
                        std::cerr << "Failed replacing \"" <<
                            info.getDecl()->getNameAsString() << "\" at " <<
                            loc.str() << ", likely inside of a macro." << std::endl;
                        exit(1);
                    }
                    sharedVarsReplaced.insert(ref);
                    break;
                }
            }
        }
    } else {
        std::vector<const clang::Stmt *> children;

        for (clang::Stmt::const_child_iterator i = stmt->child_begin(),
                e = stmt->child_end(); i != e; i++) {
            const clang::Stmt *child = *i;
            if (child != NULL) {
                children.push_back(child);
            }
        }
        for (std::vector<const clang::Stmt *>::reverse_iterator i =
                children.rbegin(), e = children.rend(); i != e; i++) {
            replaceAllReferencesTo(*i, shared);
        }
    }
}

std::string OMPToHClib::stmtToStringWithSharedVars(const clang::Stmt *stmt,
        std::vector<OMPVarInfo> *shared) {
    replaceAllReferencesTo(stmt, shared);
    return stmtToString(stmt);
}

std::string OMPToHClib::stringForAST(const clang::Stmt *stmt) {
    std::string s;
    llvm::raw_string_ostream stream(s);
    stmt->printPretty(stream, NULL, Context->getPrintingPolicy());
    stream.flush();
    return s;
}

void OMPToHClib::visitChildren(const clang::Stmt *s, bool firstTraversal) {
    for (clang::Stmt::const_child_iterator i = s->child_begin(),
            e = s->child_end(); i != e; i++) {
        const clang::Stmt *child = *i;
        if (child != NULL) {
            if (firstTraversal) {
                setParent(child, s);
            }
            VisitStmt(child);
        }
    }
}

std::string OMPToHClib::getDeclarationTypeStr(clang::QualType qualType,
        std::string name, std::string soFarBefore, std::string soFarAfter) {
    const clang::Type *type = qualType.getTypePtr();
    assert(type);

    if (const clang::ArrayType *arrayType = type->getAsArrayTypeUnsafe()) {
        if (const clang::ConstantArrayType *constantArrayType =
                clang::dyn_cast<clang::ConstantArrayType>(arrayType)) {
            std::string sizeStr = constantArrayType->getSize().toString(10,
                    true);
            return getDeclarationTypeStr(constantArrayType->getElementType(),
                    name, soFarBefore, soFarAfter + "[" + sizeStr + "]");
        } else if (const clang::ParenType *parenType =
                clang::dyn_cast<clang::ParenType>(arrayType)) {
            return getDeclarationTypeStr(parenType->getInnerType(), name,
                    soFarBefore + " (", ")" + soFarAfter);
        } else {
            std::cerr << "Unsupported array type " <<
                std::string(arrayType->getTypeClassName()) <<
                " while building " << "declaration for " << name << std::endl;
            exit(1);
        }
    } else if (const clang::PointerType *pointerType =
            type->getAs<clang::PointerType>()) {
        return getDeclarationTypeStr(pointerType->getPointeeType(), name,
                soFarBefore + "(*", ")" + soFarAfter);
    } else if (const clang::BuiltinType *builtinType =
            type->getAs<clang::BuiltinType>()) {
        std::string decl = builtinType->getName(Context->getPrintingPolicy()).str() + " " + soFarBefore + name + soFarAfter;
        if (qualType.isConstQualified()) {
            return "const " + decl;
        } else {
            return decl;
        }
    } else if (const clang::TypedefType *typedefType =
            type->getAs<clang::TypedefType>()) {
        std::cerr << "typedef type for " << name << std::endl;
        return typedefType->getDecl()->getNameAsString() + " " + soFarBefore +
            name + soFarAfter;
    } else if (const clang::ElaboratedType *elaboratedType =
            type->getAs<clang::ElaboratedType>()) {
        return elaboratedType->getNamedType().getAsString() + " " +
            soFarBefore + name + soFarAfter;
    } else if (const clang::TagType *tagType = type->getAs<clang::TagType>()) {
        clang::TagDecl *decl = tagType->getDecl();

        std::string tagName;
        if (tagType->getDecl()->getTypedefNameForAnonDecl()) {
            tagName = decl->getTypedefNameForAnonDecl()->getNameAsString();
        } else if (tagType->getDecl()->getDeclName()) {
            tagName = decl->getDeclName().getAsString() ;
        } else {
            assert(false);
        }

        return tagName + " " + soFarBefore + name + soFarAfter;
    } else {
        std::cerr << "getDeclarationTypeStr: Unsupported type " <<
            std::string(type->getTypeClassName()) << std::endl;
        exit(1);
    }
}

std::string OMPToHClib::getDeclarationStr(clang::ValueDecl *decl) {
    return getDeclarationTypeStr(decl->getType(), decl->getNameAsString(), "", "") + ";";
}

std::string OMPToHClib::getArraySizeExpr(clang::QualType qualType) {
    const clang::Type *type = qualType.getTypePtr();
    assert(type);

    if (const clang::ArrayType *arrayType = type->getAsArrayTypeUnsafe()) {
        std::string sizeStr;
        if (const clang::ConstantArrayType *constantArrayType =
                clang::dyn_cast<clang::ConstantArrayType>(arrayType)) {
            sizeStr = constantArrayType->getSize().toString(10, true);
        } else {
            std::cerr << "Unsupported array type " <<
                std::string(type->getTypeClassName()) << std::endl;
            exit(1);
        }
        return sizeStr + " * (" + getArraySizeExpr(arrayType->getElementType()) + ")";
    } else if (const clang::BuiltinType *builtinType = type->getAs<clang::BuiltinType>()) {
        return "sizeof(" + qualType.getAsString() + ")";
    } else if (const clang::DecayedType *decayedType = type->getAs<clang::DecayedType>()) {
        return getArraySizeExpr(decayedType->getOriginalType());
    } else if (const clang::TypedefType *typedefType = type->getAs<clang::TypedefType>()) {
        return "sizeof(" + qualType.getAsString() + ")";
    } else {
        std::cerr << "getArraySizeExpr: Unsupported type while getting array size expression " <<
            std::string(type->getTypeClassName()) << std::endl;
        exit(1);
    }
}

std::string OMPToHClib::getUnpackStr(clang::ValueDecl *decl) {
    const clang::Type *type = decl->getType().getTypePtr();
    assert(type);

    std::stringstream ss;
    ss << getDeclarationTypeStr(decl->getType(), decl->getNameAsString(), "",
            "") << "; ";

    if (const clang::ArrayType *arrayType = type->getAsArrayTypeUnsafe()) {
        std::string arraySizeExpr = getArraySizeExpr(decl->getType());
        ss << "memcpy(" << decl->getNameAsString() << ", ctx->" <<
            decl->getNameAsString() << ", " << arraySizeExpr << "); ";
    } else if (type->getAs<clang::PointerType>() ||
            type->getAs<clang::BuiltinType>() ||
            type->getAs<clang::TypedefType>() ||
            type->getAs<clang::ElaboratedType>() ||
            type->getAs<clang::TagType>()) {
        ss << decl->getNameAsString() << " = ctx->" << decl->getNameAsString() << ";";
    } else {
        std::cerr << "getUnpackStr: Unsupported type " << std::string(type->getTypeClassName()) << std::endl;
        exit(1);
    }

    ss << std::flush;
    return ss.str();
}

std::string OMPToHClib::getCaptureStr(clang::ValueDecl *decl) {
    const clang::Type *type = decl->getType().getTypePtr();
    assert(type);

    std::stringstream ss;

    if (type->getAs<clang::DecayedType>()) {
        type = type->getAs<clang::DecayedType>()->getPointeeType().getTypePtr();
    }

    if (const clang::ArrayType *arrayType = type->getAsArrayTypeUnsafe()) {
        std::string arraySizeExpr = getArraySizeExpr(decl->getType());
        ss << "memcpy(new_ctx->" << decl->getNameAsString() << ", " <<
            decl->getNameAsString() << ", " << arraySizeExpr << "); ";
    } else if (type->getAs<clang::PointerType>() ||
            type->getAs<clang::BuiltinType>() ||
            type->getAs<clang::TypedefType>() ||
            type->getAs<clang::ElaboratedType>() ||
            type->getAs<clang::TagType>()) {
        ss << "new_ctx->" << decl->getNameAsString() << " = " <<
            decl->getNameAsString() << ";";
    } else {
        std::cerr << "Unsupported type " <<
            std::string(type->getTypeClassName()) << std::endl;
        exit(1);
    }

    ss << std::flush;
    return ss.str();
}

std::string OMPToHClib::getStructDef(std::string structName,
        std::vector<clang::ValueDecl *> *captured, OMPClauses *clauses) {
    std::vector<OMPVarInfo> *vars = clauses->getVarInfo(captured);

    std::stringstream ss;
    ss << "typedef struct _" << structName << " {" << std::endl;

    for (std::vector<OMPVarInfo>::iterator i = vars->begin(), e = vars->end();
            i != e; i++) {
        OMPVarInfo var = *i;
        clang::ValueDecl *decl = var.getDecl();

        switch (var.getType()) {
            case (CAPTURE_TYPE::SHARED):
                if (!var.checkIsGlobal()) {
                    ss << "    " << getDeclarationTypeStr(
                            Context->getPointerType(decl->getType()),
                            decl->getNameAsString() + "_ptr",
                            "", "") << ";" << std::endl;
                }
                break;
            case (CAPTURE_TYPE::PRIVATE):
            case (CAPTURE_TYPE::FIRSTPRIVATE):
                ss << "    " << getDeclarationStr(decl) << std::endl;
                break;
            case (CAPTURE_TYPE::LASTPRIVATE):
                ss << "    " << getDeclarationStr(decl) << std::endl;
                ss << "    " << getDeclarationTypeStr(
                        Context->getPointerType(decl->getType()),
                        decl->getNameAsString() + "_ptr", "", "") << ";" <<
                    std::endl;
                break;
            default:
                std::cerr << "Unsupported capture type" << std::endl;
                exit(1);
        }
    }

    if (clauses->getReductions()->size() > 0) {
        ss << "    pthread_mutex_t reduction_mutex;" << std::endl;
    }

    ss << " } " << structName << ";" << std::endl << std::endl;

    return ss.str();
}

void OMPToHClib::preFunctionVisit(clang::FunctionDecl *func) {
    assert(getCurrentLexicalDepth() == 0);
    addNewScope();

    for (int i = 0; i < func->getNumParams(); i++) {
        clang::ParmVarDecl *param = func->getParamDecl(i);
        addToCurrentScope(param);
    }

    clang::PresumedLoc presumedStart = SM->getPresumedLoc(
            func->getLocStart());
    const int functionStartLine = presumedStart.getLine();
    pragmaTree = new PragmaNode(NULL, func->getBody(),
            new std::vector<clang::ValueDecl *>(), SM);
}

clang::Expr *OMPToHClib::unwrapCasts(clang::Expr *expr) {
    while (clang::isa<clang::ImplicitCastExpr>(expr)) {
        expr = clang::dyn_cast<clang::ImplicitCastExpr>(expr)->getSubExpr();
    }
    return expr;
}

std::string OMPToHClib::getCondVarAndLowerBoundFromInit(const clang::Stmt *init,
        const clang::ValueDecl **condVar) {
    if (const clang::BinaryOperator *bin =
            clang::dyn_cast<clang::BinaryOperator>(init)) {
        clang::Expr *lhs = bin->getLHS();
        if (const clang::DeclRefExpr *declref =
                clang::dyn_cast<clang::DeclRefExpr>(lhs)) {
            *condVar = declref->getDecl();
            return stmtToString(bin->getRHS());
        } else {
            std::cerr << "LHS is unsupported class " <<
                std::string(lhs->getStmtClassName()) << std::endl;
            exit(1);
        }
    } else if (const clang::DeclStmt *decl = clang::dyn_cast<clang::DeclStmt>(init)) {
        assert(decl->isSingleDecl());

        const clang::ValueDecl *val = clang::dyn_cast<clang::ValueDecl>(decl->getSingleDecl());
        assert(val);
        *condVar = val;

        const clang::VarDecl *var = clang::dyn_cast<clang::VarDecl>(val);
        assert(var);
        assert(var->hasInit());
        const clang::Expr *initialValue = var->getInit();
        return stmtToString(initialValue);
    } else {
        std::cerr << "Unsupported init class " <<
            std::string(init->getStmtClassName()) << std::endl;
        exit(1);
    }
}

std::string OMPToHClib::getUpperBoundFromCond(const clang::Stmt *cond,
        const clang::ValueDecl *condVar) {
    if (const clang::BinaryOperator *bin = clang::dyn_cast<clang::BinaryOperator>(cond)) {
        clang::Expr *lhs = unwrapCasts(bin->getLHS());
        const clang::DeclRefExpr *declref = clang::dyn_cast<clang::DeclRefExpr>(lhs);
        assert(declref);
        const clang::ValueDecl *decl = declref->getDecl();
        assert(condVar == decl);

        if (bin->getOpcode() == clang::BO_LT) {
            return stringForAST(bin->getRHS());
        } else if (bin->getOpcode() == clang::BO_LE) {
            return "(" + stringForAST(bin->getRHS()) + ") + 1"; // assumes integral values?
        } else {
            std::cerr << "Unsupported binary operator " << bin->getOpcode() << std::endl;
            exit(1);
        }
    } else {
        std::cerr << "Unsupported cond class " <<
            std::string(cond->getStmtClassName()) <<
            std::endl;
        exit(1);
    }
}

std::string OMPToHClib::getStrideFromIncr(const clang::Stmt *inc,
        const clang::ValueDecl *condVar) {
    if (const clang::UnaryOperator *uno = clang::dyn_cast<clang::UnaryOperator>(inc)) {
        clang::Expr *target = unwrapCasts(uno->getSubExpr());
        const clang::DeclRefExpr *declref = clang::dyn_cast<clang::DeclRefExpr>(target);
        assert(declref);
        const clang::ValueDecl *decl = declref->getDecl();
        assert(condVar == decl);

        if (uno->getOpcode() == clang::UO_PostInc ||
                uno->getOpcode() == clang::UO_PreInc) {
            return "1";
        } else if (uno->getOpcode() == clang::UO_PostDec ||
                uno->getOpcode() == clang::UO_PreDec) {
            return "-1";
        } else {
            std::cerr << "Unsupported unary operator" << std::endl;
            exit(1);
        }
    } else if (const clang::BinaryOperator *bin = clang::dyn_cast<clang::BinaryOperator>(inc)) {
        if (bin->getOpcode() == clang::BO_Assign) {
            clang::Expr *target = unwrapCasts(bin->getLHS());
            const clang::DeclRefExpr *declref = clang::dyn_cast<clang::DeclRefExpr>(target);
            assert(declref);
            assert(condVar == declref->getDecl());

            clang::Expr *rhs = unwrapCasts(bin->getRHS());
            if (const clang::BinaryOperator *rhsBin = clang::dyn_cast<clang::BinaryOperator>(rhs)) {
                clang::Expr *lhs = unwrapCasts(rhsBin->getLHS());
                clang::Expr *rhs = unwrapCasts(rhsBin->getRHS());
                clang::Expr *other = NULL;

                clang::DeclRefExpr *declref = NULL;
                if (declref = clang::dyn_cast<clang::DeclRefExpr>(lhs)) {
                    other = rhs;
                } else if (declref = clang::dyn_cast<clang::DeclRefExpr>(rhs)) {
                    other = lhs;
                } else {
                    std::cerr << "Neither of the inputs to the RHS binary " <<
                        "op for an incr clause are a decl ref" << std::endl;
                    exit(1);
                }

                assert(condVar == declref->getDecl());

                if (rhsBin->getOpcode() == clang::BO_Add) {
                    if (const clang::IntegerLiteral *literal =
                            clang::dyn_cast<clang::IntegerLiteral>(other)) {
                        return stmtToString(literal);
                    } else {
                        std::cerr << "unsupported other is a " <<
                            other->getStmtClassName() << std::endl;
                        exit(1);
                    }
                } else {
                    std::cerr << "Unsupported binary operator on RHS of " <<
                        "binary incr clause" << std::endl;
                    exit(1);
                }
            } else {
                std::cerr << "Unsupported RHS to binary incr clause: " <<
                    rhs->getStmtClassName() << std::endl;
                exit(1);
            }
        } else {
            std::cerr << "Unsupported binary "
                "operator in increment clause" << std::endl;
            exit(1);
        }
    } else {
        std::cerr << "Unsupported incr class " <<
            std::string(inc->getStmtClassName()) <<
            std::endl;
        exit(1);
    }
}

void OMPToHClib::traverseFunctorBody(const clang::Stmt *curr,
        ParallelRegionInfo &acc, bool beneathFunctionCall) {
#ifdef VERBOSE
    std::cerr << "traverseFunctorBody: " << curr->getStmtClassName() <<
        std::endl;
#endif

    if (const clang::CompoundStmt *cmpd =
            clang::dyn_cast<clang::CompoundStmt>(curr)) {
        for (clang::CompoundStmt::const_body_iterator i = cmpd->body_begin(),
                e = cmpd->body_end(); i != e; i++) {
            const clang::Stmt *member = *i;
            traverseFunctorBody(member, acc, beneathFunctionCall);
        }
    } else if (const clang::BinaryOperator *bin =
            clang::dyn_cast<clang::BinaryOperator>(curr)) {
        traverseFunctorBody(bin->getLHS(), acc, beneathFunctionCall);
        traverseFunctorBody(bin->getRHS(), acc, beneathFunctionCall);
    } else if (const clang::DeclRefExpr *declref =
            clang::dyn_cast<clang::DeclRefExpr>(curr)) {
        if (beneathFunctionCall) {
            /*
             * Escaped to a different scope, so only worry about capturing
             * global variables in the functor here.
             */
            if (isGlobal(declref->getDecl()->getNameAsString())) {
                acc.addReferencedVar(declref->getDecl());
            }
        } else {
            /*
             * This could add referenced variables that are declared from within
             * the parallel region itself. When generating the functor we can
             * ignore any referenced variables that are not in the capture list
             * of the pragma and assume they are declared within the functor,
             * though this is slightly riskier code.
             */
            acc.addReferencedVar(declref->getDecl());
        }
    } else if (const clang::CastExpr *cast =
            clang::dyn_cast<clang::CastExpr>(curr)) {
        traverseFunctorBody(cast->getSubExpr(), acc, beneathFunctionCall);
    } else if (const clang::ArraySubscriptExpr *arrayRef =
            clang::dyn_cast<clang::ArraySubscriptExpr>(curr)) {
        /*
         * Traverse the base variable reference this array reference is relative
         * to.
         */
        traverseFunctorBody(arrayRef->getBase(), acc, beneathFunctionCall);
        // Traverse the expression of the offset from base.
        traverseFunctorBody(arrayRef->getIdx(), acc, beneathFunctionCall);
    } else if (const clang::ForStmt *loop =
            clang::dyn_cast<clang::ForStmt>(curr)) {
        traverseFunctorBody(loop->getInit(), acc, beneathFunctionCall);
        traverseFunctorBody(loop->getCond(), acc, beneathFunctionCall);
        traverseFunctorBody(loop->getInc(), acc, beneathFunctionCall);
        traverseFunctorBody(loop->getBody(), acc, beneathFunctionCall);
    } else if (const clang::UnaryOperator *unary =
            clang::dyn_cast<clang::UnaryOperator>(curr)) {
        traverseFunctorBody(unary->getSubExpr(), acc, beneathFunctionCall);
    } else if (const clang::IfStmt *branch =
            clang::dyn_cast<clang::IfStmt>(curr)) {
        traverseFunctorBody(branch->getCond(), acc, beneathFunctionCall);
        if (branch->getThen()) {
            traverseFunctorBody(branch->getThen(), acc, beneathFunctionCall);
        }
        if (branch->getElse()) {
            traverseFunctorBody(branch->getElse(), acc, beneathFunctionCall);
        }
    } else if (const clang::ParenExpr *paren =
            clang::dyn_cast<clang::ParenExpr>(curr)) {
        traverseFunctorBody(paren->getSubExpr(), acc, beneathFunctionCall);
    } else if (const clang::MemberExpr *member =
            clang::dyn_cast<clang::MemberExpr>(curr)) {
        traverseFunctorBody(member->getBase(), acc, beneathFunctionCall);
    } else if (const clang::CallExpr *call =
            clang::dyn_cast<clang::CallExpr>(curr)) {
        const clang::FunctionDecl *callee = call->getDirectCallee();
        assert(callee != NULL);

        if (callee->getBody() == NULL) {
            acc.foundUnresolvedFunction(callee);
        } else {
            if (std::find(acc.called_begin(), acc.called_end(), callee) ==
                    acc.called_end()) {
                acc.addCalledFunction(callee);
                traverseFunctorBody(callee->getBody(), acc, true);
            }
        }

        for (unsigned i = 0; i < call->getNumArgs(); i++) {
            traverseFunctorBody(call->getArg(i), acc, beneathFunctionCall);
        }
    } else if (const clang::ReturnStmt *ret =
            clang::dyn_cast<clang::ReturnStmt>(curr)) {
        traverseFunctorBody(ret->getRetValue(), acc, beneathFunctionCall);
    } else if (const clang::ConditionalOperator *cond =
            clang::dyn_cast<clang::ConditionalOperator>(curr)) {
        traverseFunctorBody(cond->getCond(), acc, beneathFunctionCall);
        traverseFunctorBody(cond->getTrueExpr(), acc, beneathFunctionCall);
        traverseFunctorBody(cond->getFalseExpr(), acc, beneathFunctionCall);
    } else if (const clang::CXXConstructExpr *constr =
            clang::dyn_cast<clang::CXXConstructExpr>(curr)) {
        for (unsigned i = 0; i < constr->getNumArgs(); i++) {
            traverseFunctorBody(constr->getArg(i), acc, beneathFunctionCall);
        }
    } else if (const clang::DeclStmt *decl =
            clang::dyn_cast<clang::DeclStmt>(curr)) {
        for (clang::DeclStmt::const_decl_iterator i = decl->decl_begin(),
                e = decl->decl_end(); i != e; i++) {
            const clang::Decl *curr = *i;
            acc.addDeclaredVar(curr);

            if (const clang::VarDecl *var =
                    clang::dyn_cast<clang::VarDecl>(curr)) {
                if (var->getInit()) {
                    traverseFunctorBody(var->getInit(), acc,
                            beneathFunctionCall);
                }
            }
        }
    } else if (const clang::UnaryExprOrTypeTraitExpr *unary =
            clang::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(curr)) {
        if (!unary->isArgumentType()) {
            traverseFunctorBody(unary->getArgumentExpr(), acc,
                    beneathFunctionCall);
        }
    } else if (const clang::InitListExpr *initList =
            clang::dyn_cast<clang::InitListExpr>(curr)) {
        for (unsigned i = 0; i < initList->getNumInits(); i++) {
            traverseFunctorBody(initList->getInit(i), acc,
                    beneathFunctionCall);
        }
    } else if (clang::isa<clang::IntegerLiteral>(curr) ||
            clang::isa<clang::FloatingLiteral>(curr) ||
            clang::isa<clang::BreakStmt>(curr) ||
            clang::isa<clang::CXXBoolLiteralExpr>(curr) ||
            clang::isa<clang::ContinueStmt>(curr)) {
        // Do nothing
    } else {
        std::cerr << "traverseFunctorBody: Unsupported statement type: " <<
            curr->getStmtClassName() << std::endl;
        exit(1);
    }
}

bool OMPToHClib::handleCapturedPointer(const clang::PointerType *pointer,
        const clang::ValueDecl *ref, OMPVarInfo &foundVarInfo,
        const bool isShared, std::stringstream &constructor_sig,
        std::stringstream &constructor_body, std::stringstream &ss,
        CUDAFunctorParameters *functor_params,
        std::vector<OMPVarInfo> *toBeTransferred) {
    const clang::Type *pointee = pointer->getPointeeType().getTypePtr();

    if (const clang::BuiltinType *builtinPointee =
            clang::dyn_cast<clang::BuiltinType>(pointee)) {
        const std::string builtinStr = builtinPointee->getName(
                Context->getPrintingPolicy()).str();
        ss << "    " << builtinStr << "* " << (isShared ? "volatile " : "") <<
            ref->getNameAsString() << ";" << std::endl;
        ss << "    " << builtinStr << "* " << (isShared ? "volatile " : "") <<
            "h_" << ref->getNameAsString() << ";" << std::endl;
        constructor_sig << builtinStr << "* set_" << ref->getNameAsString();
        functor_params->addParameter(ref, foundVarInfo);
        constructor_body << "            h_" << ref->getNameAsString() <<
            " = set_" << ref->getNameAsString() << ";" << std::endl;
    } else if (const clang::TypedefType *typdef =
            clang::dyn_cast<clang::TypedefType>(pointee)) {
        const clang::Type *nested =
            typdef->getDecl()->getTypeSourceInfo()->getType().getTypePtr();

        if (const clang::ElaboratedType *elaborated =
                clang::dyn_cast<clang::ElaboratedType>(nested)) {
            const clang::Type *nested = elaborated->getNamedType().getTypePtr();
            if (const clang::RecordType *record =
                    clang::dyn_cast<clang::RecordType>(nested)) {
                if (checkForPointersInRecord(record)) {
                    std::cerr << "getCUDAFunctorDef: Found captured record " <<
                        "type \"" << record->getDecl()->getNameAsString() <<
                        "\" has pointer fields." << std::endl;
                    return false;
                }

                const std::string typdefStr =
                    typdef->getDecl()->getNameAsString();
                ss << "    " << typdefStr << "* " <<
                    (isShared ? "volatile " : "") << ref->getNameAsString() <<
                    ";" << std::endl;
                ss << "    " << typdefStr << "* " <<
                    (isShared ? "volatile " : "") << "h_" <<
                    ref->getNameAsString() << ";" << std::endl;
                constructor_sig << typdefStr << "* set_" <<
                    ref->getNameAsString();
                functor_params->addParameter(ref, foundVarInfo);
                constructor_body << "            h_" <<
                    ref->getNameAsString() << " = set_" <<
                    ref->getNameAsString() << ";" << std::endl;
            } else {
                std::cerr << "getCUDAFunctorDef: " <<
                    "Unsupported elaborated pointee type " <<
                    std::string(nested->getTypeClassName()) <<
                    " for " << ref->getNameAsString() << std::endl;
                exit(1);
            }
        } else if (const clang::BuiltinType *builtin =
                clang::dyn_cast<clang::BuiltinType>(nested)) {
            const std::string typdefStr =
                typdef->getDecl()->getNameAsString();
            ss << "    " << typdefStr << "* " <<
                (isShared ? "volatile " : "") << ref->getNameAsString() <<
                ";" << std::endl;
            ss << "    " << typdefStr << "* " <<
                (isShared ? "volatile " : "") << "h_" <<
                ref->getNameAsString() << ";" << std::endl;
            constructor_sig << typdefStr << "* set_" <<
                ref->getNameAsString();
            functor_params->addParameter(ref, foundVarInfo);
            constructor_body << "            h_" << ref->getNameAsString() <<
                " = set_" << ref->getNameAsString() << ";" << std::endl;
        } else {
            std::cerr << "getCUDAFunctorDef: " <<
                "Unsupported typedef pointee type " <<
                std::string(nested->getTypeClassName()) <<
                " for " << ref->getNameAsString() << std::endl;
            exit(1);
        }
    } else if (const clang::RecordType *record =
            clang::dyn_cast<clang::RecordType>(pointee)) {
        if (checkForPointersInRecord(record)) {
            std::cerr << "getCUDAFunctorDef: Found captured record " <<
                "type \"" << record->getDecl()->getNameAsString() <<
                "\" has pointer fields." << std::endl;
            return false;
        }

        ss << "    struct " << record->getDecl()->getNameAsString() << "* " <<
            (isShared ? "volatile" : "") << ref->getNameAsString() << ";" <<
            std::endl;
        ss << "    struct " << record->getDecl()->getNameAsString() << "* " <<
            (isShared ? "volatile" : "") << "h_" << ref->getNameAsString() <<
            ";" << std::endl;
        constructor_sig << "struct " << record->getDecl()->getNameAsString() <<
            "* set_" << ref->getNameAsString();
        functor_params->addParameter(ref, foundVarInfo);
        constructor_body << "            h_" << ref->getNameAsString() <<
            " = set_" << ref->getNameAsString() << ";" << std::endl;
    } else {
        std::cerr << "getCUDAFunctorDef: Unsupported " <<
            "pointee type " <<
            std::string(pointee->getTypeClassName()) <<
            " for " << ref->getNameAsString() << std::endl;
        return false;
        // exit(1);
    }

    toBeTransferred->push_back(foundVarInfo);
    return true;
}

bool OMPToHClib::checkForPointersInRecord(const clang::RecordType *record) {
    clang::RecordDecl *decl = record->getDecl();

    for (clang::RecordDecl::field_iterator i = decl->field_begin(),
            e = decl->field_end(); i != e; i++) {
        const clang::FieldDecl *field = *i;
        const clang::Type *fieldType = field->getType().getTypePtr();
        if (clang::isa<clang::PointerType>(fieldType)) {
            return true;
        } else if (const clang::RecordType *nested =
                clang::dyn_cast<clang::RecordType>(fieldType)) {
            if (checkForPointersInRecord(nested)) {
                return true;
            }
        }
    }

    return false;
}

std::string OMPToHClib::getCUDAFunctorDef(std::string closureName,
        std::vector<clang::ValueDecl *> *captured, OMPClauses *clauses,
        std::string iterator, std::string bodyStr, const clang::Stmt *body,
        CUDAFunctorParameters *functor_params) {
    std::vector<OMPVarInfo> *vars = clauses->getVarInfo(captured);
    std::stringstream ss;
    bool first_field = true;
    std::stringstream constructor_sig;
    std::stringstream constructor_body;
    
    std::vector<std::string> cudaIntrinsics;
    cudaIntrinsics.push_back("exp");
    cudaIntrinsics.push_back("sqrt");
    cudaIntrinsics.push_back("cos");
    cudaIntrinsics.push_back("sin");
    cudaIntrinsics.push_back("fabs");
    cudaIntrinsics.push_back("pow");
    cudaIntrinsics.push_back("log");
    cudaIntrinsics.push_back("hclib_get_current_worker");

#ifdef VERBOSE
    std::cerr << "getCUDAFunctorDef: Generating " << closureName << std::endl;
#endif

    ParallelRegionInfo info;
    traverseFunctorBody(body, info, false);

    bool anyUnresolved = false;
    for (std::vector<const clang::FunctionDecl *>::iterator i =
            info.unresolved_begin(), e = info.unresolved_end(); i != e; i++) {
        const clang::FunctionDecl *unresolved = *i;
        std::string unresolvedName = unresolved->getNameAsString();
        if (std::find(cudaIntrinsics.begin(), cudaIntrinsics.end(),
                    unresolvedName) == cudaIntrinsics.end()) {
            anyUnresolved = true;
            std::cerr << "Found call to unresolved function \"" <<
                unresolved->getNameAsString() << "\" inside acceleratable " <<
                "parallel region" << std::endl;
        }
    }

    if (anyUnresolved) {
        return "";
    }

    std::vector<OMPVarInfo> *toBeTransferred = new std::vector<OMPVarInfo>();

    constructor_sig << "        " << closureName << "(";

    ss << "class " << closureName << " {\n";
    ss << "    private:\n";
    ss << "        void **host_allocations;" << std::endl;
    ss << "        size_t *host_allocation_sizes;" << std::endl;
    ss << "        unsigned nallocations;" << std::endl;
    ss << "        void **device_allocations;" << std::endl;

    for (std::vector<const clang::FunctionDecl *>::iterator i =
            info.called_begin(), e = info.called_end(); i != e; i++) {
        const clang::FunctionDecl *func = (*i)->getMostRecentDecl();

        std::string funcDef = rewriter->getRewrittenText(clang::SourceRange(
                    func->getLocStart(),
                    func->getParamDecl(func->param_size() - 1)->getLocEnd()));
        ss << "        __device__ " << funcDef << ") {" << std::endl;
        ss << "            " << stmtToString(func->getBody()) << std::endl;
        ss << "        }" << std::endl;
    }

    for (std::vector<const clang::ValueDecl *>::iterator i =
            info.referenced_begin(), e = info.referenced_end(); i != e; i++) {
        const clang::ValueDecl *ref = *i;

        bool found = false;
        OMPVarInfo foundVarInfo;
        for (std::vector<OMPVarInfo>::iterator ii = vars->begin(), ee =
                vars->end(); ii != ee; ii++) {
            OMPVarInfo curr = *ii;
            if (curr.getDecl() == ref) {
                found = true;
                foundVarInfo = curr;
            }
        }

        /*
         * If the variable 'ref' that we discovered as being referenced from
         * inside of this functor is not either:
         *
         *   1) A variable that we marked as captured from the external scope of
         *      the OpenMP pragma (found).
         *   2) A variable that was declared locally inside the parallel region
         *      itself (info.isDeclaredInsideParallelRegion).
         *   3) The iterator variable for this parallel region (iterator).
         *
         * then this would indicate we do not know of a variable that is being
         * captured, and there's some bug in the analysis.
         */
        if (!found && !info.isDeclaredInsideParallelRegion(ref) &&
                ref->getNameAsString() != iterator) {
            std::cerr << "Failed to discover " << ref->getNameAsString() <<
                " for " << closureName << std::endl;
            exit(1);
        }

        if (found) {
            if (!first_field) {
                constructor_sig << "," << std::endl << "                ";
            }

            switch (foundVarInfo.getType()) {
                case (CAPTURE_TYPE::SHARED):
#ifdef VERBOSE
                    std::cerr << "getCUDAFunctorDef: shared field " <<
                        ref->getNameAsString() << ", type " <<
                        std::string(ref->getType()->getTypeClassName()) <<
                        std::endl;
#endif
                    if (const clang::BuiltinType *builtin =
                            clang::dyn_cast<clang::BuiltinType>(ref->getType())) {
                        const std::string builtinStr =
                            builtin->getName(Context->getPrintingPolicy()).str();
                        ss << "    volatile " << builtinStr << " " <<
                            ref->getNameAsString() << ";" << std::endl;
                        constructor_sig << builtinStr << " set_" <<
                            ref->getNameAsString();
                        functor_params->addParameter(ref, foundVarInfo);
                        constructor_body << "            " <<
                            ref->getNameAsString() << " = set_" <<
                            ref->getNameAsString() << ";" << std::endl;
                    } else if (const clang::PointerType *pointer =
                            clang::dyn_cast<clang::PointerType>(ref->getType())) {
                        if (!handleCapturedPointer(pointer, ref, foundVarInfo,
                                    true, constructor_sig, constructor_body, ss,
                                    functor_params, toBeTransferred)) {
                            return "";
                        }
                    } else if (const clang::ConstantArrayType *arr =
                            clang::dyn_cast<clang::ConstantArrayType>(
                                ref->getType())) {
                        const llvm::APInt arraySize = arr->getSize();
                        const clang::Type *eleType =
                            arr->getElementType().getTypePtr();
                        if (const clang::BuiltinType *builtin =
                                clang::dyn_cast<clang::BuiltinType>(eleType)) {
                            const std::string builtinStr =
                                builtin->getName(Context->getPrintingPolicy()).str();
                            ss << "    volatile " << builtinStr << " " <<
                                ref->getNameAsString() << "[" << arraySize.toString(10, false) <<
                                "];" << std::endl;
                            constructor_sig << builtinStr << " set_" <<
                                ref->getNameAsString() << "[" << arraySize.toString(10, false) <<
                                "]";
                            functor_params->addParameter(ref, foundVarInfo);
                            constructor_body << "            memcpy((void *)" <<
                                ref->getNameAsString() << ", (void *)set_" <<
                                ref->getNameAsString() << ", sizeof(" <<
                                ref->getNameAsString() << "));" << std::endl;
                        } else if (const clang::ConstantArrayType *nestedArr =
                                clang::dyn_cast<clang::ConstantArrayType>(eleType)) {
                            const llvm::APInt nestedArrSize = nestedArr->getSize();
                            const clang::Type *nestedEleType =
                                nestedArr->getElementType().getTypePtr();
                            if (const clang::BuiltinType *builtin =
                                    clang::dyn_cast<clang::BuiltinType>(nestedEleType)) {
                                const std::string builtinStr =
                                    builtin->getName(Context->getPrintingPolicy()).str();
                                ss << "    volatile " << builtinStr << " " <<
                                    ref->getNameAsString() << "[" << arraySize.toString(10, false) <<
                                    "][" << nestedArrSize.toString(10, false) << "];" << std::endl;
                                constructor_sig << builtinStr << " set_" <<
                                    ref->getNameAsString() << "[" << arraySize.toString(10, false) <<
                                    "][" << nestedArrSize.toString(10, false) << "]";
                                functor_params->addParameter(ref, foundVarInfo);
                                constructor_body << "            memcpy((void *)" <<
                                    ref->getNameAsString() << ", (void *)set_" <<
                                    ref->getNameAsString() << ", sizeof(" <<
                                    ref->getNameAsString() << "));" << std::endl;
                            } else {
                                std::cerr << "getCUDAFunctorDef: Unsupported " <<
                                    "element type on constant array \"" <<
                                    ref->getNameAsString() << "\": " <<
                                    std::string(nestedEleType->getTypeClassName()) <<
                                    std::endl;
                                exit(1);
                            }
                        } else {
                            std::cerr << "getCUDAFunctorDef: Unsupported " <<
                                "element type on constant array \"" <<
                                ref->getNameAsString() << "\": " <<
                                std::string(eleType->getTypeClassName()) <<
                                std::endl;
                            exit(1);
                        }
                    } else if (const clang::RecordType *record =
                            clang::dyn_cast<clang::RecordType>(ref->getType())) {
                        if (checkForPointersInRecord(record)) {
                            std::cerr << "getCUDAFunctorDef: Found captured " <<
                                "record type \"" <<
                                record->getDecl()->getNameAsString() <<
                                "\" has pointer fields." << std::endl;
                            return "";
                        }

                        ss << "    struct " <<
                            record->getDecl()->getNameAsString() << " " <<
                            ref->getNameAsString() << ";" << std::endl;
                        constructor_sig << "struct " <<
                            record->getDecl()->getNameAsString() << " *set_" <<
                            ref->getNameAsString();
                        functor_params->addParameter(ref, foundVarInfo);
                        constructor_body << "            memcpy((void *)&" <<
                            ref->getNameAsString() << ", set_" <<
                            ref->getNameAsString() << ", sizeof(struct " <<
                            record->getDecl()->getNameAsString() << "));" <<
                            std::endl;
                    } else if (const clang::TypedefType *typdef =
                            clang::dyn_cast<clang::TypedefType>(ref->getType())) {
                        const clang::Type *nested =
                            typdef->getDecl()->getTypeSourceInfo()->getType().getTypePtr();
                        if (const clang::BuiltinType *builtin = clang::dyn_cast<clang::BuiltinType>(nested)) {
                            const std::string typdefStr =
                                typdef->getDecl()->getNameAsString();
                            ss << "    volatile " << typdefStr << " " <<
                                ref->getNameAsString() << ";" << std::endl;
                            constructor_sig << typdefStr << " set_" <<
                                ref->getNameAsString();
                            functor_params->addParameter(ref, foundVarInfo);
                            constructor_body << "            " <<
                                ref->getNameAsString() << " = set_" <<
                                ref->getNameAsString() << ";" << std::endl;
                        } else {
                            std::cerr << "getCUDAFunctorDef: Unsupported " <<
                                "shared typedef capture type " <<
                                std::string(nested->getTypeClassName()) <<
                                std::endl;
                            exit(1);
                        }
                    } else {
                        std::cerr << "getCUDAFunctorDef: Unsupported shared " <<
                            "capture type " <<
                            std::string(ref->getType()->getTypeClassName()) <<
                            std::endl;
                        exit(1);
                    }
                    break;
                case (CAPTURE_TYPE::PRIVATE):
                case (CAPTURE_TYPE::FIRSTPRIVATE):
#ifdef VERBOSE
                    std::cerr << "getCUDAFunctorDef: private/firstprivate field " <<
                        ref->getNameAsString() << std::endl;
#endif
                    if (const clang::BuiltinType *builtin =
                            clang::dyn_cast<clang::BuiltinType>(ref->getType())) {
                        const std::string builtinStr =
                            builtin->getName(Context->getPrintingPolicy()).str();
                        ss << "    " << builtinStr << " " <<
                            ref->getNameAsString() << ";" << std::endl;
                        constructor_sig << builtinStr << " set_" <<
                            ref->getNameAsString();
                        functor_params->addParameter(ref, foundVarInfo);
                        constructor_body << "            " <<
                            ref->getNameAsString() << " = set_" <<
                            ref->getNameAsString() << ";" << std::endl;
                    } else if (const clang::PointerType *pointer =
                            clang::dyn_cast<clang::PointerType>(ref->getType())) {
                        if (!handleCapturedPointer(pointer, ref, foundVarInfo,
                                    false, constructor_sig, constructor_body,
                                    ss, functor_params, toBeTransferred)) {
                            return "";
                        }
                    } else if (const clang::TypedefType *typedefType =
                            clang::dyn_cast<clang::TypedefType>(ref->getType())) {
                        const clang::Type *nested =
                            typedefType->getDecl()->getTypeSourceInfo()->getType().getTypePtr();

                        if (const clang::ElaboratedType *elaborated = clang::dyn_cast<clang::ElaboratedType>(nested)) {
                            const clang::Type *nested = elaborated->getNamedType().getTypePtr();
                            if (const clang::RecordType *record =
                                    clang::dyn_cast<clang::RecordType>(nested)) {
                                if (checkForPointersInRecord(record)) {
                                    std::cerr << "getCUDAFunctorDef: Found " <<
                                        "captured record type \"" <<
                                        record->getDecl()->getNameAsString() <<
                                        "\" has pointer fields." << std::endl;
                                    return "";
                                }

                                const std::string typdefStr =
                                    typedefType->getDecl()->getNameAsString();
                                ss << "    " << typdefStr << " " <<
                                    ref->getNameAsString() << ";" << std::endl;
                                constructor_sig << typdefStr << " set_" <<
                                    ref->getNameAsString();
                                functor_params->addParameter(ref, foundVarInfo);
                                constructor_body << "            " <<
                                    ref->getNameAsString() << " = set_" <<
                                    ref->getNameAsString() << ";" << std::endl;
                            } else {
                                std::cerr << "getCUDAFunctorDef: Unsupported " <<
                                    "nested private/firstprivate elaborated type: " <<
                                    std::string(nested->getTypeClassName()) <<
                                    std::endl;
                                exit(1);
                            }
                        } else if (const clang::BuiltinType *builtin =
                                clang::dyn_cast<clang::BuiltinType>(nested)) {
                            const std::string typdefStr =
                                typedefType->getDecl()->getNameAsString();
                            ss << "    " << typdefStr << " " <<
                                ref->getNameAsString() << ";" << std::endl;
                            constructor_sig << typdefStr << " set_" <<
                                ref->getNameAsString();
                            functor_params->addParameter(ref, foundVarInfo);
                            constructor_body << "            " <<
                                ref->getNameAsString() << " = set_" <<
                                ref->getNameAsString() << ";" << std::endl;
                        } else {
                            std::cerr << "getCUDAFunctorDef: Unsupported " <<
                                "nested private/firstprivate typedef type: " <<
                                std::string(nested->getTypeClassName()) <<
                                std::endl;
                            exit(1);
                        }
                    } else {
                        std::cerr << "getCUDAFunctorDef: Unsupported " <<
                            "private/firstprivate capture type " <<
                            std::string(ref->getType()->getTypeClassName()) <<
                            std::endl;
                        exit(1);
                    }
                    break;
                case (CAPTURE_TYPE::LASTPRIVATE):
#ifdef VERBOSE
                    std::cerr << "getCUDAFunctorDef: lastprivate field " <<
                        ref->getNameAsString() << std::endl;
#endif
                    assert(false);
                    break;
                default:
                    std::cerr << "getCUDAFunctorDef: Unsupported capture clause " <<
                        foundVarInfo.getType() << std::endl;
                    exit(1);
            }

            first_field = false;
        }
    }

    std::stringstream cudaErrorChecking;
    cudaErrorChecking << "        if (err != cudaSuccess) {" << std::endl;
    cudaErrorChecking << "            fprintf(stderr, \"CUDA Error @ %s:%d " <<
        "- %s\\n\", __FILE__, __LINE__, cudaGetErrorString(err));" << std::endl;
    cudaErrorChecking << "            exit(3);" << std::endl;
    cudaErrorChecking << "        }" << std::endl;
    const std::string cudaErrorCheckingStr = cudaErrorChecking.str();

    ss << "\n";
    ss << "    public:\n";
    ss << constructor_sig.str() << ") {" << std::endl;
    ss << constructor_body.str() << std::endl;
    ss << "        }" << std::endl;
    ss << std::endl;
    ss << "    void transfer_to_device() {" << std::endl;
    ss << "        int i;" << std::endl;
    ss << "        cudaError_t err;" << std::endl;
    ss << std::endl;
    for (std::vector<OMPVarInfo>::iterator i = toBeTransferred->begin(),
            e = toBeTransferred->end(); i != e; i++) {
        OMPVarInfo curr = *i;
        ss << "        " << curr.getDecl()->getNameAsString() << " = NULL;" <<
            std::endl;
    }
    ss << std::endl;
    ss << "        get_underlying_allocations(&host_allocations, " <<
        "&host_allocation_sizes, &nallocations, " << toBeTransferred->size();
    for (std::vector<OMPVarInfo>::iterator i = toBeTransferred->begin(),
            e = toBeTransferred->end(); i != e; i++) {
        OMPVarInfo curr = *i;
        ss << ", h_" << curr.getDecl()->getNameAsString();
    }
    ss << ");" << std::endl;
    ss << "        device_allocations = (void **)malloc(" <<
        "nallocations * sizeof(void *));" << std::endl;
    ss << "        for (i = 0; i < nallocations; i++) {" << std::endl;
    ss << "            err = cudaMalloc((void **)&device_allocations[i], " <<
        "host_allocation_sizes[i]);" << std::endl;
    ss << cudaErrorCheckingStr;
    ss << "            err = cudaMemcpy((void *)device_allocations[i], " <<
        "(void *)host_allocations[i], host_allocation_sizes[i], " <<
        "cudaMemcpyHostToDevice);" << std::endl;
    ss << cudaErrorCheckingStr;
    for (std::vector<OMPVarInfo>::iterator i = toBeTransferred->begin(),
            e = toBeTransferred->end(); i != e; i++) {
        OMPVarInfo curr = *i;
        const std::string varname = curr.getDecl()->getNameAsString();
        ss << "            if (" << varname << " == NULL && (char *)h_" << varname <<
            " >= (char *)host_allocations[i] && ((char *)h_" << varname <<
            " - (char *)host_allocations[i]) < host_allocation_sizes[i]) {" <<
            std::endl;
        ss << "                char *tmp = (char *)device_allocations[i] + " <<
            "((char *)h_" << varname << " - (char *)host_allocations[i]);" <<
            std::endl;
        ss << "                memcpy((void *)(&" << varname <<
            "), (void *)(&tmp), sizeof(void *));" << std::endl;
        ss << "            }" << std::endl;
    }
    ss << "        }" << std::endl;
    ss << std::endl;
    for (std::vector<OMPVarInfo>::iterator i = toBeTransferred->begin(),
            e = toBeTransferred->end(); i != e; i++) {
        OMPVarInfo curr = *i;
        ss << "        assert(" << curr.getDecl()->getNameAsString() <<
            " || h_" << curr.getDecl()->getNameAsString() << " == NULL);" <<
            std::endl;
    }
    ss << std::endl;
    ss << "    }" << std::endl;
    ss << std::endl;
    ss << "    void transfer_from_device() {" << std::endl;
    ss << "        cudaError_t err;" << std::endl;
    ss << "        int i;" << std::endl;
    ss << "        for (i = 0; i < nallocations; i++) {" << std::endl;
    ss << "            err = cudaMemcpy((void *)host_allocations[i], " <<
        "(void *)device_allocations[i], host_allocation_sizes[i], " <<
        "cudaMemcpyDeviceToHost);" << std::endl;
    ss << cudaErrorCheckingStr;
    ss << "            err = cudaFree(device_allocations[i]);" << std::endl;
    ss << cudaErrorCheckingStr;
    ss << "        }" << std::endl;
    ss << "    }" << std::endl;
    ss << std::endl;
    ss << "        __device__ void operator()(int " << iterator << ") {\n";
    // Included so that continue statements have the same semantics.
    ss << "            for (int __dummy_iter = 0; __dummy_iter < 1; __dummy_iter++) {" << std::endl;
    ss << "                " << bodyStr << "\n";
    ss << "            }" << std::endl;
    ss << "        }\n";
    ss << "};\n\n";

    return ss.str();
}

std::string OMPToHClib::getClosureDecl(std::string closureName,
        bool isForasyncClosure, int forasyncDim, bool isFuture,
        bool &isAcceleratable, std::string iterator, std::string bodyStr,
        const clang::Stmt *body, std::vector<clang::ValueDecl *> *captured,
        OMPClauses *clauses, CUDAFunctorParameters *functor_params) {
    std::stringstream ss;

    if (target == CUDA) {
        if (isAcceleratable) {
            std::string functor = getCUDAFunctorDef(closureName, captured, clauses,
                    iterator, bodyStr, body, functor_params);
            if (functor.length() == 0) {
                isAcceleratable = false;
            } else {
                ss << functor;
            }
        }
    } else {
        ss << "static ";
        if (isFuture) {
            ss << "void *";
        } else {
            ss << "void ";
        }
        ss << closureName << "(void *____arg";
        if (isForasyncClosure) {
            assert(forasyncDim > 0);
            for (int i = 0; i < forasyncDim; i++) {
                ss << ", const int ___iter" << i;
            }
        } else {
            assert(forasyncDim == -1);
        }
        ss << ");\n";
    }

    return ss.str();
}

bool OMPToHClib::canLaunchTasks(const clang::Stmt *body) {
    if (const clang::CallExpr *call = clang::dyn_cast<clang::CallExpr>(body)) {
        /*
         * For now, if there are any function calls assume they may result in
         * task creation. This is a very conservative decision. TODO improve.
         */
        return true;
    }

    for (clang::Stmt::const_child_iterator i = body->child_begin(),
            e = body->child_end(); i != e; i++) {
        const clang::Stmt *child = *i;
        if (child != NULL && canLaunchTasks(child)) {
            return true;
        }
    }
    return false;
}

std::string OMPToHClib::getClosureDef(std::string closureName,
        bool isForasyncClosure, bool isAsyncClosure,
        std::string contextName, std::vector<clang::ValueDecl *> *captured,
        std::string bodyStr, bool isFuture, OMPClauses *clauses,
        bool wrapBodyInFinish, bool waitAtEnd,
        std::vector<const clang::ValueDecl *> *condVars) {
    assert(!(isForasyncClosure && isAsyncClosure));
    std::vector<OMPReductionVar> *reductions = clauses->getReductions();
    std::vector<OMPVarInfo> *vars = clauses->getVarInfo(captured);

    std::stringstream ss;

    ss << "\nstatic ";
    if (isFuture) {
        ss << "void *";
    } else {
        ss << "void ";
    }
    ss << closureName << "(void *____arg";
    if (isForasyncClosure) {
        for (int i = 0; i < condVars->size(); i++) {
            ss << ", const int ___iter" << i;
        }
    }
    ss << ") {\n";

    ss << "    " << contextName << " *ctx = (" << contextName <<
        " *)____arg;\n";
    
    for (std::vector<OMPVarInfo>::iterator i = vars->begin(), e = vars->end();
            i != e; i++) {
        OMPVarInfo var = *i;
        clang::ValueDecl *decl = var.getDecl();

        switch (var.getType()) {
            case (CAPTURE_TYPE::SHARED):
                // Do nothing, should refer to the captured address in the body
                break;
            case (CAPTURE_TYPE::PRIVATE):
            case (CAPTURE_TYPE::FIRSTPRIVATE):
            case (CAPTURE_TYPE::LASTPRIVATE):
                ss << "    " << getUnpackStr(decl) << std::endl;
                break;
            default:
                std::cerr << "Unsupported capture type" << std::endl;
                exit(1);
        }
    }

    if (wrapBodyInFinish) {
        /*
         * In OMP's task model you can use taskwait to wait on all previously
         * created child tasks of the currently executing task. To model this in
         * HClib, we wrap the body of each async in a finish scope and on
         * encountering a taskwait call hclib_end_finish followed by
         * hclib_start_finish.
         */
        ss << "    hclib_start_finish();\n";
    }

    if (isForasyncClosure) {
        /*
         * Insert a one iteration do-loop around the original body so that
         * continues have the same semantics and in case the condition variable
         * declared below has the same name as a captured variable.
         */
        ss << "    do {\n";

        /*
         * In the case of C++ style loops with the iterator variable declared inside
         * the initialization clause, make sure the condition variable still gets
         * added into the body of the kernel.
         */
        for (std::vector<const clang::ValueDecl *>::iterator i =
                condVars->begin(), e = condVars->end(); i != e; i++) {
            const clang::ValueDecl *condVar = *i;
            if (std::find(captured->begin(), captured->end(), condVar) == captured->end()) {
                ss << "    " << getDeclarationTypeStr(condVar->getType(),
                        condVar->getNameAsString(), "", "") << "; ";
            }
        }

        int iterCount = 0;
        for (std::vector<const clang::ValueDecl *>::iterator i =
                condVars->begin(), e = condVars->end(); i != e; i++) {
            const clang::ValueDecl *condVar = *i;
            ss << "    " << condVar->getNameAsString() << " = ___iter" <<
                iterCount << ";\n";
            iterCount++;
        }
    }

    ss << bodyStr << " ; ";

    if (isForasyncClosure) {
        ss << "    } while (0);\n";
        if (!reductions->empty()) {
            ss << "    const int lock_err = pthread_mutex_lock(&ctx->reduction_mutex);\n";
            ss << "    assert(lock_err == 0);\n";

            for (std::vector<OMPReductionVar>::iterator i = reductions->begin(),
                    e = reductions->end(); i != e; i++) {
                OMPReductionVar red = *i;
                std::string varname = red.getVar();

                ss << "    ctx->" << varname << " " << red.getOp() << "= " <<
                    varname << ";\n";
            }

            ss << "    const int unlock_err = pthread_mutex_unlock(&ctx->reduction_mutex);\n";
            ss << "    assert(unlock_err == 0);\n";
        }
    }

    if (wrapBodyInFinish) {
        /*
         * We wrap the body of this task with a finish scope to support internal
         * taskwait calls. However, a call to hclib_end_finish can be very
         * expensive because of lightweight context switching. Therefore, when
         * possible we simply close the finish scope and do not block on it.
         * This is mainly useful at the end of a task block or inside the body
         * of a parallel for, because there is no implicit barrier there.
         */
        if (waitAtEnd) {
            ss << "    ; hclib_end_finish();\n\n";
        } else {
            ss << "    ; hclib_end_finish_nonblocking();\n\n";
        }
    }

    for (std::vector<OMPVarInfo>::iterator i = vars->begin(), e = vars->end();
            i != e; i++) {
        OMPVarInfo var = *i;
        clang::ValueDecl *decl = var.getDecl();

        switch (var.getType()) {
            case (CAPTURE_TYPE::SHARED):
            case (CAPTURE_TYPE::PRIVATE):
            case (CAPTURE_TYPE::FIRSTPRIVATE):
                break;
            case (CAPTURE_TYPE::LASTPRIVATE):
                /*
                 * Unsupported at the moment, would need to check if this is the
                 * last iteration.
                 */
                assert(false);
                assert(isForasyncClosure);
                ss << "    *(ctx->" << decl->getNameAsString() << "_ptr) = " <<
                    decl->getNameAsString() << ";" << std::endl;
                break;
            default:
                std::cerr << "Unsupported capture type" << std::endl;
                exit(1);
        }
    }

    if (!isForasyncClosure) {
        ss << "    free(____arg);\n";
    }

    if (isFuture) {
        ss << "    return NULL;\n";
    }

    ss << "}\n\n";

    return ss.str();
}

enum CAPTURE_TYPE OMPToHClib::getParentCaptureType(PragmaNode *curr,
        std::string varname) {
    if (curr == NULL || curr->getPragmaName() != "omp") {
        return CAPTURE_TYPE::PRIVATE;
    }

    OMPClauses *clauses = getOMPClausesForMarker(curr->getMarker());
    std::vector<OMPVarInfo> *vars = clauses->getVarInfo(curr->getCaptures());
    for (std::vector<OMPVarInfo>::iterator i = vars->begin(), e = vars->end();
            i != e; i++) {
        OMPVarInfo info = *i;
        if (info.getDecl()->getNameAsString() == varname) {
            return info.getType();
        }
    }

    return getParentCaptureType(curr->getParentAccountForFusing(), varname);
}

std::string OMPToHClib::getCriticalSectionLockStr(int criticalSectionId) {
    std::stringstream lock_ss;
    lock_ss << " { const int ____lock_" << criticalSectionId <<
        "_err = pthread_mutex_lock(&critical_" <<
        criticalSectionId << "_lock); assert(____lock_" <<
        criticalSectionId << "_err == 0); ";
    return lock_ss.str();
}

std::string OMPToHClib::getCriticalSectionUnlockStr(int criticalSectionId) {
    std::stringstream unlock_ss;
    unlock_ss << "; const int ____unlock_" <<
        criticalSectionId << "_err = " <<
        "pthread_mutex_unlock(&critical_" <<
        criticalSectionId << "_lock); assert(____unlock_" <<
        criticalSectionId << "_err == 0); } ";
    return unlock_ss.str();
}

std::string OMPToHClib::getContextSetup(PragmaNode *node,
        std::string structName, std::vector<clang::ValueDecl *> *captured,
        OMPClauses *clauses) {
    std::stringstream ss;
    ss << structName << " *new_ctx = (" << structName << " *)malloc(sizeof(" <<
        structName << "));\n";
    std::vector<OMPVarInfo> *vars = clauses->getVarInfo(captured);

    for (std::vector<OMPVarInfo>::iterator i = vars->begin(), e = vars->end();
            i != e; i++) {
        OMPVarInfo var = *i;
        clang::ValueDecl *decl = var.getDecl();
        enum CAPTURE_TYPE parentType = getParentCaptureType(
                node->getParentAccountForFusing(), decl->getNameAsString());

        switch (var.getType()) {
            case (CAPTURE_TYPE::SHARED):
                if (!var.checkIsGlobal()) {
                    if (parentType == CAPTURE_TYPE::SHARED) {
                        ss << "new_ctx->" << decl->getNameAsString() <<
                            "_ptr = ctx->" << decl->getNameAsString() <<
                            "_ptr;" << std::endl;
                    } else {
                        ss << "new_ctx->" << decl->getNameAsString() <<
                            "_ptr = &(" << decl->getNameAsString() << ");" <<
                            std::endl;
                    }
                }
                break;
            case (CAPTURE_TYPE::PRIVATE):
            case (CAPTURE_TYPE::FIRSTPRIVATE):
                if (parentType == CAPTURE_TYPE::SHARED) {
                    ss << "new_ctx->" << decl->getNameAsString() <<
                        " = *(ctx->" << decl->getNameAsString() << "_ptr);" <<
                        std::endl;
                } else {
                    ss << getCaptureStr(decl) << std::endl;
                }
                break;
            case (CAPTURE_TYPE::LASTPRIVATE):
                assert(false);
                break;
            default:
                std::cerr << "Unsupported capture type" << std::endl;
                exit(1);
        }
    }

    std::vector<OMPReductionVar> *reductions = clauses->getReductions();
    if (!reductions->empty()) {
        for (std::vector<OMPReductionVar>::iterator i = reductions->begin(),
                e = reductions->end(); i != e; i++) {
            OMPReductionVar red = *i;
            // Initialize the reduction target based on the reduction op
            ss << "new_ctx->" << red.getVar() << " = " << red.getInitialValue() <<
                ";\n";
        }
        ss << "const int init_err = pthread_mutex_init(&new_ctx->reduction_mutex, NULL);\n";
        ss << "assert(init_err == 0);\n";
    }

    return ss.str();
}

void OMPToHClib::removePragma(PragmaNode *node) {
    std::string bodyStr;
    if (node->getBody()) {
        bodyStr = stmtToString(node->getBody());
    } else {
        bodyStr = "";
    }

    const bool failed = rewriter->ReplaceText(clang::SourceRange(
                node->getStartLoc(), node->getEndLoc()), bodyStr);
    assert(!failed);
}

void OMPToHClib::postFunctionVisit(clang::FunctionDecl *func) {
    assert(getCurrentLexicalDepth() == 1);
    popScope();

    if (func) {
        std::string fname = func->getNameAsString();
        clang::PresumedLoc presumedStart = SM->getPresumedLoc(
                func->getLocStart());
        const int functionStartLine = presumedStart.getLine();

        pragmaTree->print();

        std::string accumulatedStructDefs = "";
        std::string accumulatedKernelDecls = "";
        std::string accumulatedKernelDefs = "";

        std::vector<PragmaNode *> *leaves = pragmaTree->getLeaves();

        for (std::vector<PragmaNode *>::iterator i = leaves->begin(),
                e = leaves->end(); i != e; i++) {
            PragmaNode *node = *i;
            std::string pragmaName = node->getPragmaName();

            if (pragmaName == "omp_to_hclib") {
                if (target == HCLIB) {
                    if (foundOmpToHclibLaunch) {
                        std::cerr << "Found multiple locations where the " <<
                            "omp_to_hclib pragma is used. This use case is " <<
                            "currently unsupported." << std::endl;
                        exit(1);
                    }
                    foundOmpToHclibLaunch = true;

                    OMPClauses *generatedClauses = new OMPClauses();
                    std::vector<clang::ValueDecl *> *captures = node->getCaptures();
                    for (std::vector<clang::ValueDecl *>::iterator i =
                            captures->begin(), e = captures->end(); i != e; i++) {
                        generatedClauses->addClauseArg("private",
                                (*i)->getNameAsString());
                    }

                    std::string launchBody = stmtToString(node->getBody());
                    std::string launchStruct = getStructDef(
                            "main_entrypoint_ctx", captures, generatedClauses);
                    std::string closureFunction = getClosureDef(
                            "main_entrypoint", false, false, "main_entrypoint_ctx",
                            captures, launchBody, false, generatedClauses, false, false);
                    std::string contextSetup = getContextSetup(node,
                            "main_entrypoint_ctx", captures,
                            generatedClauses);
                    std::string launchStr = contextSetup +
                        "const char *deps[] = { \"system\" };\n" +
                        "hclib_launch(main_entrypoint, new_ctx, deps, 1);\n";

                    bool failed = rewriter->ReplaceText(
                            clang::SourceRange(node->getStartLoc(),
                                node->getEndLoc()), launchStr);
                    assert(!failed);

                    accumulatedStructDefs += launchStruct;
                    accumulatedKernelDecls += closureFunction;
                } else {
                    removePragma(node);
                }
            } else if (pragmaName == "omp") {
                std::string ompCmd = node->getPragmaCmd();
                OMPClauses *clauses = getOMPClausesForMarker(node->getMarker());

                if (ompCmd == "critical") {
                    if (target == HCLIB) {
                        const clang::Stmt *body = node->getBody();

                        std::string lock = getCriticalSectionLockStr(criticalSectionId);
                        std::string unlock = getCriticalSectionUnlockStr(criticalSectionId);
                        criticalSectionId++;

                        const bool failed = rewriter->ReplaceText(
                                clang::SourceRange(node->getStartLoc(), node->getEndLoc()),
                                lock + stmtToString(body) + unlock);
                        assert(!failed);
                    } else {
                        removePragma(node);
                    }
                } else if (ompCmd == "atomic") {
                    // For now we implement atomic with rather heavyweight locks
                    // TODO, use actual atomic instructions
                    const clang::Stmt *body = node->getBody();
                    const clang::BinaryOperator *bin = clang::dyn_cast<clang::BinaryOperator>(body);
                    assert(bin);
                    clang::Expr *rhs = bin->getRHS();

                    std::stringstream tmp_varname;
                    tmp_varname << "____critical_section_tmp_" << criticalSectionId;

                    clang::Expr *lhs = bin->getLHS();
                    clang::DeclRefExpr *declRef = clang::dyn_cast<clang::DeclRefExpr>(lhs);
                    assert(declRef);
                    clang::ValueDecl *decl = declRef->getDecl();
                    std::string typeStr = decl->getType().getAsString();

                    // std::string lock = getCriticalSectionLockStr(criticalSectionId);
                    // std::string unlock = getCriticalSectionUnlockStr(criticalSectionId);
                    // std::stringstream ss;
                    // ss << "const " << typeStr << " " << tmp_varname.str() << " = " << stmtToString(rhs) <<
                    //     ";" << std::endl;
                    // criticalSectionId++;

                    if (bin->getOpcode() == clang::BO_AddAssign) {
                        std::stringstream ss;
                        ss << "__sync_fetch_and_add(&(" << stmtToString(lhs) << "), " << stmtToString(rhs) << "); ";
                        const bool failed = rewriter->ReplaceText(clang::SourceRange(node->getStartLoc(), node->getEndLoc()),
                                ss.str());
                        // const bool failed = rewriter->ReplaceText(
                        //         clang::SourceRange(node->getStartLoc(), node->getEndLoc()),
                        //         ss.str() + lock + stmtToString(lhs) + " += " + tmp_varname.str() + " " + unlock);
                        assert(!failed);
                    } else {
                        std::cerr << "Unsupported binary operator in atomic: " << bin->getOpcode() << std::endl;
                        exit(1);
                    }
                } else if (ompCmd == "taskwait") {
                    if (target == HCLIB) {
                        const bool failed = rewriter->ReplaceText(
                                clang::SourceRange(node->getStartLoc(),
                                    node->getEndLoc()),
                                " hclib_end_finish(); hclib_start_finish(); ");
                        assert(!failed);
                    } else {
                        removePragma(node);
                    }
                } else if (ompCmd == "single" || ompCmd == "master") {
                    assert(node->getParent()); // should not be root
                    assert(node->getParent()->getPragmaName() == "omp");

                    std::string parentCmd = node->getParent()->getPragmaCmd();
                    if (parentCmd != "parallel") {
                        std::cerr << "It appears the \"single\" pragma " <<
                            "on line " << node->getStartLine() <<
                            " does not have a \"parallel\" pragma as " <<
                            "its parent. Instead has \"" << parentCmd <<
                            "\" at line " <<
                            node->getParent()->getStartLine() << "." <<
                            std::endl;
                        node->getParent()->print();
                        exit(1);
                    }

                    if (target == HCLIB) {
                        const clang::CompoundStmt *parentCmpd =
                            clang::dyn_cast<clang::CompoundStmt>(
                                    node->getParent()->getBody());
                        assert(parentCmpd);
                        assert(parentCmpd->size() == 2);
                        clang::CompoundStmt::const_body_iterator bodyIter = parentCmpd->body_begin();
                        assert(*bodyIter == node->getMarker());
                        bodyIter++;
                        assert(*bodyIter == node->getBody());

                        /*
                         * Verify that at least one of these pragmas does not
                         * have the nowait clause.
                         */
                        OMPClauses *parentClauses = getOMPClausesForMarker(
                                node->getParent()->getMarker());
                        assert(!clauses->hasClause("nowait") ||
                                !parentClauses->hasClause("nowait"));

                        // Insert finish
                        const clang::Stmt *body = node->getBody();

                        if (ompCmd == "master") {
                            std::string bodyStr = stmtToStringWithSharedVars(body,
                                    clauses->getSharedVarInfo(node->getCaptures()));

                            bool isAcceleratable = false;
                            accumulatedKernelDecls += getClosureDecl(
                                    node->getLbl() + ASYNC_SUFFIX, false, -1, true,
                                    isAcceleratable);

                            accumulatedKernelDefs += getClosureDef(
                                    node->getLbl() + ASYNC_SUFFIX, false, true,
                                    node->getLbl(), node->getCaptures(),
                                    bodyStr, true, clauses, canLaunchTasks(body), true);

                            const std::string structDef = getStructDef(
                                    node->getLbl(), node->getCaptures(), clauses);
                            accumulatedStructDefs += structDef;

                            std::stringstream contextCreation;
                            contextCreation << "\n" << getContextSetup(node,
                                    node->getLbl(), node->getCaptures(), clauses);
                            contextCreation << "hclib_future_t *fut = " <<
                                "hclib_async_future(" << node->getLbl() <<
                                ASYNC_SUFFIX << ", new_ctx, NO_FUTURE, " <<
                                "hclib_get_master_place());\n";
                            contextCreation << "hclib_future_wait(fut);\n";
                            // Add braces to ensure we don't change control flow
                            const bool failed = rewriter->ReplaceText(
                                    clang::SourceRange(node->getParent()->getStartLoc(),
                                        node->getParent()->getEndLoc()),
                                    " { " + contextCreation.str() + " } ");
                            assert(!failed);
                        } else { // single
                            const bool failed = rewriter->ReplaceText(
                                    clang::SourceRange(node->getParent()->getStartLoc(),
                                        node->getParent()->getEndLoc()),
                                    "hclib_start_finish(); " + stmtToString(body) +
                                        " ; hclib_end_finish(); ");
                            assert(!failed);
                        }
                    } else {
                        removePragma(node->getParent());
                        removePragma(node);
                    }
                } else if (ompCmd == "task") {
                    switch (target) {
                        case (HCLIB): {
                            const clang::Stmt *body = node->getBody();
                            OMPClauses *clauses = getOMPClausesForMarker(
                                    node->getMarker());
                            std::string bodyStr = stmtToStringWithSharedVars(body,
                                    clauses->getSharedVarInfo(node->getCaptures()));

                            bool isAcceleratable = false;
                            accumulatedKernelDecls += getClosureDecl(
                                    node->getLbl() + ASYNC_SUFFIX, false, -1,
                                    clauses->hasClause("depend"), isAcceleratable);

                            accumulatedKernelDefs += getClosureDef(
                                    node->getLbl() + ASYNC_SUFFIX, false, true,
                                    node->getLbl(), node->getCaptures(),
                                    bodyStr, clauses->hasClause("depend"),
                                    clauses, canLaunchTasks(body), false);

                            const std::string structDef = getStructDef(
                                    node->getLbl(), node->getCaptures(), clauses);
                            accumulatedStructDefs += structDef;

                            std::stringstream contextCreation;
                            contextCreation << "\n" << getContextSetup(node,
                                    node->getLbl(), node->getCaptures(), clauses);

                            if (clauses->hasClause("if")) {
                                // We have already asserted there is only one if arg
                                std::string ifArg = clauses->getSingleArg("if");
                                // If the if condition is false, just call the task body sequentially
                                contextCreation << "if (!(" << ifArg << ")) {\n";
                                contextCreation << "    " << node->getLbl() << ASYNC_SUFFIX << "(new_ctx);\n";
                                contextCreation << "} else {\n";
                            }

                            if (clauses->hasClause("depend")) {
                                OMPDependencies *depends = new OMPDependencies(
                                        clauses->getArgs("depend"));
                                std::vector<OMPDependency> *in =
                                    depends->getInDependencies();
                                std::vector<OMPDependency> *out =
                                    depends->getOutDependencies();
                                contextCreation << "hclib_emulate_omp_task(" <<
                                    node->getLbl() << ASYNC_SUFFIX <<
                                    ", new_ctx, ANY_PLACE, " << in->size() << ", " <<
                                    out->size();

                                for (std::vector<OMPDependency>::iterator i =
                                        in->begin(), e = in->end(); i != e; i++) {
                                    OMPDependency curr = *i;
                                    contextCreation << ", " << curr.getAddrStr() <<
                                        ", " << curr.getLengthStr();
                                }
                                for (std::vector<OMPDependency>::iterator i =
                                        out->begin(), e = out->end(); i != e; i++) {
                                    OMPDependency curr = *i;
                                    contextCreation << ", " << curr.getAddrStr() <<
                                        ", " << curr.getLengthStr();
                                }
                                contextCreation << ");\n";

                            } else {
                                contextCreation << "hclib_async(" << node->getLbl() <<
                                    ASYNC_SUFFIX << ", new_ctx, NO_FUTURE, ANY_PLACE);\n";
                            }

                            if (clauses->hasClause("if")) {
                                contextCreation << "}\n";
                            }

                            // Add braces to ensure we don't change control flow
                            const bool failed = rewriter->ReplaceText(
                                    clang::SourceRange(node->getStartLoc(), node->getEndLoc()),
                                    " { " + contextCreation.str() + " } ");
                            assert(!failed);
                            break;
                        }
                        case (CUDA):
                            removePragma(node);
                            break;
                        default:
                            std::cerr << "OMP command " << ompCmd <<
                                " unsupported by target language " << target <<
                                std::endl;
                            exit(1);
                    }
                } else if (ompCmd == "parallel") {
                    OMPClauses *clauses = getOMPClausesForMarker(node->getMarker());

                    if (clauses->hasClause("for")) {

                        const clang::ForStmt *forLoop =
                            clang::dyn_cast<clang::ForStmt>(node->getBody());
                        if (!forLoop) {
                            std::cerr << "Expected to find for loop " <<
                                "inside omp parallel for at line " <<
                                node->getStartLine() << " but found a " <<
                                node->getBody()->getStmtClassName() <<
                                " instead." << std::endl;
                            std::cerr << stmtToString(node->getBody()) << std::endl;
                            exit(1);
                        }

                        std::vector<OMPReductionVar> *reductions =
                            clauses->getReductions();

                        std::string bodyStr;
                        std::vector<const clang::ValueDecl *> condVars;
                        const int nLoops = clauses->getNumCollapsedLoops();

                        std::vector<std::string> accumulated_low;
                        std::vector<std::string> accumulated_high;
                        std::vector<std::string> accumulated_stride;
                        std::vector<const clang::ValueDecl *> accumulated_cond;

                        std::stringstream loopConfiguration;
                        loopConfiguration << "hclib_loop_domain_t domain[" <<
                            nLoops << "];\n";
                        const clang::ForStmt *currLoop = forLoop;
                        std::string originalBodyStr;
                        const clang::Stmt *body = NULL;

                        for (int l = 0; l < nLoops; l++) {
                            const clang::Stmt *init = currLoop->getInit();
                            const clang::Stmt *cond = currLoop->getCond();
                            const clang::Expr *inc = currLoop->getInc();

                            const clang::ValueDecl *condVar = NULL;
                            std::string lowStr =
                                getCondVarAndLowerBoundFromInit(init,
                                        &condVar);
                            assert(condVar);
                            condVars.push_back(condVar);
                            std::string highStr = getUpperBoundFromCond(
                                    cond, condVar);
                            std::string strideStr = getStrideFromIncr(inc,
                                    condVar);

                            accumulated_low.push_back(lowStr);
                            accumulated_high.push_back(highStr);
                            accumulated_stride.push_back(strideStr);
                            accumulated_cond.push_back(condVar);

                            clauses->addClauseArg("private",
                                    condVar->getNameAsString());

                            originalBodyStr = stmtToString(currLoop->getBody());
                            body = currLoop->getBody();
                            if (target == HCLIB) {
                                bodyStr = stmtToStringWithSharedVars(
                                        currLoop->getBody(),
                                        clauses->getSharedVarInfo(node->getCaptures()));
                            }

                            loopConfiguration << "domain["  << l << "].low = " <<
                                lowStr << ";\n";
                            loopConfiguration << "domain["  << l << "].high = " <<
                                highStr << ";\n";
                            loopConfiguration << "domain["  << l <<
                                "].stride = " << strideStr << ";\n";
                            loopConfiguration << "domain["  << l <<
                                "].tile = -1;\n";

                            currLoop = clang::dyn_cast<clang::ForStmt>(
                                    currLoop->getBody());
                            assert(currLoop || l == nLoops - 1);
                        }

                        bool isAcceleratable = (nLoops == 1 &&
                                accumulated_stride.at(0) == "1");

                        CUDAFunctorParameters functor_parameters;
                        accumulatedKernelDecls += getClosureDecl(
                                node->getLbl() + ASYNC_SUFFIX, true, nLoops,
                                false, isAcceleratable,
                                accumulated_cond.at(0)->getNameAsString(),
                                originalBodyStr, body, node->getCaptures(),
                                clauses, &functor_parameters);

                        std::stringstream constructor_params;
                        for (std::vector<CUDAParameter>::iterator i =
                                functor_parameters.begin(), e =
                                functor_parameters.end(); i != e; i++) {
                            CUDAParameter param = *i;
                            if (i != functor_parameters.begin()) {
                                constructor_params << ", ";
                            }

                            if (param.getInfo().getType() == CAPTURE_TYPE::SHARED &&
                                    clang::isa<clang::RecordType>(param.getDecl()->getType())) {
                                constructor_params << "&" << param.getDecl()->getNameAsString();
                            } else {
                                constructor_params << param.getDecl()->getNameAsString();
                            }
                        }

                        std::stringstream contextCreation;
                        if (target == HCLIB) {
                            contextCreation << "\n" <<
                                getContextSetup(node, node->getLbl(),
                                        node->getCaptures(), clauses);
                            contextCreation << loopConfiguration.str();

                            const std::string structDef = getStructDef(
                                    node->getLbl(), node->getCaptures(), clauses);
                            accumulatedStructDefs += structDef;

                            accumulatedKernelDefs += getClosureDef(
                                    node->getLbl() + ASYNC_SUFFIX, true, false,
                                    node->getLbl(), node->getCaptures(),
                                    bodyStr, false, clauses,
                                    canLaunchTasks(forLoop), false, &condVars);
                        }

                        // For now only support offload of 1D parallel loops
                        if (target == CUDA && isAcceleratable) {
                            contextCreation << "const int niters = (" <<
                                accumulated_high.at(0) << ") - (" <<
                                accumulated_low.at(0) << ");" << std::endl;
                            contextCreation << "const int iters_offset = (" <<
                                accumulated_low.at(0) << ");" << std::endl;
                            contextCreation << "kernel_launcher(\"" <<
                                node->getLbl() << "\", iters_offset, " <<
                                "niters, " << node->getLbl() << ASYNC_SUFFIX <<
                                "(" << constructor_params.str() <<
                                "));" << std::endl;
                        } else if (target == HCLIB) {
                            contextCreation << "hclib_future_t *fut = " <<
                                "hclib_forasync_future((void *)" <<
                                node->getLbl() << ASYNC_SUFFIX << ", new_ctx, " <<
                                nLoops << ", domain, HCLIB_FORASYNC_MODE);\n";
                            contextCreation << "hclib_future_wait(fut);\n";
                            contextCreation << "free(new_ctx);\n";

                            for (std::vector<OMPReductionVar>::iterator i =
                                    reductions->begin(), e = reductions->end();
                                    i != e; i++) {
                                OMPReductionVar var = *i;
                                contextCreation << var.getVar() << " = " <<
                                    "new_ctx->" << var.getVar() << ";\n";
                            }
                        }

                        if ((target == CUDA && isAcceleratable) ||
                                target == HCLIB) {
                            // Add braces to ensure we don't change control flow
                            const bool failed = rewriter->ReplaceText(
                                    clang::SourceRange(node->getStartLoc(),
                                        node->getEndLoc()),
                                    " { " + contextCreation.str() + " } ");
                            assert(!failed);
                        } else {
                            /*
                             * Failed to do code generation for this parallel
                             * region.
                             */
                            removePragma(node);
                        }
                    } else {
                        std::cerr << "Parallel pragma without a for at line " <<
                            node->getStartLine() << std::endl;
                        exit(1);
                    }
                } else if (ompCmd == "simd") {
                    // Do nothing for now, just delete this pragma
                    removePragma(node);
                } else {
                    std::cerr << "Unhandled OMP command \"" << ompCmd << "\"" <<
                        std::endl;
                    exit(1);
                }
            } else if (pragmaName == "root") {
                // Nothing to do once we get to the top of the pragma tree
            } else {
                std::cerr << "Unhandled pragma type \"" << pragmaName << "\"" <<
                    std::endl;
                exit(1);
            }
        }

        if (accumulatedStructDefs.length() > 0 ||
                accumulatedKernelDefs.length() > 0 ||
                accumulatedKernelDecls.length() > 0) {
            clang::SourceLocation insertLoc = func->getLocStart();
            if (func->getTemplatedKind() == clang::FunctionDecl::TemplatedKind::TK_FunctionTemplateSpecialization) {
                insertLoc = func->getPrimaryTemplate()->getLocStart();
            }

            clang::SourceLocation insertLocation = func->getLocStart();
            if (func->getTemplatedKind() == clang::FunctionDecl::TemplatedKind::TK_FunctionTemplateSpecialization) {
                insertLocation = func->getPrimaryTemplate()->getLocStart();
            }

            bool failed = rewriter->InsertText(insertLocation,
                    accumulatedStructDefs + accumulatedKernelDecls, true, true);
            assert(!failed);
            failed = rewriter->ReplaceText(func->getLocEnd(), 1,
                    "} " + accumulatedKernelDefs);
            assert(!failed);
        }
    }
}

bool OMPToHClib::isScopeCreatingStmt(const clang::Stmt *s) {
    return clang::isa<clang::CompoundStmt>(s) || clang::isa<clang::ForStmt>(s);
}

int OMPToHClib::getCurrentLexicalDepth() {
    return in_scope.size();
}

void OMPToHClib::addNewScope() {
    in_scope.push_back(new std::vector<clang::ValueDecl *>());
}

void OMPToHClib::popScope() {
    assert(in_scope.size() >= 1);
    in_scope.pop_back();
}

void OMPToHClib::addToCurrentScope(clang::ValueDecl *d) {
    in_scope[in_scope.size() - 1]->push_back(d);
}

std::vector<clang::ValueDecl *> *OMPToHClib::visibleDecls() {
    std::vector<std::string> alreadyCaptured;
    std::vector<clang::ValueDecl *> *visible =
        new std::vector<clang::ValueDecl *>();

    for (std::vector<std::vector<clang::ValueDecl *> *>::reverse_iterator i =
            in_scope.rbegin(), e = in_scope.rend(); i != e; i++) {
        std::vector<clang::ValueDecl *> *currentScope = *i;
        for (std::vector<clang::ValueDecl *>::iterator ii =
                currentScope->begin(), ee = currentScope->end(); ii != ee;
                ii++) {
            std::string varname = (*ii)->getNameAsString();
            // Deal with variables with the same name but in nested scopes
            if (std::find(alreadyCaptured.begin(), alreadyCaptured.end(),
                        varname) == alreadyCaptured.end()) {
                visible->push_back(*ii);
            }
            alreadyCaptured.push_back(varname);
        }
    }
    return visible;
}

std::string OMPToHClib::getPragmaNameForMarker(const clang::CallExpr *call) {
    if (call == NULL) {
        // Root node only
        return "root";
    }
    assert(call->getNumArgs() == 3);
    const clang::Expr *pragmaNameArg = call->getArg(0);
    while (clang::isa<clang::ImplicitCastExpr>(pragmaNameArg)) {
        pragmaNameArg = clang::dyn_cast<clang::ImplicitCastExpr>(pragmaNameArg)->getSubExpr();
    }
    const clang::StringLiteral *literal = clang::dyn_cast<clang::StringLiteral>(pragmaNameArg);
    assert(literal);
    std::string pragmaName = literal->getString().str();
    return pragmaName;
}

std::string OMPToHClib::getPragmaArgumentsForMarker(const clang::CallExpr *call) {
    assert(call->getNumArgs() == 3);
    const clang::Expr *pragmaArgsArg = call->getArg(1);
    while (clang::isa<clang::ImplicitCastExpr>(pragmaArgsArg)) {
        pragmaArgsArg = clang::dyn_cast<clang::ImplicitCastExpr>(pragmaArgsArg)->getSubExpr();
    }
    const clang::StringLiteral *literal = clang::dyn_cast<clang::StringLiteral>(pragmaArgsArg);
    assert(literal);
    return literal->getString().str();
}

const clang::Stmt *OMPToHClib::getBodyFrom(const clang::CallExpr *call,
        std::string lbl) {
    const clang::Stmt *parent = getParent(call);
    const clang::Stmt *body = NULL;

    if (const clang::CompoundStmt *compound =
            clang::dyn_cast<clang::CompoundStmt>(parent)) {
        clang::CompoundStmt::const_body_iterator i = compound->body_begin();
        clang::CompoundStmt::const_body_iterator e = compound->body_end();
        while (i != e) {
            const clang::Stmt *curr = *i;
            if (curr == call) {
                i++;
                body = *i;
                break;
            }
            i++;
        }
    } else {
        std::cerr << "Unhandled parent while looking for body: " <<
            parent->getStmtClassName() << " of " << lbl << std::endl;
        exit(1);
    }

    assert(body);
    return body;
}

const clang::Stmt *OMPToHClib::getBodyForMarker(const clang::CallExpr *call) {
    std::string pragmaName = getPragmaNameForMarker(call);
    std::string pragmaArgs = getPragmaArgumentsForMarker(call);

    if (pragmaName == "omp") {
        size_t ompPragmaNameEnd = pragmaArgs.find(' ');
        std::string ompPragma;

        if (ompPragmaNameEnd == std::string::npos) {
            // No clauses
            ompPragma = pragmaArgs;
        } else {
            ompPragma = pragmaArgs.substr(0, ompPragmaNameEnd);
        }

        if (ompPragma == "taskwait") {
            return NULL;
        } else if (ompPragma == "task" || ompPragma == "critical" ||
                ompPragma == "atomic" || ompPragma == "parallel" ||
                ompPragma == "single" || ompPragma == "simd" ||
                ompPragma == "master") {
            return getBodyFrom(call, ompPragma);
        } else {
            std::cerr << "Unhandled OMP pragma \"" << ompPragma << "\"" << std::endl;
            exit(1);
        }
    } else if (pragmaName == "omp_to_hclib") {
        return getBodyFrom(call, "omp_to_hclib");
    } else {
        std::cerr << "Unhandled pragma name \"" << pragmaName << "\"" <<
            std::endl;
        exit(1);
    }
}

OMPClauses *OMPToHClib::getOMPClausesForMarker(const clang::CallExpr *call) {
    std::string pragmaName = getPragmaNameForMarker(call);
    assert(pragmaName == "omp");

    std::string pragmaArgs = getPragmaArgumentsForMarker(call);

    size_t ompPragmaNameEnd = pragmaArgs.find(' ');
    std::string ompPragma;

    OMPClauses *parsed = NULL;
    if (ompPragmaNameEnd != std::string::npos) {
        // Some clauses, non-empty args
        std::string clauses = pragmaArgs.substr(ompPragmaNameEnd + 1);
        parsed = new OMPClauses(clauses);
        ompPragma = pragmaArgs.substr(0, ompPragmaNameEnd);
    } else {
        parsed = new OMPClauses();
        ompPragma = pragmaArgs;
    }

    /*
     * This block of code is just here to make sure that if we run into a new
     * OMP clause we haven't tested before an error message prompts us. Since it
     * is decoupled from the actual handling of these clauses, this isn't a
     * particularly safe check.
     */
    for (std::map<std::string, std::vector<SingleClauseArgs *> *>::iterator i =
            parsed->begin(), e = parsed->end(); i != e; i++) {
        std::string clauseName = i->first;
        bool handledClause = false;

        if (ompPragma == "parallel") {
            if (clauseName == "collapse") {
                assert(parsed->hasClause("for"));
                handledClause = true;
            } else if (clauseName == "for" || clauseName == "private" ||
                    clauseName == "shared" || clauseName == "schedule" ||
                    clauseName == "reduction" || clauseName == "firstprivate" ||
                    clauseName == "num_threads" || clauseName == "default") {
                // Do nothing
                handledClause = true;
            }
        } else if (ompPragma == "task") {
            if (clauseName == "firstprivate" || clauseName == "private" ||
                    clauseName == "shared" || clauseName == "untied" ||
                    clauseName == "default" || clauseName == "if") {
                // Do nothing
                handledClause = true;
            } else if (clauseName == "depend") {
                // Handled during code generation.
                handledClause = true;
            }
        } else if (ompPragma == "single") {
            if (clauseName == "private" || clauseName == "nowait") {
                // Do nothing
                handledClause = true;
            }
        }
        if (!handledClause) {
            std::cerr << "Unchecked clause \"" << clauseName <<
                "\" on OMP pragma \"" << ompPragma << "\"" << std::endl;
            exit(1);
        }
    }

    return parsed;
}

void OMPToHClib::VisitStmt(const clang::Stmt *s) {
    clang::SourceLocation start = s->getLocStart();
    clang::SourceLocation end = s->getLocEnd();

    const int currentScope = getCurrentLexicalDepth();
    if (isScopeCreatingStmt(s)) {
        addNewScope();
    }

    if (start.isValid() && end.isValid() && SM->isInMainFile(end)) {
        clang::PresumedLoc presumedStart = SM->getPresumedLoc(start);
        clang::PresumedLoc presumedEnd = SM->getPresumedLoc(end);

        /*
         * Look for OMP calls and make sure the user has to replace them with
         * something appropriate.
         */
        if (const clang::CallExpr *call = clang::dyn_cast<clang::CallExpr>(s)) {
            if (call->getDirectCallee()) {
                const clang::FunctionDecl *callee = call->getDirectCallee();
                std::string calleeName = callee->getNameAsString();
                bool abort = false;
                if (calleeName.find("omp_") == 0) {
                    std::cerr << "Found OMP function call to \"" <<
                        calleeName << "\" on line " <<
                        presumedStart.getLine() << std::endl;
                    abort = true;
                } else if (checkForPthread && calleeName.find("pthread_") == 0 &&
                        std::find(compatiblePthreadAPIs.begin(),
                            compatiblePthreadAPIs.end(), calleeName) ==
                        compatiblePthreadAPIs.end()) {
                    std::cerr << "Found pthread function call to \"" <<
                        calleeName << "\" on line " <<
                        presumedStart.getLine() << std::endl;
                    abort = true;
                }

                if (abort) {
                    std::cerr << "Please replace or delete, then retry the " <<
                        "translation" << std::endl;
                    exit(1);
                }

                // Support for OpenSHMEM
                if (calleeName.find("shmem_") == 0) {
                    if (calleeName == "shmem_malloc" ||
                            calleeName == "shmem_free" ||
                            calleeName == "shmem_n_pes" ||
                            calleeName == "shmem_barrier_all" ||
                            calleeName == "shmem_put64" ||
                            calleeName == "shmem_broadcast64" ||
                            calleeName == "shmem_set_lock" ||
                            calleeName == "shmem_clear_lock" ||
                            calleeName == "shmem_int_get" ||
                            calleeName == "shmem_int_put" ||
                            calleeName == "shmem_getmem" ||
                            calleeName == "shmem_int_add" ||
                            calleeName == "shmem_int_fadd") {
                        // Translate to reference the equivalent OpenSHMEM API in the HClib namespace.
                        const bool failed = rewriter->InsertText(start, "hclib::", true, true);
                        if (failed) {
                            std::cerr << "Failed inserting at " << calleeName <<
                                " at " << presumedStart.getLine() << ":" <<
                                presumedStart.getColumn() << std::endl;
                            exit(1);
                        }
                    } else if (calleeName == "shmem_my_pe") {
                        const bool failed = rewriter->ReplaceText(
                                clang::SourceRange(start, end),
                                "hclib::pe_for_locale(hclib::shmem_my_pe())");
                        assert(!failed);
                    } else if (calleeName == "shmem_init" ||
                            calleeName == "shmem_finalize") {
                        /*
                         * Assume that OpenSHMEM initialization and finalization
                         * is taken care of by the HClib runtime, as part of the
                         * OpenSHMEM module's duties during runtime
                         * initialization and teardown.
                         */
                        const bool failed = rewriter->RemoveText(clang::SourceRange(start, end));
                        assert(!failed);
                    } else {
                        std::cerr << "Found unsupported OpenSHMEM call \"" << calleeName << "\"" << std::endl;
                        exit(1);
                    }

                    anyShmemCalls = true;
                }

                if (calleeName == "hclib_pragma_marker") {
                    const clang::Stmt *body = getBodyForMarker(call);
                    std::vector<clang::ValueDecl *> *captures = visibleDecls();
                    PragmaNode *node = new PragmaNode(call, body, captures, SM);
                    pragmaTree->addChild(node);
                }
            }
        }

        /*
         * First check if this statement is a variable declaration, and if so
         * add it to the list of visible declarations in the current scope. This
         * step must be done before the block of code below for the case where
         * the last statement before an OMP pragma is a variable declaration. We
         * want to make sure that declaration is included in the capture list of
         * the following pragma.
         */
        if (const clang::DeclStmt *decls = clang::dyn_cast<clang::DeclStmt>(s)) {
            for (clang::DeclStmt::const_decl_iterator i = decls->decl_begin(),
                    e = decls->decl_end(); i != e; i++) {
                clang::Decl *decl = *i;
                if (clang::ValueDecl *named = clang::dyn_cast<clang::ValueDecl>(decl)) {
                    addToCurrentScope(named);
                }
            }
        }
    }

    visitChildren(s);

    if (isScopeCreatingStmt(s)) {
        popScope();
    }
    assert(currentScope == getCurrentLexicalDepth());
}

OMPToHClib::OMPToHClib(const char *checkForPthreadStr,
        const char *startCriticalSectionId) {

    if (strcmp(checkForPthreadStr, "true") == 0) {
        checkForPthread = true;
    } else if (strcmp(checkForPthreadStr, "false") == 0) {
        checkForPthread = false;
    } else {
        std::cerr << "Invalid value \"" << std::string(checkForPthreadStr) <<
            "\" provided for -n" << std::endl;
        exit(1);
    }

    criticalSectionId = atoi(startCriticalSectionId);

    compatiblePthreadAPIs.push_back("pthread_mutex_init");
    compatiblePthreadAPIs.push_back("pthread_mutex_lock");
    compatiblePthreadAPIs.push_back("pthread_mutex_unlock");
}

OMPToHClib::~OMPToHClib() {
}
