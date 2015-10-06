/* This is a parser and evaluator for a simple language used to describe the
 * mapping loaded into a five input lookup table.  The language is a simple
 * operator precedence grammar with the following operations:
 *
 *  variables   = "A" | "B" | "C" | "D" | "E"
 *  constants   = "0" | "1"
 *  unary-ops   = "~"
 *  binary-ops  = "&" | "|" | "=" | "^" | "=>"
 *  ternary-ops = "?:"
 *
 * We give the binary operations precedence according to C, == is allowed as
 * alias for =, and => is the lowest precedence of all except for ?:
 *
 * The parser is a classic precedence operator parser.  The result is a 32-bit
 * number corresponding to a 5-bit binary lookup table. */

#include <stdbool.h>

#include "parse_lut.h"


/* This determines the maximum complexity of an expression.  We have an
 * arbitrary and rather small value here because there's really no point in
 * supporting really complex expressions. */
#define MAX_DEPTH   20


/* Constant definitions for the five variables and two constants. */
#define CONSTANT_0  0x00000000
#define CONSTANT_1  0xffffffff
#define CONSTANT_A  0xffff0000
#define CONSTANT_B  0xff00ff00
#define CONSTANT_C  0xf0f0f0f0
#define CONSTANT_D  0xcccccccc
#define CONSTANT_E  0xaaaaaaaa

/* Each token identifier encodes up to four qualities:
 *  index: this is the index into the operator relation table parse_table.
 *  precedence: for binary operators this determines the final precedence
 *  arity: for reduce operators this is the number of values consumed
 *  reduce: for reduce operators this is the number of extra tokens used. */
#define TOKEN(index, precedence, arity, reduce) \
    ((index) | ((precedence) << 4) | ((arity) << 8) | ((reduce) << 12))
#define TOKEN_INDEX(token)      ((token) & 0xF)
#define TOKEN_PRECEDENCE(token) (((token) >> 4) & 0xF)
#define TOKEN_ARITY(token)      (((token) >> 8) & 0xF)
#define TOKEN_REDUCE(token)     (((token) >> 12) & 0xF)
#define X   0   // Unused fields for token definitions

/* Constant definitions for the operators. */
enum token_symbol {
    TOKEN_CONSTANT  = TOKEN(0, X, 0, 0),     // 0, 1, A-E
    TOKEN_BRA       = TOKEN(1, X, X, X),     // (
    TOKEN_KET       = TOKEN(2, X, 1, 1),     // )
    TOKEN_NOT       = TOKEN(3, X, 1, 0),     // ~
    // All the binary operators have a numerical precedence
    TOKEN_EQ        = TOKEN(4, 5, 2, 0),     // =
    TOKEN_AND       = TOKEN(4, 4, 2, 0),     // &
    TOKEN_XOR       = TOKEN(4, 3, 2, 0),     // ^
    TOKEN_OR        = TOKEN(4, 2, 2, 0),     // |
    TOKEN_IMPLIES   = TOKEN(4, 1, 2, 0),     // =>
    TOKEN_IF        = TOKEN(5, X, X, X),     // ?
    TOKEN_ELSE      = TOKEN(6, X, 3, 1),     // :
    TOKEN_END       = TOKEN(7, X, X, X),     // end of input
};

/* The parse relationship between two adjacent tokens is one of LT, EQ, GT or
 * an error.  LT (<) means that the right hand token needs to be reduced first,
 * EQ (=) means that the two tokens need to be reduced together, and GT (>)
 * means that the token on the left needs to be reduced first. */
enum action {
    LT,     // Push next operator
    EQ,     // Push next operator and reduce together
    GT,     // Reduce current operator
    PR,     // Compare by numerical precedence
    // Parse error codes returned straight to caller
    ERROR_BASE,     // Base for error codes
    E1 = ERROR_BASE + LUT_PARSE_NO_OPERATOR,
    E2 = ERROR_BASE + LUT_PARSE_NO_OPEN,
    E3 = ERROR_BASE + LUT_PARSE_NO_CLOSE,
    E4 = ERROR_BASE + LUT_PARSE_NO_IF,
    E5 = ERROR_BASE + LUT_PARSE_NO_ELSE,
    E6 = ERROR_BASE + LUT_PARSE_ERROR,
};


/* Precedence parsing table. */
static const char parse_table[8][8] = {
/*            K   (   )   ~   bin ?   :   EOF */
/*            --  --  --  --  --  --  --  --  */
/* K   */   { E1, E1, GT, GT, GT, GT, GT, GT, },
/* (   */   { LT, LT, EQ, LT, LT, LT, E4, E3, },
/* )   */   { E1, E1, GT, GT, GT, GT, GT, GT, },
/* ~   */   { LT, LT, GT, LT, GT, GT, GT, GT, },
/* bin */   { LT, LT, GT, LT, PR, GT, GT, GT, },
/* ?   */   { LT, LT, E5, LT, LT, LT, EQ, E5, },
/* :   */   { LT, LT, GT, LT, LT, LT, E4, GT, },
/* EOF */   { LT, LT, E2, LT, LT, LT, E4, E6, },
};


/* Computes relationship between two tokens, one of LT, EQ, GT, or returns an
 * error code.  The error code can be converted to a parse_lut_status by
 * subtracting ERROR_BASE. */
static enum action lookup_action(int left_token, int right_token)
{
    enum action action =
        parse_table[TOKEN_INDEX(left_token)][TOKEN_INDEX(right_token)];
    if (action == PR)
    {
        /* When two binary operators meet their precedence is decided
         * numerically, and we bind from left to right. */
        if (TOKEN_PRECEDENCE(left_token) < TOKEN_PRECEDENCE(right_token))
            return LT;
        else
            return GT;
    }
    else
        return action;
}


