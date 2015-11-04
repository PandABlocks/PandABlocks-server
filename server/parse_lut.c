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

#include "parse_lut.h"


/* This determines the maximum complexity of an expression.  We have an
 * arbitrary and rather small value here because there's really no point in
 * supporting really complex expressions. */
#define MAX_DEPTH   40


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
 *  reduce: for reduce operators this is the number of tokens consumed. */
#define TOKEN(index, precedence, arity, reduce) \
    ((index) | ((precedence) << 4) | ((arity) << 8) | ((reduce) << 12))
#define TOKEN_INDEX(token)      ((token) & 0xF)
#define TOKEN_PRECEDENCE(token) (((token) >> 4) & 0xF)
#define TOKEN_ARITY(token)      (((token) >> 8) & 0xF)
#define TOKEN_REDUCE(token)     (((token) >> 12) & 0xF)
#define X   0   // Unused fields for token definitions

/* Constant definitions for the operators. */
enum token_symbol {
    TOKEN_CONSTANT  = TOKEN(0, X, 0, 1),     // 0, 1, A-E
    TOKEN_BRA       = TOKEN(1, X, X, X),     // (
    TOKEN_KET       = TOKEN(2, X, 1, 2),     // )
    TOKEN_NOT       = TOKEN(3, X, 1, 1),     // ~
    // All the binary operators have a numerical precedence
    TOKEN_EQ        = TOKEN(4, 5, 2, 1),     // =
    TOKEN_AND       = TOKEN(4, 4, 2, 1),     // &
    TOKEN_XOR       = TOKEN(4, 3, 2, 1),     // ^
    TOKEN_OR        = TOKEN(4, 2, 2, 1),     // |
    TOKEN_IMPLIES   = TOKEN(4, 1, 2, 1),     // =>
    TOKEN_IF        = TOKEN(5, X, X, X),     // ?
    TOKEN_ELSE      = TOKEN(6, X, 3, 2),     // :
    TOKEN_END       = TOKEN(7, X, X, X),     // end of input
};


/* The parse relationship between two adjacent tokens is one of LT, EQ, GT or
 * an error.  LT (<) means that the right hand token needs to be reduced first,
 * EQ (=) means that the two tokens need to be reduced together, and GT (>)
 * means that the token on the left needs to be reduced first. */
enum parse_action {
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
};


/* Precedence parsing table.  A slightly more readable version of the table is
 * here:
 *           K   (   )   ~  bin  ?   :  EOF
 *    ----+--------------------------------
 *     K  |          >       >   >   >   >
 *     (  |  <   <   =   <   <   <
 *     )  |          >       >   >   >   >
 *     ~  |  <   <   >   <   >   >   >   >
 *    bin |  <   <   >   <   *1  >   >   >
 *     ?  |  <   <       <   <   <   =
 *     :  |  <   <   >   <   <   <   >   >
 *    EOF |  <   <       <   <   <       *2
 *
 * *1: Precedence between binary operators depends on the operators.
 * *2: Note that the last entry, EOF-EOF, is not reachable. */
static const char parse_table[8][8] = {
/*            K   (   )   ~   bin ?   :   EOF */
/*            --  --  --  --  --  --  --  --  */
/* K   */   { E1, E1, GT, E1, GT, GT, GT, GT, },
/* (   */   { LT, LT, EQ, LT, LT, LT, E4, E3, },
/* )   */   { E1, E1, GT, E1, GT, GT, GT, GT, },
/* ~   */   { LT, LT, GT, LT, GT, GT, GT, GT, },
/* bin */   { LT, LT, GT, LT, PR, GT, GT, GT, },
/* ?   */   { LT, LT, E5, LT, LT, LT, EQ, E5, },
/* :   */   { LT, LT, GT, LT, LT, LT, GT, GT, },
/* EOF */   { LT, LT, E2, LT, LT, LT, E4,  X, },
};


/* Computes relationship between two tokens, one of LT, EQ, GT, or returns an
 * error code.  The error code can be converted to a parse_lut_status by
 * subtracting ERROR_BASE. */
static enum parse_action lookup_action(int left_token, int right_token)
{
    enum parse_action parse_action =
        parse_table[TOKEN_INDEX(left_token)][TOKEN_INDEX(right_token)];
    if (parse_action == PR)
    {
        /* When two binary operators meet their precedence is decided
         * numerically, and we bind from left to right. */
        if (TOKEN_PRECEDENCE(left_token) < TOKEN_PRECEDENCE(right_token))
            parse_action = LT;
        else
            parse_action = GT;
    }
    return parse_action;
}


/* Input tokeniser.  Consumes one input token from input stream.  If the token
 * has an associated value it is also assigned at the same time.
 *
 * Strictly speaking a token should be represented as a pair: (symbol, [value])
 * where the value is only present when the symbol is a constant.  However in
 * this simple grammar where only one constant is ever waiting to be processed
 * it's slightly simpler to use a separate value.
 *
 * It's important that EOF on input never triggers a push to stack operation,
 * because this function consumes the end of string.  */
