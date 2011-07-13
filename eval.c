/***************************************************************/
/* File: eval.c                                                */
/* Implementation of evaluation the expression.                */
/* Author: Minjie Zha                                          */
/***************************************************************/
#include "globals.h"
#include "util.h"
#include "varset.h"
#include "builtin.h"
#include "primitive.h"
#include "stdlib.h" // standard library
#include "cek_machine.h"
#include "eval.h"

static TreeNode* resolveFunction(const char* name);
static Closure* lookupVariable(const char *name, Environment *env);
static TreeNode* resolveFreeVariables(TreeNode *expr, Environment *env);
static VarSet * FV(TreeNode *expr);
static TreeNode *substitute(TreeNode *expr, TreeNode *var, TreeNode *sub);
static int _evaluate(State *state);
static Environment *buildGlobalEnvironment();


TreeNode * evaluate(TreeNode *expr) {
    Environment *globalEnv = buildGlobalEnvironment();
    State *state = cek_newState();
    state->closure = cek_newClosure(expr,globalEnv);

    TreeNode* result = NULL;
    if(_evaluate(state)) {
        // The result control string may contain free variables, need to 
        // substitute them using the environment for it.
        TreeNode *tmp = resolveFreeVariables(state->closure->expr,state->closure->env);
        if(tmp!=NULL) {
            // actually, tmp and state->closure->expr are the same expression
            result = tmp;
            state->closure->expr = NULL;
        }
    }
    cek_cleanup(state);
    return result;
}

/* 
 * Evaluates the expression in the state. 
 * Returns 1 if success, otherwise returns 0.
 */
static int _evaluate(State *state) {
    Continuation * ctn = NULL;
    Closure *closure = NULL;
    Environment *env = NULL;
    while(!cek_canTerminate(state)) {
        if(state->closure->expr->kind==IdK) {
            // Find mapped closure from the evironment
            closure = lookupVariable(state->closure->expr->name,state->closure->env);
            if(closure==NULL) {
                // find function from builtin and standard library
                TreeNode* fun = resolveFunction(state->closure->expr->name);
                if(fun==NULL) {
                    fprintf(errOut, "Error: %s is not a defined variable or function.\n", state->closure->expr->name);
                    return 0;
                }
                // Replace the function name with function definition
                deleteTreeNode(state->closure->expr);
                state->closure->expr = fun;
            } else {
                /*
                 * Should not use the found closure directly, because it will
                 * be deleted after evaluted and the mapped id may be used
                 * again afterwards. So does the expression in it.
                 */
                closure = cek_newClosure(duplicateTree(closure->expr),closure->env);
                // replace the closure with mapped closure.
                deleteTreeNode(state->closure->expr);
                cek_deleteClosure(state->closure);
                state->closure = closure;
            }
        } else if(isValue(state->closure->expr)) {
            if(state->continuation->tag==ArgKK) {
                if(state->closure->expr->kind==ConstK) {
                    fprintf(errOut, "Error: cannot apply a constant to any argument.\n");
                    fprintf(errOut, "Expression:\t");
                    printExpression(state->closure->expr,errOut);
                    fprintf(errOut,"\n");
                    return 0;
                }else if(state->closure->expr->kind==IdK) {
                    // find function from builtin and standard library
                    TreeNode* fun = resolveFunction(state->closure->expr->name);
                    if(fun==NULL) {
                        fprintf(errOut, "Error: %s is not a predefined function.\n", state->closure->expr->name);
                        return 0;
                    }
                    // replace the expression with function definition
                    deleteTreeNode(state->closure->expr);
                    state->closure->expr = fun;
                } else {
                    // pop the continuation
                    ctn = state->continuation;
                    state->continuation = ctn->next;
                    env = cek_newEnvironment(state->closure->expr->children[0]->name,ctn->closure,state->closure->env);
                    closure = cek_newClosure(state->closure->expr->children[1],env);
                    state->closure->expr->children[1] = NULL;
                    deleteTree(state->closure->expr);
                    cek_deleteClosure(state->closure);
                    cek_deleteContinuation(ctn);

                    state->closure = closure;
                }
            } else if(state->continuation->tag==OprKK) {
                ctn = state->continuation;
                // only perform primitive operation if operands are constants
                if(ctn->closure->expr->children[0]->kind==ConstK 
                    && state->closure->expr->kind==ConstK) {
                    state->continuation = ctn->next;
                    // reattach the second operand
                    ctn->closure->expr->children[1] = state->closure->expr;
                    TreeNode* tmp  = evalPrimitive(ctn->closure->expr);
                    cek_deleteClosure(state->closure);
                    state->closure = cek_newClosure(tmp,NULL);

                    deleteTree(ctn->closure->expr);
                    cek_deleteClosure(ctn->closure);
                    cek_deleteContinuation(ctn);
                } else {
                    fprintf(errOut, "Error: %s can only be applied on constants.\n", ctn->closure->expr->name);
                    return 0;
                }
            } else if(state->continuation->tag==OpdKK) {
                state->continuation->tag = OprKK;
                // switch environment and expression
                env = state->continuation->closure->env;
                state->continuation->closure->env = state->closure->env;
                state->closure->env = env;
                // attach the expression back to the PrimiK node
                state->continuation->closure->expr->children[0] = state->closure->expr;
                state->closure->expr = state->continuation->closure->expr->children[1];
                // dettach the operand from PrimiK node
                state->continuation->closure->expr->children[1] = NULL;
            } else {
                fprintf(errOut,"Error: Unknown continuation tag.\n");
                return 0;
            }
        } else if(state->closure->expr->kind==AppK) {
            ctn = cek_newContinuation(ArgKK);
            ctn->closure = cek_newClosure(state->closure->expr->children[1],state->closure->env);
            ctn->next = state->continuation;
            state->continuation = ctn;
            TreeNode *tmp = state->closure->expr;
            state->closure->expr = tmp->children[0];
            deleteTreeNode(tmp);
        } else if(state->closure->expr->kind==PrimiK) {
            ctn = cek_newContinuation(OpdKK);
            ctn->closure = state->closure;
            ctn->next = state->continuation;
            state->continuation = ctn;
            state->closure = cek_newClosure(state->closure->expr->children[0],state->closure->env);
            state->closure->expr->children[0] = NULL;   // dettach
        }
    }

    return 1;
}