/* Input tokeniser.  Consumes one input token from input stream.  If the token
 * has an associated value it is also assigned at the same time.
 *
 * Strictly speaking a token should be represented as a pair: (symbol, [value])
 * where the value is only present when the symbol is a constant.  However in
 * this simple grammar where only one constant is ever waiting to be processed
 * it's slightly simpler to use a separate value. */
static bool read_token(const char **input, int *token, int *value)
{
    /* Skip any whitespace. */
    while (**input == ' ')
        *input += 1;

    if (**input == '\0')
    {
        *token = TOKEN_END;
        return true;
    }
    else
    {
        char ch = **input;
        *input += 1;

        switch (ch)
        {
            /* Constants. */
            case '0': *value = CONSTANT_0; *token = TOKEN_CONSTANT; return true;
            case '1': *value = CONSTANT_1; *token = TOKEN_CONSTANT; return true;
            case 'A': *value = CONSTANT_A; *token = TOKEN_CONSTANT; return true;
            case 'B': *value = CONSTANT_B; *token = TOKEN_CONSTANT; return true;
            case 'C': *value = CONSTANT_C; *token = TOKEN_CONSTANT; return true;
            case 'D': *value = CONSTANT_D; *token = TOKEN_CONSTANT; return true;
            case 'E': *value = CONSTANT_E; *token = TOKEN_CONSTANT; return true;

            /* Simple operators. */
            case '(': *token = TOKEN_BRA;  return true;
            case ')': *token = TOKEN_KET;  return true;
            case '~': *token = TOKEN_NOT;  return true;
            case '&': *token = TOKEN_AND;  return true;
            case '|': *token = TOKEN_OR;   return true;
            case '^': *token = TOKEN_XOR;  return true;
            case '?': *token = TOKEN_IF;   return true;
            case ':': *token = TOKEN_ELSE; return true;

            /* Compound operator: = or =>. */
            case '=':
                if (**input == '>')
                {
                    *input += 1;
                    *token = TOKEN_IMPLIES;
                }
                else if (**input == '=')
                {
                    *input += 1;
                    *token = TOKEN_EQ;
                }
                else
                    *token = TOKEN_EQ;
                return true;

            default:
                return false;
        }
    }
}


/* Reduction and evaluation.  Takes token (and any associated value) together
 * with the stack of values to process.
 *   Note that reduction cannot fail, assuming the precedence table is correct.
 * Firstly, ) and : can only ever be placed on the token stack immediately
 * following ( and ? respectively, so their reductions are safe.  Secondly, the
 * unlisted tokens (, ?, END are never reduced by themselves. */
void reduce(int token, int value, int values[])
{
    switch (token)
    {
        /* Simple evaluation actions. */
        case TOKEN_CONSTANT:    values[0] = value;                      break;
        case TOKEN_NOT:         values[0] = ~values[0];                 break;
        case TOKEN_AND:         values[0] = values[0] & values[1];      break;
        case TOKEN_OR:          values[0] = values[0] | values[1];      break;
        case TOKEN_EQ:          values[0] = ~(values[0] ^ values[1]);   break;
        case TOKEN_XOR:         values[0] = values[0] ^ values[1];      break;
        case TOKEN_IMPLIES:     values[0] = ~values[0] | values[1];     break;

        /* Compound reductions. */
        case TOKEN_KET:
            break;
        case TOKEN_ELSE:
            values[0] = (values[0] & values[1]) | (~values[0] & values[2]);
            break;
    }
}



enum parse_lut_status parse_lut(const char *input, int *result)
{
    /* Stack of operators pending reduction. */
    int token_stack[MAX_DEPTH] = {TOKEN_END};
    int token_sp = 1;
    /* Stack of operands. */
    int value_stack[MAX_DEPTH];
    int value_sp = 0;

    /* Prime the pump by reading one token from the input. */
    int next_token;
    int value;
    if (!read_token(&input, &next_token, &value))
        return LUT_PARSE_TOKEN_ERROR;

    /* Loop until the stack is empty and the input has been consumed. */
    while (token_sp > 1  ||  next_token != TOKEN_END)
    {
        int this_token = token_stack[token_sp - 1];
        enum action action = lookup_action(this_token, next_token);
        switch (action)
        {
            case LT:
            case EQ:
                /* Push token and value, if present.  The operator stack can
                 * overflow at this point if the input is too complex. */
                if (token_sp == MAX_DEPTH)
                    return LUT_PARSE_TOO_COMPLEX;
                token_stack[token_sp++] = next_token;

                /* Advance to next input token. */
                if (!read_token(&input, &next_token, &value))
                    return LUT_PARSE_TOKEN_ERROR;
                break;

            case GT:
                /* Reduce current top of stack.  We can exhaust the value stack,
                 * but the token stack is safe. */
                if (value_sp < TOKEN_ARITY(this_token))
                    return LUT_PARSE_NO_VALUE;

                /* For values we consume arity values and push one value.  For
                 * tokens we just consume, and the top token isn't counted. */
                value_sp -= TOKEN_ARITY(this_token) - 1;
                token_sp -= TOKEN_REDUCE(this_token) + 1;
                reduce(this_token, value, &value_stack[value_sp - 1]);
                break;

            default:
                /* Return syntax error from action. */
                return action - ERROR_BASE;
        }
    }

    if (value_sp == 1)
    {
        *result = value_stack[0];
        return LUT_PARSE_OK;
    }
    else
        return LUT_PARSE_NO_VALUE;
}