static enum parse_lut_status read_token(
    const char **input, int *token, unsigned int *value)
{
    /* Read next character from input string skipping any whitespace. */
    char ch;
    do {
        ch = **input;
        *input += 1;
    } while (ch == ' ');

    enum parse_lut_status status = LUT_PARSE_OK;
    switch (ch)
    {
        /* Constants. */
        case '0': *value = CONSTANT_0; *token = TOKEN_CONSTANT; break;
        case '1': *value = CONSTANT_1; *token = TOKEN_CONSTANT; break;
        case 'A': *value = CONSTANT_A; *token = TOKEN_CONSTANT; break;
        case 'B': *value = CONSTANT_B; *token = TOKEN_CONSTANT; break;
        case 'C': *value = CONSTANT_C; *token = TOKEN_CONSTANT; break;
        case 'D': *value = CONSTANT_D; *token = TOKEN_CONSTANT; break;
        case 'E': *value = CONSTANT_E; *token = TOKEN_CONSTANT; break;

        /* Simple operators. */
        case '(': *token = TOKEN_BRA;   break;
        case ')': *token = TOKEN_KET;   break;
        case '~': *token = TOKEN_NOT;   break;
        case '&': *token = TOKEN_AND;   break;
        case '|': *token = TOKEN_OR;    break;
        case '^': *token = TOKEN_XOR;   break;
        case '?': *token = TOKEN_IF;    break;
        case ':': *token = TOKEN_ELSE;  break;

        case '\0': *token = TOKEN_END;  break;

        /* Compound operator: = or =>. */
        case '=':
            if (**input == '>')
            {
                *input += 1;
                *token = TOKEN_IMPLIES;
            }
            else if (**input == '=')
            {
                /* Allow == as an alias for =. */
                *input += 1;
                *token = TOKEN_EQ;
            }
            else
                *token = TOKEN_EQ;
            break;

        default:
            status = LUT_PARSE_TOKEN_ERROR;
            break;
    }
    return status;
}


/* Reduction and evaluation.  Takes token (and any associated value) together
 * with the stack of values to process.
 *   Note that reduction cannot fail, assuming the precedence table is correct.
 * Firstly, ) and : can only ever be placed on the token stack immediately
 * following ( and ? respectively, so their reductions are safe.  Secondly, the
 * unlisted tokens (, ?, END are never reduced by themselves. */
static void reduce(int token, unsigned int value, unsigned int values[])
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


/* If the incoming token is LT or EQ to the token at the top of the stack then
 * add this token to the top of the stack and read another from the input. */
static enum parse_lut_status push_token(
    const char **input,
    int token_stack[], int *token_sp, int *next_token, unsigned int *value)
{
    /* Push token  The operator stack can overflow at this point if the input is
     * too complex.  Note that this token overflow guard is enough to prevent
     * the value stack from overflowing: because we push at most one value per
     * token. */
    if (*token_sp == MAX_DEPTH)
        return LUT_PARSE_TOO_COMPLEX;
    else
    {
        token_stack[*token_sp] = *next_token;
        *token_sp += 1;

        /* Advance to next input token. */
        return read_token(input, next_token, value);
    }
}


/* If the incoming token is GT the top of the stack then we can consume the
 * tokens at the top of the stack with a reduce action. */
static enum parse_lut_status reduce_stack(
    int this_token, int *token_sp,
    unsigned int value_stack[], int *value_sp, unsigned int value)
{
    /* Reduce current top of stack.  We can exhaust the value stack, but the
     * token stack is safe, see notes for reduce(). */
    if (*value_sp < TOKEN_ARITY(this_token))
        return LUT_PARSE_NO_VALUE;
    else
    {
        /* For values we consume arity values and push one value.  For tokens we
         * just consume.  Note that the value stack is safe from overflow, but
         * this is a property of this particular grammar. */
        *value_sp -= TOKEN_ARITY(this_token) - 1;
        *token_sp -= TOKEN_REDUCE(this_token);
        reduce(this_token, value, &value_stack[*value_sp - 1]);
        return LUT_PARSE_OK;
    }
}


/* Extracts the final result from the value stack. */
static enum parse_lut_status extract_result(
    unsigned int value_stack[], int value_sp, unsigned int *result)
{
    if (value_sp == 1)
    {
        *result = value_stack[0];
        return LUT_PARSE_OK;
    }
    else
        return LUT_PARSE_NO_VALUE;
}


enum parse_lut_status parse_lut(const char *input, unsigned int *result)
{
    /* Stack of operators pending reduction. */
    int token_stack[MAX_DEPTH];
    int token_sp = 1;
    token_stack[0] = TOKEN_END;
    /* Stack of operands. */
    unsigned int value_stack[MAX_DEPTH];
    int value_sp = 0;

    /* Prime the pump by reading one token from the input. */
    int next_token;
    unsigned int value = 0;
    enum parse_lut_status status = read_token(&input, &next_token, &value);

    /* Loop until the stack is empty and the input has been consumed. */
    while (status == LUT_PARSE_OK  &&
           (token_sp > 1  ||  next_token != TOKEN_END))
    {
        int this_token = token_stack[token_sp - 1];
        enum parse_action parse_action = lookup_action(this_token, next_token);
        switch (parse_action)
        {
            case LT:
            case EQ:
                status = push_token(
                    &input, token_stack, &token_sp, &next_token, &value);
                break;

            case GT:
                status = reduce_stack(
                    this_token, &token_sp, value_stack, &value_sp, value);
                break;

            default:
                /* Return syntax error from action. */
                status = parse_action - ERROR_BASE;
                break;
        }
    }

    if (status == LUT_PARSE_OK)
        return extract_result(value_stack, value_sp, result);
    else
        return status;
}


const char *parse_lut_error_string(enum parse_lut_status status)
{
    /* Note that this table of strings must match the entries in the declaration
     * of enum parse_lut_status in parse_lut.h. */
    const char *error_messages[] = {
        "OK",
        "Invalid token",
        "Expression too complex",
        "Missing operator between values",
        "Missing open bracket",
        "Missing close bracket",
        "Missing value",
        "Missing ? before :",
        "Missing : after ?",
    };
    unsigned int count = sizeof(error_messages) / sizeof(error_messages[0]);
    if (status < count)
        return error_messages[status];
    else
        return "Unknown parse status";
}