TreeNode * alphaConversion(TreeNode *expr) {
    if(expr->kind!=AbsK) {
        fprintf(errOut,"Alpha conversion can only be applied to abstraction expression.\n");
        return expr;
    }

    VarSet* set = FV(expr->children[1]);
    char *name = NULL;
    int len = strlen(expr->children[0]->name);
    int attempts = 0;
    // pick a new name
    do {
        if(name!=NULL) free(name);  // free the last attempt
        attempts++;
        name = malloc(len+attempts);
        strcpy(name,expr->children[0]->name);
        int a;
        // append '_' to the original name
        for(a=0;a<attempts;a++) {
            strcat(name,"_");
        }
    } while(strcmp(name,expr->children[0]->name)==0 || contains(set,name)==1);

    TreeNode *var = newTreeNode(IdK);
    var->name = name;
    TreeNode *result = substitute(expr->children[1], expr->children[0], var);
    expr->children[1] = result;
    deleteTree(expr->children[0]);
    expr->children[0] = var;
    return expr;
}

TreeNode * betaReduction(TreeNode *expr) {
    if(expr->kind!=AppK) {
        fprintf(errOut,"Beta reduction can only be applied to application expression.\n");
        return expr;
    }
    TreeNode* left = expr->children[0];
    if(left->kind==IdK || left->kind==AppK) {
        return expr;
    }else if(left->kind==AbsK) {
        TreeNode* result = substitute(left->children[1],left->children[0],expr->children[1]);
        // If result == left->children[1], prevent deleting the result;
        // If result != left->children[1], already deleted in substitute(), so
        // prevent double free.
        left->children[1] = NULL;
        deleteTree(expr);
        return result;
    }
    return expr;
}

/* == Definitions of the local functions. */
/* Gets the free variables in the expression. */ 
static VarSet * FV(TreeNode *expr) {
    VarSet* set = NULL;
    VarSet* set1 = NULL;
    VarSet* set2 = NULL;
    switch(expr->kind) {
        case IdK:
            set = newVarSet();
            addVar(set,expr->name);
            break;
        case ConstK:
            set = newVarSet();
            break;
        case AbsK:
            set = FV(expr->children[1]);
            deleteVar(set,expr->children[0]->name);
            break;
        case AppK:
        case PrimiK:
            set = newVarSet();
            set1 = FV(expr->children[0]);
            set2 = FV(expr->children[1]);
            unionVarSet(set,set1,set2);
            deleteVarSet(set1);
            deleteVarSet(set2);
            break;
        default:
            fprintf(errOut,"Unknown expression type.\n");
    }
    return set;
}

/* Performs substitution on the expression. */
static TreeNode *substitute(TreeNode *expr, TreeNode *var, TreeNode *sub) {
    if(expr==NULL || var==NULL || sub==NULL) return expr;

    if(var->kind!=IdK) {
        fprintf(errOut,"The replaced expression is not a variable.\n");
        return expr;
    }
    const char * parname = NULL;
    TreeNode * result = NULL;
    switch(expr->kind) {
        case IdK:
            if(strcmp(expr->name,var->name)==0) {
                deleteTree(expr);
                return duplicateTree(sub);
            }else {
                return expr;
            }
        case ConstK:
            return expr;
        case AbsK:
            parname = expr->children[0]->name;
            if(strcmp(parname,var->name)!=0) {
                VarSet* set = FV(sub); 
                while(contains(set,parname)) {  // do alpha conversion
                    expr = alphaConversion(expr);
                    parname = expr->children[0]->name;
                }
                result = substitute(expr->children[1],var,sub);
                expr->children[1] = result;
                deleteVarSet(set);
            }
            return expr;
        case AppK:
        case PrimiK:
            result = substitute(expr->children[0],var,sub);
            expr->children[0] = result;
            result = substitute(expr->children[1],var,sub);
            expr->children[1] = result;
            return expr;
        default:
            fprintf(errOut,"Unknown expression type.\n");
    }
    return expr;
}

/* Search a function in builtin library and standard library by name. */
static TreeNode* resolveFunction(const char* name) {
    BuiltinFun* fun = NULL;
    StandardFun* stdFun = NULL;
    if((fun=lookupBuiltinFun(name))!=NULL) {
        return (fun->expandFun)();
    } else if((stdFun=lookupStandardFun(name))!=NULL) {
        return expandStandardFun(stdFun);
    }
    return NULL;
}

/*
 * Lookup closure by name in the environment or its parent.
 */
static Closure* lookupVariable(const char *name, Environment *env) {
    if(env==NULL) return NULL;
    if(strcmp(name,env->name)==0) {
        Closure *closure = env->closure;
        if(closure->env!=NULL) {
            // evaluates the expression in its environment
            State *state = cek_newState();
            state->closure = cek_newClosure(duplicateTree(closure->expr),cek_newEnvironment("",cek_newClosure(NULL,NULL),closure->env));

            if(_evaluate(state)) {
                deleteTree(closure->expr);
                cek_deleteClosure(closure);
                // save the evaluated result
                env->closure = cek_newClosure(state->closure->expr,state->closure->env);
                state->closure->expr = NULL;
            }
            cek_cleanup(state);
        } 
        return env->closure;
    }
    return lookupVariable(name,env->parent);
}

/*
 * Substitutes free variables in the expression using mappings in the
 * environment.
 */
static TreeNode* resolveFreeVariables(TreeNode *expr, Environment *env) {
    // get the free variable set
    VarSet* set = FV(expr);
    VarSetList *list = vs_asList(set);
    deleteVarSet(set);

    TreeNode *result = expr;
    Closure *closure = NULL;
    VarSetList *l = list;
    while(l!=NULL) {
        TreeNode *fun = resolveFunction(l->name);
        if(fun!=NULL) { // builtin and standard functions are not FV
            deleteTree(fun);
            l = l->next;
            continue;
        }
        closure = lookupVariable(l->name,env);
        if(closure==NULL) {
            fprintf(errOut,"Error: Variable %s is not defined.\n",l->name);
            result = NULL;
            break;
        } else {
            TreeNode *tmp = resolveFreeVariables(closure->expr,closure->env);
            if(tmp==NULL) {
                result = NULL;
                break;
            }
            TreeNode *var = newTreeNode(IdK);
            var->name = stringCopy(l->name);
            result = substitute(result,var,tmp);
            deleteTree(var);
        }
        l = l->next;
    }
    vs_deleteVarSetList(list);
    return result;
}

static Environment *buildGlobalEnvironment() {
    Environment *ret = NULL;
    int size = 0;
    BuiltinFun *funs = builtinFuns(&size);
    int i;
    for(i=0;i<size;i++) {
        BuiltinFun fun = funs[i];
        ret = cek_newEnvironment(fun.name,cek_newClosure((fun.expandFun)(),NULL),ret);
    }

    StandardFun *stdFuns = standardFuns(&size);
    for(i=0;i<size;i++) {
        StandardFun fun = stdFuns[i];
        ret = cek_newEnvironment(fun.name,cek_newClosure(expandStandardFun(&fun),NULL),ret);
    }

    return ret;
}
