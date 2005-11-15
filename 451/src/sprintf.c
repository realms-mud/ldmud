/*---------------------------------------------------------------------------
 * sprintf for LPMud
 *
 * Implemented and put into the public domain by Lynscar (Sean A Reith).
 *---------------------------------------------------------------------------
 * An implementation of (s)printf() for LPC, with quite a few
 * extensions (some parameters have slightly different meaning or
 * restrictions to "standard" (s)printf.)
 *
 * This version supports the following as modifiers:
 *  " "   pad positive integers with a space.
 *  "+"   pad positive integers with a plus sign.
 *  "-"   left aligned within field size.
 *        NB: std (s)printf() defaults to right alignment, which is
 *            unnatural in the context of a mainly string based language
 *            but has been retained for "compatability" ;)
 *  "|"   centered within field size.
 *  "$"   justified to field size. Makes sense only for strings and columns
 *        of strings.
 *  "="   column mode if strings are greater than field size.  this is only
 *        meaningful with strings, all other types are ignored. The strings
 *        are broken into the size of 'precision', and the last line is
 *        padded to have a length of 'fs'.
 *        columns are auto-magically word wrapped.
 *  "#"   table mode, print a list of '\n' separated 'words' in a
 *        compact table within the field size.  Only meaningful with strings.
 *   n    specifies the field size, a '*' specifies to use the corresponding
 *        arg as the field size.  If n is prepended with a zero, then the field
 *        is printed with leading zeros.
 *  "."n  precision of n, simple strings truncate after this (if precision is
 *        greater than field size, then field size = precision), tables use
 *        precision to specify the number of columns (if precision not specified
 *        then tables calculate a best fit), all other types ignore this.
 *  ":"n  n specifies the fs _and_ the precision, if n is prepended by a zero
 *        then it is padded with zeros instead of spaces.
 *  "@"   the argument is an array.  the corresponding format_info (minus the
 *        "@") is applyed to each element of the array.
 *  "'X'" The char(s) between the single-quotes are used to pad to field
 *        size (defaults to space) (if both a zero (in front of field
 *        size) and a pad string are specified, the one specified second
 *        overrules).
 *        To include "'" in the pad string, you must use "\\'" (as the
 *        backslash has to be escaped past the interpreter), similarly, to
 *        include "\" requires "\\\\".
 * The following are the possible type specifiers.
 *  "%"   in which case no arguments are interpreted, and a "%" is inserted, and
 *        all modifiers are ignored.
 *  "O"   the argument is an LPC datatype.
 *  "Q"   the argument is an LPC datatype, strings are printed in LPC notation.
 *  "s"   the argument is a string.
 *  "d"   the integer arg is printed in decimal.
 *  "i"   as d.
 *  "c"   the integer arg is to be printed as a character.
 *  "o"   the integer arg is printed in octal.
 *  "x"   the integer arg is printed in hex.
 *  "X"   the integer arg is printed in hex (in capitals).
 * "e","E","f","F","g","G"
 *        floating point formatting like in C.
 *  "^"   prints "%^" for compatibility with terminal_colour() strings.
 *        No modifiers are allowed.
 *---------------------------------------------------------------------------
 */

#include "driver.h"
#include "typedefs.h"
 
#include "my-alloca.h"
#include <stdio.h>
#include <setjmp.h>
#include <sys/types.h>

#define NO_REF_STRING
#include "sprintf.h"

#include "array.h"
#include "closure.h"
#include "exec.h"
#include "interpret.h"
#include "instrs.h"
#include "main.h"
#include "mapping.h"
#include "object.h"
#include "ptrtable.h"
#include "random.h"
#include "sent.h"
#include "simulate.h"
#include "simul_efun.h"
#include "stdstrings.h"
#include "stralloc.h"
#include "swap.h"
#include "xalloc.h"

/* If this #define is defined then error messages are returned,
 * otherwise error() is called (ie: A "wrongness in the fabric...")
 */
#undef RETURN_ERROR_MESSAGES

/*-------------------------------------------------------------------------*/
/* Format of format_info:
 *
 *   00000000 0000xxxx : argument type INFO_T:
 *                                0000 : type not found yet;
 *                                0001 : error type not found;
 *                                0010 : percent sign, null argument;
 *                                0011 : LPC datatype;
 *                                0100 : string;
 *                                0101 : integer;
 *                                0110 : float
 *   00000000 00xx0000 : aligment INFO_A:
 *                                00 : right;
 *                                01 : centre;
 *                                10 : left;
 *                                11 : justified;
 *   00000000 xx000000 : positive pad char INFO_PP:
 *                                00 : none;
 *                                01 : ' ';
 *                                10 : '+';
 *   0000000x 00000000 : array mode (INFO_ARRAY)?
 *   000000x0 00000000 : column mode (INFO_COLS)?
 *   00000x00 00000000 : table mode (INFO_TABLE)?
 *
 *   0000x000 00000000 : field is to be left-padded with zero
 *   000x0000 00000000 : pad-spaces before a newline are kept
 */

typedef unsigned int format_info;

#define INFO_T        0xF
#define INFO_T_ERROR  0x1
#define INFO_T_NULL   0x2
#define INFO_T_LPC    0x3
#define INFO_T_QLPC   0x4
#define INFO_T_STRING 0x5
#define INFO_T_INT    0x6
#define INFO_T_FLOAT  0x7

#define INFO_A         0x30 /* Right alignment */
#define INFO_A_CENTRE  0x10
#define INFO_A_LEFT    0x20
#define INFO_A_JUSTIFY 0x30

#define INFO_PP       0xC0
#define INFO_PP_SPACE 0x40
#define INFO_PP_PLUS  0x80

#define INFO_ARRAY    0x100
#define INFO_COLS     0x200
#define INFO_TABLE    0x400

#define INFO_PS_ZERO  0x800
#define INFO_PS_KEEP  0x1000

/*-------------------------------------------------------------------------*/

#define BUFF_SIZE 0x20000  /* 128 KByte */
  /* Max size of returned string.
   */

/* The error handling */

#define ERROR(x) (longjmp(st->error_jmp, (x)))

#define ERR_ID_NUMBER            0xFFFF /* Mask for the error number */
#define ERR_ARGUMENT         0xFFFF0000 /* Mask for the arg number */

#define ERR_BUFF_OVERFLOW       0x1     /* buffer overflowed */
#define ERR_TO_FEW_ARGS         0x2     /* more arguments spec'ed than passed */
#define ERR_INVALID_STAR        0x3     /* invalid arg to * */
#define ERR_PREC_EXPECTED       0x4     /* expected precision not found */
#define ERR_INVALID_FORMAT_STR  0x5     /* error in format string */
#define ERR_INCORRECT_ARG       0x6     /* invalid arg to %[idcxXs] */
#define ERR_CST_REQUIRES_FS     0x7     /* field size not given for c/t */
#define ERR_UNDEFINED_TYPE      0x8     /* undefined type found */
#define ERR_QUOTE_EXPECTED      0x9     /* expected ' not found */
#define ERR_UNEXPECTED_EOS      0xA     /* fs terminated unexpectedly */
#define ERR_NULL_PS             0xB     /* pad string is null */
#define ERR_ARRAY_EXPECTED      0xC     /* array expected */
#define ERR_NOMEM               0xD     /* Out of memory */


#define ERROR1(e,a)              ERROR((e) | (a)<<16)
#define EXTRACT_ERR_ARGUMENT(i)  ((i)>>16)

/*-------------------------------------------------------------------------*/
/* Types */

typedef struct SaveChars        savechars;
typedef struct ColumnSlashTable cst;
typedef struct sprintf_buffer   sprintf_buffer_t;
typedef struct stsf_locals      stsf_locals_t;
typedef struct fmt_state        fmt_state_t;


/* --- struct SaveChars: list of characters to restore before exiting.
 */
struct SaveChars
{
    char       what;   /* Saved character */
    char      *where;  /* Original position */
    savechars *next;
};


/* --- struct ColumnSlashTable: data for one column or table
 *
 * All tables and columns in one line are kept in a linked
 * list and added line-wise to the result string.
 */

struct ColumnSlashTable
{
    union CSTData {
        char  *col;  /* column data, possibly multiple lines */
        char **tab;  /* table data */
    } d;                     /* d == data */
    unsigned short  nocols;  /* number of columns in table *sigh* */
    char           *pad;     /* the pad string */
    size_t          start;   /* starting cursor position */
    size_t          size;    /* column/table width */
    int             pres;    /* precision */
    format_info     info;    /* formatting data */
    cst            *next;    /* next column structure */
};

/* --- struct sprintf_buffer: dynamic string buffer
 *
 * The structure implements a dynamic string buffer. The structure
 * sits at the end of the allocated memory and knows where the
 * char* pointing to the begin of the allocated memory is. This
 * way all the user sees is a char[] which automagically happens
 * to be of the right size.
 */
 
struct sprintf_buffer
{
    /* char text[.size - sizeof(sprintf_buffer_t)]; */
#define BUF_TEXT(b) ((char *)(b))
    int offset;    /* Offset from .size to the first free byte
                    * (ie. a negative number).
                    */
    int size;      /* Total size of the buffer */
    char **start;  /* Pointer to the string pointer */
};


/* --- struct stsf_locals: auxiliary structure
 *
 * The structure is used when printing a mapping to pass
 * the essential data.
 */

struct stsf_locals
{
    sprintf_buffer_t *spb;  /* Target buffer */
    int indent;             /* Indentation */
    int num_values;         /* Mapping width */
    Bool quote;             /* TRUE: Quote strings */
    fmt_state_t      *st;   /* sprintf state */
};

/* --- struct fmt_state: status of the sprintf operation
 *
 * This structure contains several data items which the functions
 * here use and modify while creating the result string.
 * The structure is allocated by the string_print_formatted()
 * in order to achieve re-entrancy.
 *
 * The canonic declaration for this structure as parameter
 * is 'fmt_state_t *st', and is expected as such by some
 * macros.
 */

struct fmt_state
{
    savechars  * saves;            /* Characters to restore */
    cst        * csts;             /* list of columns/tables to be done */
    svalue_t     clean;            /* holds a temporary string */
    char         buff[BUFF_SIZE];  /* Buffer for returned string. */
    size_t       bpos;             /* Position in buff. */
    ssize_t      sppos;
      /* -1, or the buffer position of the first character of
       * a trailing space padding. This is used to remove space
       * padding at the end of lines.
       */
    unsigned int line_start;       /* Position where to start a line. */
    jmp_buf      error_jmp;
      /* Error-exit context. In case of errors, the functions longjmp()
       * directly back to the top function which then handles the error.
       */
    struct pointer_table * ptable;
      /* When printing svalue, this keeps track of arrays and mappings
       * in order to catch recursions.
       */
    int32 pointer_id;
      /* Next ID to give to an array or mapping when printing svalues.
       */
};

/*-------------------------------------------------------------------------*/
/* Forward declarations */

static sprintf_buffer_t *svalue_to_string(fmt_state_t *
                                         , svalue_t *, sprintf_buffer_t *
                                         , int, Bool, Bool);

/*-------------------------------------------------------------------------*/
/* Macros */
#define ADD_CHAR(x) {\
    if (st->bpos >= BUFF_SIZE) ERROR(ERR_BUFF_OVERFLOW); \
    if (x == '\n' && st->sppos != -1) st->bpos = st->sppos; \
    st->sppos = -1; \
    st->buff[st->bpos++] = x;\
}

  /* Add character <x> to the buffer.
   */

/*-------------------------------------------------------------------------*/
#define ADD_STRN(s, n) { \
    if (st->bpos + n > BUFF_SIZE) ERROR(ERR_BUFF_OVERFLOW); \
    if (n >= 1 && (s)[0] == '\n' && st->sppos != -1) st->bpos = st->sppos; \
    st->sppos = -1; \
    strncpy(st->buff+st->bpos, (s), n); \
    st->bpos += n; \
}

  /* Add the <n> characters from <s> to the buffer.
   */

/*-------------------------------------------------------------------------*/
#define ADD_CHARN(c, n) { \
    /* n must not be negative! */ \
    if (st->bpos + n > BUFF_SIZE) ERROR(ERR_BUFF_OVERFLOW); \
    if (n >= 1 && c == '\n' && st->sppos != -1) st->bpos = st->sppos; \
    st->sppos = -1; \
    memset(st->buff+st->bpos, c, n); \
    st->bpos += n; \
}

  /* Add character <c> <n>-times to the buffer.
   */

/*-------------------------------------------------------------------------*/
#define ADD_PADDING(pad, N) { \
    int n = (N); \
\
    if (!pad[1]) { \
        ADD_CHARN(*pad, n) \
    } else { \
        int l; \
\
        l = strlen(pad); \
        for (i=0; --n >= 0; ) { \
            if (pad[i] == '\\') \
                i++; \
            ADD_CHAR(pad[i]); \
            if (++i == l) \
                i = 0; \
        } \
    } \
}

  /* Add the padding string <pad> to the buffer, repeatedly if necessary,
   * yielding a total length of <N>.
   */

/*-------------------------------------------------------------------------*/
static sprintf_buffer_t *
realloc_sprintf_buffer (fmt_state_t *st, sprintf_buffer_t *b)

/* Increase the size of buffer <b> and return the new buffer.
 * At the time of call, a positive .offset determines how much more
 * data is needed. If it's negative to begin with, the size is just
 * doubled.
 */

{
    int offset   = b->offset;
    int size     = b->size;
    char **start = b->start;
    char *newstart;

    /* Get more memory */
    do {
        if (size > BUFF_SIZE)
            ERROR(ERR_BUFF_OVERFLOW);
        offset -= size;
        size *= 2;
        newstart = rexalloc(*start, size);
        if (!newstart)
            ERROR(ERR_NOMEM);
        *start = newstart;
    } while (offset >= 0);

    b = (sprintf_buffer_t*)(*start+size - sizeof(sprintf_buffer_t));
    b->offset = offset;
    b->size = size;
    b->start = start;
    return b;
} /* realloc_sprintf_buffer() */

/*-------------------------------------------------------------------------*/
static void
stradd (fmt_state_t *st, sprintf_buffer_t **buffer, char *add)

/* Add string <add> to the <buffer>.
 * The function add_indent() intentionally matches our signature.
 */

{
    sprintf_buffer_t *b = *buffer;
    int o;
    int len;

    len = strlen(add);
    o = b->offset;
    if ( (b->offset = o + len) >= 0)
    {
        *buffer = b = realloc_sprintf_buffer(st, b);
        o = b->offset - len;
    }
    strcpy(BUF_TEXT(b) + o, add);
} /* stradd() */

/*-------------------------------------------------------------------------*/
static void
numadd (fmt_state_t *st, sprintf_buffer_t **buffer, int num)

/* Add the <num>ber to the <buffer>.
 */

{
    sprintf_buffer_t *b = *buffer;
    int i, j;
    Bool nve;

    if (num < 0)
    {
        /* Negative number: remember that and make
         * the number positive.
         */
        if ( (num *= -1) < 0)
        {
            /* num == MININT: add <num>+1 to the buffer, the
             * increment the last digit by one.
             * Since all possible MIN values are powers of two,
             * the last digit can never be 9.
             */
            numadd(st, buffer, num+1);
            b = *buffer;
            BUF_TEXT(b)[b->offset - 1] += 1;
            return;
        }
        nve = MY_TRUE;
    }
    else
        nve = MY_FALSE;

    /* Determine the number of digits required */
    if (num <= 1)
        j = 1;
    else
        j = (int) ceil(log10((double)num+1));
    if (nve)
        j++;

    /* Get the memory */
    i = b->offset;
    if ((b->offset = i + j) >= 0) {
        *buffer = b = realloc_sprintf_buffer(st, b);
        i = b->offset - j;
    }

    BUF_TEXT(b)[i+j] = '\0'; /* Add terminator in advance */

    /* '-' required? */
    if (nve)
    {
        BUF_TEXT(b)[i] = '-';
        j--;
    }
    else
        i--;

    /* Now store the number */
    for (; j; j--, num /= 10)
        BUF_TEXT(b)[i+j] = (num%10) + '0';
} /* num_add() */

/*-------------------------------------------------------------------------*/
static void
add_indent (fmt_state_t *st, sprintf_buffer_t **buffer, int indent)

/* Add <indent> characters indentation to <buffer>.
 * The function intentionally matches the signature of stradd().
 */

{
    int i;
    sprintf_buffer_t *b = *buffer;
    char *p;

    i = b->offset;
    if ( (b->offset = i + indent) >= 0)
    {
        *buffer = b = realloc_sprintf_buffer(st, b);
        i = b->offset - indent;
    }

    p = BUF_TEXT(b) + i;
    for (;indent;indent--)
        *p++ = ' ';
    *p = '\0';
} /* add_indent() */

/*-------------------------------------------------------------------------*/
static void
svalue_to_string_filter(svalue_t *key, svalue_t *data, void *extra)

/* Filter to add a mapping entry to the given sprintf_buffer.
 * <extra> is a stsf_locals*.
 */

{
    int i;
    stsf_locals_t *locals = (stsf_locals_t *)extra;
    char *delimiter = ":";

    i = locals->num_values;
    locals->spb =
      svalue_to_string(locals->st, key, locals->spb, locals->indent, !i, locals->quote);
    while (--i >= 0)
    {
        stradd(locals->st, &locals->spb, delimiter);
        locals->spb = svalue_to_string(locals->st, data++, locals->spb, 1, !i, locals->quote);
        delimiter = ";";
    }
} /* svalue_to_string_filter() */

/*-------------------------------------------------------------------------*/
static sprintf_buffer_t *
svalue_to_string ( fmt_state_t *st
                 , svalue_t *obj, sprintf_buffer_t *str
                 , int indent, Bool trailing, Bool quoteStrings)

/* Print the value <obj> into the buffer <str> with indentation <indent>.
 * If <trailing> is true, add ",\n" after the printed value.
 *
 * Result is the (updated) string buffer.
 * The function calls itself for recursive values.
 */

{
    mp_int i;

    add_indent(st, &str, indent);
    switch (obj->type)
    {
    case T_INVALID:
        stradd(st, &str, "T_INVALID");
        break;

    case T_LVALUE:
        stradd(st, &str, "lvalue: ");
        str = svalue_to_string(st, obj->u.lvalue, str, indent+2, trailing, quoteStrings);
        break;

    case T_NUMBER:
        numadd(st, &str, obj->u.number);
        break;

    case T_FLOAT:
      {
        char s[200]; /* TODO: Might be too small */

        sprintf(s, "%g", READ_DOUBLE(obj) );
        stradd(st, &str, s);
        break;
      }

    case T_STRING:
        stradd(st, &str, "\"");
        if (!quoteStrings)
        {
            stradd(st, &str, obj->u.string);
        }
        else
        {
            size_t len;
            size_t stringlen = strlen(obj->u.string);

            /* Compute the size of the result string */
            for (len = 0, i = (mp_int)stringlen; i > 0; --i)
            {
                unsigned char c = (unsigned char) obj->u.string[i];

                switch(c)
                {
                case '"':
                case '\n':
                case '\r':
                case '\t':
                case '\a':
                case 0x1b:
                case 0x08:
                case '\\':
                    len += 2; break;
                default:
                    if (c >= 0x20 && c < 0x7F)
                    {
                       len++;
                    }
                    else
                    {
                       len += 4;
                    }
                    break;
                }
            }

            if ( len == stringlen )
            {
                /* No special characters found */
                stradd(st, &str, obj->u.string);
            }
            else
            {
                char * tmpstr, *dest;
                unsigned char *src;

                /* Allocate the temporary string */
                tmpstr = alloca(len+1);

                src = (unsigned char *)obj->u.string;
                dest = tmpstr;
                for (i = (mp_int)stringlen; i > 0; --i)
                {
                    unsigned char c = *src++;

                    switch(c)
                    {
                    case '"': strcpy(dest, "\\\""); dest += 2; break;
                    case '\n': strcpy(dest, "\\n"); dest += 2; break;
                    case '\r': strcpy(dest, "\\r"); dest += 2; break;
                    case '\t': strcpy(dest, "\\t"); dest += 2; break;
                    case '\a': strcpy(dest, "\\a"); dest += 2; break;
                    case 0x1b: strcpy(dest, "\\e"); dest += 2; break;
                    case 0x08: strcpy(dest, "\\b"); dest += 2; break;
                    case '\\': strcpy(dest, "\\\\"); dest += 2; break;
                    default:
                        if (c >= 0x20 && c < 0x7F)
                        {
                           *dest++ = (char)c;
                        }
                        else
                        {
                           static char hex[] = "0123456789abcdef";
                           *dest++ = '\\';
                           *dest++ = 'x';
                           *dest++ = hex[c >> 4];
                           *dest++ = hex[c & 0xf];
                        }
                        break;
                    }
                } /* for() */
                *dest = '\0';

                stradd(st, &str, tmpstr);
            }

        } 

        stradd(st, &str, "\"");
        break;

    case T_QUOTED_ARRAY:
      {
        i = obj->x.quotes;
        do {
            stradd(st, &str, "\'");
        } while (--i);
      }
      /* FALLTHROUGH */

    case T_POINTER:
      {
        size_t size;

        size = VEC_SIZE(obj->u.vec);
        if (!size)
        {
            stradd(st, &str, "({ })");
        }
        else
        {
            struct pointer_record *prec;

            prec = find_add_pointer(st->ptable, obj->u.vec, MY_TRUE);
            if (!prec->id_number)
            {
                /* New array */
                prec->id_number = st->pointer_id++;
                
                stradd(st, &str, "({ /* #");
                numadd(st, &str, prec->id_number);
                stradd(st, &str, ", size: ");
                numadd(st, &str, size);
                stradd(st, &str, " */\n");
                for (i = 0; i < size-1; i++)
                    str = svalue_to_string(st, &(obj->u.vec->item[i]), str, indent+2, MY_TRUE, quoteStrings);
                str = svalue_to_string(st, &(obj->u.vec->item[i]), str, indent+2, MY_FALSE, quoteStrings);
                stradd(st, &str, "\n");
                add_indent(st, &str, indent);
                stradd(st, &str, "})");
            }
            else
            {
                /* Recursion! */
                stradd(st, &str, "({ #");
                numadd(st, &str, prec->id_number);
                stradd(st, &str, " })");
            }
        }
        break;
      }

    case T_MAPPING:
      {
        struct stsf_locals locals;
        struct pointer_record *prec;

        prec = find_add_pointer(st->ptable, obj->u.map, MY_TRUE);
        if (!prec->id_number)
        {
            /* New mapping */
            prec->id_number = st->pointer_id++;

            stradd(st, &str, "([ /* #");
            numadd(st, &str, prec->id_number);
            stradd(st, &str, " */\n");
            locals.spb = str;
            locals.indent = indent + 2;
            locals.num_values = obj->u.map->num_values;
            locals.st = st;
            locals.quote = quoteStrings;
            check_map_for_destr(obj->u.map);
            walk_mapping(obj->u.map, svalue_to_string_filter, &locals);
            str = locals.spb;
            add_indent(st, &str, indent);
            stradd(st, &str, "])");
        }
        else
        {
            /* Recursion! */
            stradd(st, &str, "([ #");
            numadd(st, &str, prec->id_number);
            stradd(st, &str, " ])");
        }
        break;
      }

    case T_OBJECT:
      {
        svalue_t *temp;

        if (obj->u.ob->flags & O_DESTRUCTED)
        {
            /* *obj might be a mapping key, thus we mustn't change it. */
            stradd(st, &str,"0");
            break;
        }
        if (!compat_mode)
            stradd(st, &str, "/");
        stradd(st, &str, obj->u.ob->name);
        push_object(obj->u.ob);
        temp = apply_master_ob(STR_PRINTF_OBJ_NAME, 1);
        if (temp && (temp->type == T_STRING)) {
            stradd(st, &str, " (\"");
            stradd(st, &str, temp->u.string);
            stradd(st, &str, "\")");
        }
        break;
      }

    case T_SYMBOL:
        i = obj->x.quotes;
        do {
            stradd(st, &str, "\'");
        } while (--i);
        stradd(st, &str, obj->u.string);
        break;

    case T_CLOSURE:
      {
        int type;

        switch(type = obj->x.closure_type)
        {
        case CLOSURE_LFUN:
        case CLOSURE_ALIEN_LFUN:
          {
            lambda_t *l;
            program_t *prog;
            int ix;
            funflag_t flags;
            char *function_name;
            object_t *ob;
            Bool is_inherited;

            l = obj->u.lambda;
            if (type == CLOSURE_LFUN)
            {
                ob = l->ob;
                ix = l->function.index;
            }
            else
            {
                ob = l->function.alien.ob;
                ix = l->function.alien.index;
            }

            if (ob->flags & O_DESTRUCTED)
            {
                stradd(st, &str, "<local function in destructed object>");
                break;
            }
            if (O_PROG_SWAPPED(ob))
                load_ob_from_swap(ob);
            stradd(st, &str, "#'");
            stradd(st, &str, ob->name);
            prog = ob->prog;
            flags = prog->functions[ix];
            is_inherited = MY_FALSE;
            while (flags & NAME_INHERITED)
            {
                inherit_t *inheritp;

                is_inherited = MY_TRUE;
                inheritp = &prog->inherit[flags & INHERIT_MASK];
                ix -= inheritp->function_index_offset;
                prog = inheritp->prog;
                flags = prog->functions[ix];
            }

            if (is_inherited)
            {
                stradd(st, &str, "(");
                stradd(st, &str, prog->name);
                str->offset -= 2; /* Remove the '.c' after the program name */
                stradd(st, &str, ")");
            }
            stradd(st, &str, "->");
            memcpy(&function_name
                  , FUNCTION_NAMEP(prog->program + (flags & FUNSTART_MASK))
                  , sizeof function_name
                  );
            stradd(st, &str, function_name);
            break;
          }

        case CLOSURE_IDENTIFIER:
          {
            lambda_t *l;

            l = obj->u.lambda;
            if (l->function.index == VANISHED_VARCLOSURE_INDEX)
            {
                stradd(st, &str, "<local variable from replaced program>");
                break;
            }
            if (l->ob->flags & O_DESTRUCTED)
            {
                stradd(st, &str, "<local variable in destructed object>");
                break;
            }

            if (O_PROG_SWAPPED(l->ob))
                load_ob_from_swap(l->ob);
            stradd(st, &str, "#'");
            stradd(st, &str, l->ob->name);
            stradd(st, &str, "->");
            stradd(st, &str, l->ob->prog->variable_names[l->function.index].name);
            break;
          }

        default:
            if (type < 0)
            {
                switch(type & -0x0800)
                {
                case CLOSURE_OPERATOR:
                  {
                    char *s = NULL;
                    switch(type - CLOSURE_OPERATOR)
                    {
                    case F_POP_VALUE:
                        s = ",";
                        break;

                    case F_BBRANCH_WHEN_NON_ZERO:
                        s = "do";
                        break;

                    case F_BBRANCH_WHEN_ZERO:
                        s = "while";
                        break;

                    case F_BRANCH:
                        s = "continue";
                        break;

                    case F_CSTRING0:
                        s = "default";
                        break;

                    case F_BRANCH_WHEN_ZERO:
                        s = "?";
                        break;

                    case F_BRANCH_WHEN_NON_ZERO:
                        s = "?!";
                        break;

                    case F_RANGE:
                        s = "[..]";
                        break;

                    case F_NR_RANGE:
                        s = "[..<]";
                        break;

                    case F_RR_RANGE:
                        s = "[<..<]";
                        break;

                    case F_RN_RANGE:
                        s = "[<..]";
                        break;

                    case F_MAP_INDEX:
                        s = "[,]";
                        break;

                    case F_NX_RANGE:
                        s = "[..";
                        break;

                    case F_RX_RANGE:
                        s = "[<..";
                        break;

                    }

                    if (s)
                    {
                        stradd(st, &str, "#'");
                        stradd(st, &str, s);
                        break;
                    }
                    type += CLOSURE_EFUN - CLOSURE_OPERATOR;
                  }
                /* default action for operators: FALLTHROUGH */

                case CLOSURE_EFUN:
                    stradd(st, &str, "#'");
                    stradd(st, &str, instrs[type - CLOSURE_EFUN].name);
                    break;

                case CLOSURE_SIMUL_EFUN:
                  {
                    stradd(st, &str, "#'<sefun>");
                    stradd(st, &str, simul_efunp[type - CLOSURE_SIMUL_EFUN].name);
                    break;
                  }
                }
                break;
            } /* if (type) */

        case CLOSURE_UNBOUND_LAMBDA: /* Unbound-Lambda Closure */
        case CLOSURE_PRELIMINARY:    /* Preliminary Lambda Closure */
          {
              char buf[80];

              if (type == CLOSURE_PRELIMINARY)
                  sprintf(buf, "<prelim lambda 0x%p>", obj->u.lambda);
              else
                  sprintf(buf, "<free lambda 0x%p>", obj->u.lambda);
              stradd(st, &str, buf);
              break;
          }

        case CLOSURE_LAMBDA:         /* Lambda Closure */
        case CLOSURE_BOUND_LAMBDA:   /* Bound-Lambda Closure */
          {
              char      buf[80];
              object_t *ob;

              if (type == CLOSURE_BOUND_LAMBDA)
                  sprintf(buf, "<bound lambda 0x%p:", obj->u.lambda);
              else
                  sprintf(buf, "<lambda 0x%p:", obj->u.lambda);

              stradd(st, &str, buf);
              ob = obj->u.lambda->ob;

              if (!ob)
              {
                  stradd(st, &str, "{null}>");
              }
              else
              {
                  if (ob->flags & O_DESTRUCTED)
                      stradd(st, &str, "{dest}");
                  stradd(st, &str, "/");
                  stradd(st, &str, ob->name);
                  stradd(st, &str, ">");
              }
              break;
          }
        } /* switch(type) */

      break;
    } /* case T_CLOSURE */

  case T_CHAR_LVALUE:
    {
        char buf[2];

        buf[0] = *(obj->u.string);
        buf[1] = '\0';
        stradd(st, &str, "'");
        stradd(st, &str, buf);
        stradd(st, &str, "' (");
        numadd(st, &str, buf[0] & 0xff);
        stradd(st, &str, ")");
        break;
    }

  case T_PROTECTED_CHAR_LVALUE:
    {
        stradd(st, &str, "prot char: ");
        str = svalue_to_string(st, obj->u.lvalue, str, indent+2, trailing, quoteStrings);
        break;
    }

  case T_STRING_RANGE_LVALUE:
  case T_PROTECTED_STRING_RANGE_LVALUE:
    {
        if (obj->type == T_PROTECTED_STRING_RANGE_LVALUE)
            stradd(st, &str, "prot: ");
        stradd(st, &str, "\"");
        stradd(st, &str, obj->u.string);
        stradd(st, &str, "\"");
        break;
    }

  case T_POINTER_RANGE_LVALUE:
  case T_PROTECTED_POINTER_RANGE_LVALUE:
    {
        size_t size;

        if (obj->type == T_PROTECTED_POINTER_RANGE_LVALUE)
            stradd(st, &str, "prot: ");

        size = VEC_SIZE(obj->u.vec);
        if (!size)
        {
            stradd(st, &str, "({ })");
        }
        else
        {
            struct pointer_record *prec;

            prec = find_add_pointer(st->ptable, obj->u.vec, MY_TRUE);
            if (!prec->id_number)
            {
                /* New array */
                prec->id_number = st->pointer_id++;
                
                stradd(st, &str, "({ /* #");
                numadd(st, &str, prec->id_number);
                stradd(st, &str, ", size: ");
                numadd(st, &str, size);
                stradd(st, &str, " */\n");
                for (i = 0; i < size-1; i++)
                    str = svalue_to_string(st, &(obj->u.vec->item[i]), str, indent+2, MY_TRUE, quoteStrings);
                str = svalue_to_string(st, &(obj->u.vec->item[i]), str, indent+2, MY_FALSE, quoteStrings);
                stradd(st, &str, "\n");
                add_indent(st, &str, indent);
                stradd(st, &str, "})");
            }
            else
            {
                /* Recursion! */
                stradd(st, &str, "({ #");
                numadd(st, &str, prec->id_number);
                stradd(st, &str, " })");
            }
        }
        break;
    }

  case T_PROTECTED_LVALUE:              
      stradd(st, &str, "prot lvalue: ");
      str = svalue_to_string(st, obj->u.lvalue, str, indent+2, trailing, quoteStrings);
      break;

  default:
      stradd(st, &str, "!ERROR: GARBAGE SVALUE (");
      numadd(st, &str, obj->type);
      stradd(st, &str, ")!");
  } /* end of switch (obj->type) */

  if (trailing)
      stradd(st, &str, ",\n");

  return str;
} /* end of svalue_to_string() */

/*-------------------------------------------------------------------------*/
static void
add_justified ( fmt_state_t *st
              , char *str, size_t len, int fs
              )

/* Justify string <str> (length <len>) within the fieldsize <fs>.
 * After that, add it to the global buff[].
 */

{
    int i;
    size_t sppos;
    int num_words;    /* Number of words in the input */
    int num_chars;    /* Number of non-space characters in the input */
    int num_spaces;   /* Number of spaces required */
    int min_spaces;   /* Min number of pad spaces */
    size_t pos;

    /* Check how much data we have */

    num_words = 0;
    num_chars = 0;

    /* Find the first non-space character.
     * If it's all spaces, return.
     */
    for (pos = 0; pos < len && *str == ' '; pos++, str++) NOOP;
    if (pos >= len)
        return;

    len -= (size_t)pos;
    pos = 0;
    num_words = 1;

    while (pos < len)
    {
        /* Find the end of the word */
        for ( ; pos < len && str[pos] != ' '; pos++, num_chars++) NOOP;
        if (pos >= len)
            break;

        /* Find the start of the next word */
        for ( ; pos < len && str[pos] == ' '; pos++) NOOP;
        if (pos >= len)
            break;

        /* We got a new word - count it */
        num_words++;
    }

#ifdef DEBUG
    if (fs < num_words - 1 + num_chars)
        fatal("add_justified(): fieldsize %d < data length %d\n", fs, num_words - 1 + num_chars);
#endif

    /* Compute the number of spaces we need to insert.
     * It is guaranteed here that we have enough space to insert
     * at least one space between each word.
     */
    num_spaces = fs - num_chars;
    if (num_words == 1)
        min_spaces = num_spaces;
    else
        min_spaces = num_spaces / (num_words-1);
        /* min_spaces * (num_words-1) <= num_spaces < min_spaces * num_words */

    /* Loop again over the data, now adding spaces as we go. */

    for (pos = 0; pos < len; )
    {
        int mark;
        int padlength;

        /* Find the end of the current word */
        for (mark = pos ; pos < len && str[pos] != ' '; pos++) NOOP;

        /* Add the word */
        ADD_STRN(str+mark, pos - mark);
        num_words--;

        if (pos >= len || num_words < 1)
            break;

        /* There is a word following - add spaces */
        if (num_words == 1) /* Last word: add all remaining padding */
            padlength = (int)num_spaces;
        else if (num_spaces < min_spaces * num_words) /* Space underrun */
            padlength = 1;
        else if (num_spaces == min_spaces * num_words)
            /* Exactly the min. padlength per word avail. */
            padlength = min_spaces;
        else if (num_spaces >= min_spaces * num_words + 2)
            /* Force an extra space */
            padlength = min_spaces+1;
        else /* Randomly add one space */
            padlength = min_spaces + (int)random_number(2);
        sppos = st->bpos;
        ADD_PADDING(" ", padlength);
        st->sppos = sppos;
        num_spaces -= padlength;

        /* Find the start of the next word */
        for ( ; pos < len && str[pos] == ' '; pos++) NOOP;
    }
} /* add_justified() */

/*-------------------------------------------------------------------------*/
static void
add_aligned ( fmt_state_t *st
            , char *str, size_t len, char *pad, int fs
            , format_info finfo)

/* Align string <str> (length <len>) within the fieldsize <fs> according
 * to the <finfo>. After that, add it to the global buff[].
 */

{
    int i;
    size_t sppos;
    Bool is_space_pad;

    if (fs < len)
        fs = len;

    sppos = 0;
    is_space_pad = MY_FALSE;
    if (pad[0] == ' ' && pad[1] == '\0' && !(finfo & INFO_PS_KEEP))
        is_space_pad = MY_TRUE;

    switch(finfo & INFO_A)
    {
    case INFO_A_JUSTIFY:
    case INFO_A_LEFT:
        /* Also called for the last line of a justified block */
        ADD_STRN(str, len)
        if (is_space_pad)
            sppos = st->bpos;
        ADD_PADDING(pad, fs - len)
        if (is_space_pad)
            st->sppos = sppos;
        break;

    case INFO_A_CENTRE:
        if (finfo & INFO_PS_ZERO)
        {
            ADD_PADDING("0", (fs - len + 1) >> 1)
        }
        else
        {
            ADD_PADDING(pad, (fs - len + 1) >> 1)
        }
        ADD_STRN(str, len)
        if (is_space_pad)
            sppos = st->bpos;
        ADD_PADDING(pad, (fs - len) >> 1)
        if (is_space_pad)
            st->sppos = sppos;
        break;

    default:
      { /* std (s)printf defaults to right alignment.
         */
        if (finfo & INFO_PS_ZERO)
        {
            ADD_PADDING("0", fs - len)
        }
        else
        {
            ADD_PADDING(pad, fs - len)
        }
        ADD_STRN(str, len)
      }
    }
} /* add_aligned() */

/*-------------------------------------------------------------------------*/
static int
add_column (fmt_state_t *st, cst **column)

/* Add the the next line from <column> to the buffer buff[].
 * Result 0: column not finished (more lines/data pending)
 *        1: column completed and removed from the list
 *        2: column completed with terminating \n, column removed from
 *           the list.
 */

{
#define COL (*column)

    unsigned int done;
    mp_int length;
    unsigned int save;
    char *COL_D = COL->d.col;
    char *p;

    /* Set done to the actual number of characters to copy.
     */
    length = COL->pres;
    if ((COL->info & INFO_A) == INFO_A_JUSTIFY && length > COL->size)
        length = COL->size;
    for (p = COL_D; length && *p && *p !='\n'; p++, length--) NOOP;
    done = p - COL_D;
    if (*p && *p !='\n')
    {
        /* Column data longer than the permitted size: find a
         * a space to do wordwrapping.
         */
        save = done;
        for (; ; done--,p--)
        {
            /* handle larger than column size words... */
            if (!done)
            {
                /* Sorry, it's one big word.
                 * Print the word over the fieldsize.
                 */
                done = save - 1;
                p += save;
                break;
            }
            if (*p == ' ')
            {
                /* If went more than one character back, check if
                 * the next word is longer than permitted. If that is
                 * the case we might as well start breaking it up right
                 * here.
                 */
                if (save-2 > done)
                {
                    char *p2;

                    length = COL->pres;
                    if ((COL->info & INFO_A) == INFO_A_JUSTIFY
                     && length > COL->size)
                        length = COL->size;
                    for ( p2 = p+1, length--
                        ; length && *p2 && *p2 !='\n' && *p2 != ' '
                        ; p2++, length--) NOOP;
                    if (*p2 && *p2 != '\n' && *p2 != ' ')
                    {
                        /* Yup, the next word is far too long. */
                        p += save - done;
                        done = save - 1;
                    }
                    /* else: the next word is not too long */
                }
                /* else: breaking too long word here would look silly anyway
                 */
                break;
            } /* if (p == ' ') */
        } /* for (done) */
    } /* if (breaking needed) */

    /* On justified formatting, don't format the last line that way, nor
     * justified lines ending in NL.
     */
    if ((COL->info & INFO_A) == INFO_A_JUSTIFY
     && *COL_D && *(COL_D+1)
     && *p != '\n' && *p != '\0'
       )
    {
        add_justified(st, COL_D, p - COL_D, COL->size);
    }
    else
    {
        add_aligned(st, COL_D, p - COL_D, COL->pad, COL->size, COL->info);
    }

    COL_D += done; /* inc'ed below ... */

    /* if this or the next character is a '\0' then take this column out
     * of the list.
     */
    if (!(*COL_D) || !(*(++COL_D)))
    {
        cst *temp;
        int ret;

        if (*(COL_D-1) == '\n')
            ret = 2;
        else
            ret = 1;

        temp = COL->next;
        xfree(COL);
        COL = temp;
        return ret;
    }

    /* Column not finished */
    COL->d.col = COL_D;
    return 0;

#undef COL

} /* add_column() */

/*-------------------------------------------------------------------------*/
static Bool
add_table (fmt_state_t *st, cst **table)

/* Add the next line of <table> to the buffer.
 * Return TRUE if the table was completed and removed from the list.
 */

{
    unsigned int done, i;

#define TAB (*table)
#define TAB_D (TAB->d.tab[i])

    /* Loop over all columns of the table */
    for (i = 0; i < TAB->nocols && TAB_D; i++)
    {
        /* Get the length to add */
        for (done = 0; (TAB_D[done]) && (TAB_D[done] != '\n'); done++) NOOP;

        add_aligned(st, TAB_D, done, TAB->pad, TAB->size, TAB->info);

        TAB_D += done; /* inc'ed next line ... */
        if (!(*TAB_D) || !(*(++TAB_D)))
            TAB_D = NULL;
    }

    /* Fill up the end of the table if required */
    if (i < TAB->nocols)
    {
        done = TAB->size;
        for (; i < TAB->nocols; i++)
        {
            /* TAB->size is not negative. */
            ADD_CHARN(' ', done)
        }
    }

    if (!TAB->d.tab[0])
    {
        /* Table finished */

        cst *temp;

        temp = TAB->next;
        xfree(TAB->d.tab);
        xfree(TAB);
        TAB = temp;
        return MY_TRUE;
    }

    return MY_FALSE;

#undef TAB
#undef TAB_D

} /* add_table() */

/*-------------------------------------------------------------------------*/
char *
string_print_formatted (char *format_str, int argc, svalue_t *argv)

/* The (s)printf() function: format <format_str> with the given arguments
 * and return a pointer to the result (stored in a static buffer).
 * If an error occurs and RETURN_ERROR_MESSAGES is defined, an error
 * will return the error string as result; if R_E_M is undefined, an
 * true error() is raised.
 */

{
    fmt_state_t  *st;     /* The formatting state */
static char buff[BUFF_SIZE]; /* The buffer to return the result */

    svalue_t     *carg;            /* current arg */
    int           arg;             /* current arg number */
    format_info   finfo;           /* parse formatting info */
    char          format_char;     /* format type */
    unsigned int  nelemno;         /* next offset into array */
    unsigned int  fpos;            /* position in format_str */
    unsigned int  fs;              /* field size */
    int           pres;            /* precision */
    unsigned int  err_num;         /* error code */
    char         *pad;             /* fs pad string */
    int           column_stat;     /* Most recent column add status */

#   define GET_NEXT_ARG {\
        if (++arg >= argc) ERROR(ERR_TO_FEW_ARGS); \
        carg = (argv+arg);\
    }

#   define SAVE_CHAR(pointer) {\
        savechars *new;\
        new = xalloc(sizeof(savechars));\
        if (!new) \
            ERROR(ERR_NOMEM); \
        new->what = *(pointer);\
        new->where = pointer;\
        new->next = st->saves;\
        st->saves = new;\
    }

    xallocate(st, sizeof *st, "sprintf() context");

    st->saves = NULL;
    st->clean.u.string = NULL;
    st->csts = NULL;
    st->ptable = NULL;

    if (0 != (err_num = setjmp(st->error_jmp)))
    {
        /* error handling */
        char *err;
        cst  *tcst;

        if (st->ptable)
            free_pointer_table(st->ptable);

        /* Restore the saved characters */
        while (st->saves)
        {
            savechars *tmp;
            *(st->saves->where) = st->saves->what;
            tmp = st->saves;
            st->saves = st->saves->next;
            xfree(tmp);
        }

        /* Get rid of a temp string */
        if (st->clean.u.string)
            xfree(st->clean.u.string);

        /* Free column and table data */
        while ( NULL != (tcst = st->csts) )
        {
            st->csts = tcst->next;
            if ((tcst->info & (INFO_COLS|INFO_TABLE)) == INFO_TABLE
             && tcst->d.tab)
                xfree(tcst->d.tab);
            xfree(tcst);
        }

        /* Select the error string */
        switch(err_num & ERR_ID_NUMBER)
        {
        default:
#ifdef DEBUG
            fatal("undefined (s)printf() error 0x%X\n", err_num);
#endif
        case ERR_BUFF_OVERFLOW:
            err = "BUFF_SIZE overflowed...";
            break;

        case ERR_TO_FEW_ARGS:
            err = "More arguments specified than passed.";
            break;

        case ERR_INVALID_STAR:
            err = "Incorrect argument type to *.";
            break;

        case ERR_PREC_EXPECTED:
            err = "Expected precision not found.";
            break;

        case ERR_INVALID_FORMAT_STR:
            err = "Error in format string.";
            break;

        case ERR_INCORRECT_ARG:
            err = "incorrect argument type to %%%c.";
            break;

        case ERR_CST_REQUIRES_FS:
            err = "Column/table mode requires a field size.";
            break;

        case ERR_UNDEFINED_TYPE:
            err = "!feature - undefined type!";
            break;

        case ERR_QUOTE_EXPECTED:
            err = "Quote expected in format string.";
            break;

        case ERR_UNEXPECTED_EOS:
            err = "Unexpected end of format string.";
            break;

        case ERR_NULL_PS:
            err = "Null pad string specified.";
            break;

        case ERR_ARRAY_EXPECTED:
            err = "Array expected.";
            break;

        case ERR_NOMEM:
            err = "Out of memory.";
            break;
        }

        /* Create the error message in buff[] */
        st->buff[0]='\0';
        if ((err_num & ERR_ID_NUMBER) != ERR_NOMEM)
        {
            int line;
            char *file;

            line = get_line_number_if_any(&file);
            sprintf(st->buff, "%s:%d: ", file, line);
        }

#ifdef RETURN_ERROR_MESSAGES
        strcat(st->buff, "(s)printf() error: ");
#else
        strcat(st->buff, "(s)printf(): ");
#endif
        sprintf(st->buff + strlen(st->buff)
               , err, EXTRACT_ERR_ARGUMENT(err_num));
        strcat(st->buff, "\n");
        strcpy(buff, st->buff);
        xfree(st);
#ifndef RETURN_ERROR_MESSAGES
        error("%s", buff); /* buff may contain a '%' */
        /* NOTREACHED */
#endif /* RETURN_ERROR_MESSAGES */
        return buff;
    }

    format_char = 0;
    nelemno = 0;
    column_stat = 0;

    arg = -1;
    st->bpos = 0;
    st->sppos = -1;
    st->line_start = 0;

    /* Walk through the format string */
    for (fpos = 0; MY_TRUE; fpos++)
    {
        if ((format_str[fpos] == '\n') || (!format_str[fpos]))
        {
            /* Line- or Format end */

            if (!st->csts)
            {
                /* No columns/tables to resolve, but add a second
                 * newline if there is one pending from an added
                 * column
                 */

                if (column_stat == 2)
                    ADD_CHAR('\n');
                column_stat = 0;
                if (!format_str[fpos])
                    break;
                ADD_CHAR('\n');
                st->line_start = st->bpos;
                continue;
            }

            column_stat = 0; /* If there was a newline pending, it
                              * will be implicitely added now.
                              */
            ADD_CHAR('\n');
            st->line_start = st->bpos;

            /* Handle pending columns and tables */
            while (st->csts)
            {
                cst **temp;

                /* Add one line from each column/table */
                temp = &st->csts;
                while (*temp)
                {
                    p_int i;
                    if ((*temp)->info & INFO_COLS)
                    {
                        if (*((*temp)->d.col-1) != '\n')
                            while (*((*temp)->d.col) == ' ')
                                (*temp)->d.col++;
                        i = (*temp)->start - (st->bpos - st->line_start);
                        ADD_CHARN(' ', i);
                        column_stat = add_column(st, temp);
                        if (!column_stat)
                            temp = &((*temp)->next);
                    }
                    else
                    {
                        i = (*temp)->start - (st->bpos - st->line_start);
                        if (i > 0)
                            ADD_CHARN(' ', i);
                        if (!add_table(st, temp))
                            temp = &((*temp)->next);
                    }
                } /* while (*temp) */

                if (st->csts || format_str[fpos] == '\n')
                    ADD_CHAR('\n');
                st->line_start = st->bpos;
            } /* while (csts) */

            if (column_stat == 2 && format_str[fpos] != '\n')
                ADD_CHAR('\n');

            if (!format_str[fpos])
                break;
            continue;
        } /* if newline or formatend */

        if (format_str[fpos] == '%')
        {
            /* Another format entry */

            if (format_str[fpos+1] == '%')
            {
                ADD_CHAR('%');
                fpos++;
                continue;
            }

            if (format_str[fpos+1] == '^')
            {
                ADD_CHAR('%');
                fpos++;
                ADD_CHAR('^');
                continue;
            }

            GET_NEXT_ARG;
            fs = 0;
            pres = 0;
            pad = " ";
            finfo = 0;

            /* Parse the formatting entry */
            for (fpos++; !(finfo & INFO_T); fpos++)
            {
                if (!format_str[fpos])
                {
                    finfo |= INFO_T_ERROR;
                    break;
                }
                if ((format_str[fpos] >= '0' && format_str[fpos] <= '9')
                 || (format_str[fpos] == '*'))
                {
                    /* Precision resp. fieldwidth */
                    if (pres == -1) /* then looking for pres */
                    {
                        if (format_str[fpos] == '*')
                        {
                            /* Get the value from the args */
                            if (carg->type != T_NUMBER)
                                ERROR(ERR_INVALID_STAR);
                            pres = carg->u.number;
                            GET_NEXT_ARG;
                            continue;
                        }

                        /* Parse the number */
                        pres = format_str[fpos] - '0';
                        for ( fpos++
                            ; format_str[fpos]>='0' && format_str[fpos]<='9'
                            ; fpos++)
                        {
                            pres = pres*10 + format_str[fpos] - '0';
                        }
                    }
                    else /* then is fs (and maybe pres) */
                    {
                        if (format_str[fpos] == '0'
                         && (   (   format_str[fpos+1] >= '1'
                                 && format_str[fpos+1] <= '9')
                             || format_str[fpos+1] == '*')
                           )
                            finfo |= INFO_PS_ZERO;
                        else
                        {
                            if (format_str[fpos] == '*')
                            {
                                if (carg->type != T_NUMBER)
                                    ERROR(ERR_INVALID_STAR);
                                if ((int)(fs = carg->u.number) < 0)
                                {
                                    if (fs == PINT_MIN)
                                        fs = PINT_MAX;
                                    else
                                        fs = -fs;
                                    finfo |= INFO_A_LEFT;
                                }

                                if (pres == -2)
                                    pres = fs; /* colon */
                                GET_NEXT_ARG;
                                continue;
                            }
                            fs = format_str[fpos] - '0';
                        }

                        for ( fpos++
                            ; format_str[fpos] >= '0' && format_str[fpos] <= '9'
                            ; fpos++)
                        {
                            fs = fs*10 + format_str[fpos] - '0';
                        }

                        if (pres == -2) /* colon */
                            pres = fs;
                    }

                    fpos--; /* bout to get incremented */
                    continue;
                } /* if (precision/alignment field) */

                /* fpos now points to format type */

                switch (format_str[fpos])
                {
                case ' ': finfo |= INFO_PP_SPACE; break;
                case '+': finfo |= INFO_PP_PLUS; break;
                case '-': finfo |= INFO_A_LEFT; break;
                case '|': finfo |= INFO_A_CENTRE; break;
                case '$': finfo |= INFO_A_JUSTIFY; break;
                case '@': finfo |= INFO_ARRAY; break;
                case '=': finfo |= INFO_COLS; break;
                case '#': finfo |= INFO_TABLE; break;
                case '.': pres = -1; break;
                case ':': pres = -2; break;
                case '%': finfo |= INFO_T_NULL; break; /* never reached */
                case 'O': finfo |= INFO_T_LPC; break;
                case 'Q': finfo |= INFO_T_QLPC; break;
                case 's': finfo |= INFO_T_STRING; break;
                case 'i': finfo |= INFO_T_INT; format_char = 'd'; break;
                case 'd':
                case 'c':
                case 'o':
                case 'x':
                case 'X':
                    format_char = format_str[fpos];
                    finfo |= INFO_T_INT;
                    break;
                case 'f':
                case 'F':
                case 'g':
                case 'G':
                case 'e':
                case 'E':
                    format_char = format_str[fpos];
                    finfo |= INFO_T_FLOAT;
                    break;
                case '\'':
                    pad = &(format_str[++fpos]);
                    finfo |= INFO_PS_KEEP;
                    while (1)
                    {
                        if (!format_str[fpos])
                            ERROR(ERR_UNEXPECTED_EOS);

                        if (format_str[fpos] == '\\')
                        {
                            if (!format_str[fpos+1])
                                ERROR(ERR_UNEXPECTED_EOS);
                            fpos += 2;
                            continue;
                        }

                        if (format_str[fpos] == '\'')
                        {
                            if (format_str+fpos == pad)
                                ERROR(ERR_NULL_PS);
                            SAVE_CHAR(format_str+fpos);
                            format_str[fpos] = '\0';
                            break;
                        }
                        fpos++;
                    }
                    break;
                default:
                    finfo |= INFO_T_ERROR;
                    break;
                }
            } /* for(format parsing) */

            if (pres < 0)
                ERROR(ERR_PREC_EXPECTED);

            /* Now handle the different arg types...
             */
            if (finfo & INFO_ARRAY)
            {
                if (carg->type != T_POINTER)
                    ERROR(ERR_ARRAY_EXPECTED);
                if (carg->u.vec == &null_vector)
                {
                    fpos--; /* 'bout to get incremented */
                    continue;
                }
                carg = (argv+arg)->u.vec->item;
                nelemno = 1; /* next element number */
            }

            while(1)
            {
                switch(finfo & INFO_T)
                {
                case INFO_T_ERROR:
                    ERROR(ERR_INVALID_FORMAT_STR);

                case INFO_T_NULL:
                  {
                    /* never reached... */
                    fprintf(stderr, "%s: (s)printf: INFO_T_NULL.... found.\n"
                                  , current_object->name);
                    ADD_CHAR('%');
                    break;
                  }

                case INFO_T_LPC:
                case INFO_T_QLPC:
                  {
                    sprintf_buffer_t *b;
#                   define CLEANSIZ 0x200

                    if (st->clean.u.string)
                        xfree(st->clean.u.string);

                    put_malloced_string(&st->clean, xalloc(CLEANSIZ));
                    if (!st->clean.u.string)
                        ERROR(ERR_NOMEM);
                    st->clean.u.string[0] = '\0';

                    st->ptable = new_pointer_table();
                    if (!st->ptable)
                        ERROR(ERR_NOMEM);
                    st->pointer_id = 1;

                    b = (sprintf_buffer_t *)
                        ( st->clean.u.string+CLEANSIZ-sizeof(sprintf_buffer_t) );
                    b->offset = -CLEANSIZ+(p_int)sizeof(sprintf_buffer_t);
                    b->size = CLEANSIZ;
                    b->start = &st->clean.u.string;
                    svalue_to_string(st, carg, b, 0, MY_FALSE
                                    , (finfo & INFO_T) == INFO_T_QLPC);
                    carg = &st->clean; /* passed to INFO_T_STRING */
                    free_pointer_table(st->ptable);
                    st->ptable = NULL;
                    /* FALLTHROUGH */
                  }

                case INFO_T_STRING:
                  {
                    int slen;

                    if (carg->type != T_STRING)
                        ERROR1(ERR_INCORRECT_ARG, 's');
                    slen = strlen(carg->u.string);


                    if (finfo & (INFO_COLS | INFO_TABLE) )
                    {
                        /* Add a new column/table info
                         * ... this is complicated ...
                         */
                        cst **temp;

                        if (!fs)
                            ERROR(ERR_CST_REQUIRES_FS);

                        if ((finfo & (INFO_COLS | INFO_TABLE))
                          == (INFO_COLS | INFO_TABLE)
                           )
                            ERROR(ERR_INVALID_FORMAT_STR);

                        /* Find the end of the list of columns/tables */
                        temp = &st->csts;
                        while (*temp)
                            temp = &((*temp)->next);

                        if (finfo & INFO_COLS)
                        {
                            /* Create a new columns structure */
                            *temp = xalloc(sizeof(cst));
                            if (!*temp)
                                ERROR(ERR_NOMEM);
                            (*temp)->next = NULL;
                            (*temp)->d.col = carg->u.string;
                            (*temp)->pad = pad;
                            (*temp)->size = fs;
                            (*temp)->pres = (pres) ? pres : fs;
                            (*temp)->info = finfo;
                            (*temp)->start = st->bpos - st->line_start;

                            /* Format the first line from the column */
                            column_stat = add_column(st, temp);
                        }
                        else
                        {
                            /* (finfo & INFO_TABLE) */
                            unsigned int n, len, max;
                            int tpres;
                            char c, *s, *start;
                            p_uint i;

#                    define TABLE carg->u.string

                            /* Create the new table structure */
                            (*temp) = (cst *)xalloc(sizeof(cst));
                            if (!*temp)
                                ERROR(ERR_NOMEM);
                            (*temp)->pad = pad;
                            (*temp)->info = finfo;
                            (*temp)->start = st->bpos - st->line_start;
                            (*temp)->next = NULL;

                            /* Determine the size of the table */
                            max = len = 0;
                            n = 1;
                            s = TABLE;
                            if ( '\0' != (c = *(start = s)) ) for (;;)
                            {
                                if (c != '\n')
                                {
                                    if ( '\0' != (c = *++s) )
                                        continue;
                                    else
                                        break;
                                }
                                len = s - start;
                                if (len > max)
                                    max = len;
                                n++;
                                if ( '\0' != (c = *(start = ++s)) )
                                {
                                    continue;
                                }
                                n--;
                                break;
                            } /* if() for() */

                            /* Now: n = number of lines
                             *      max = max length of the lines
                             */

                            tpres = pres;
                            if (tpres)
                            {
                                (*temp)->size = fs/tpres;
                            }
                            else
                            {
                                len = s - start;
                                if (len > max)
                                    max = len; /* the null terminated word */
                                tpres = fs/(max+2);
                                  /* at least two separating spaces */
                                if (!tpres)
                                    tpres = 1;
                                (*temp)->size = fs/tpres;
                            }

                            len = n/tpres; /* length of average column */

                            if (n < (unsigned int)tpres)
                                tpres = n;
                            if (len*tpres < n)
                                len++;
                            /* Since the table will be filled by column,
                             * the result will be a rectangle with as little
                             * as possible empty space. This means we have
                             * to adjust the no of columns (tpres) to
                             * what the fill algorithm will actually
                             * produce.
                             */
                            if (len > 1 && n%tpres)
                                tpres -= (tpres - n%tpres)/len;

                            (*temp)->d.tab = xalloc(tpres*sizeof(char *));
                            if (!(*temp)->d.tab)
                                ERROR(ERR_NOMEM);
                            (*temp)->nocols = tpres; /* heavy sigh */
                            (*temp)->d.tab[0] = TABLE;

                            if (tpres == 1)
                                goto add_table_now;

                            /* Subformat the table, replacing some characters
                             * in the given string.
                             * The original chars are saved for later restoring.
                             */
                            i = 1; /* the next column number */
                            n = 0; /* the current "word" number in this column */
                            for (fs = 0; TABLE[fs]; fs++)
                            {
                                /* throwing away fs... */
                                if (TABLE[fs] == '\n')
                                {
                                    if (++n >= len)
                                    {
                                        SAVE_CHAR(((TABLE)+fs));
                                        TABLE[fs] = '\0';
                                        (*temp)->d.tab[i++] = TABLE+fs+1;
                                        if (i >= (unsigned int)tpres)
                                            goto add_table_now;
                                        n = 0;
                                    }
                                }
                            } /* for (fs) */

add_table_now:
                            /* Now add the table (at least the first line) */
                            add_table(st, temp);
                        }
                    }
                    else
                    {
                        Bool justifyString;

                        /* not column or table */
                        if (pres && pres < slen)
                        {
                            slen = pres;
                        }

                        /* Determine whether to print this string
                         * justified, or not.
                         */
                        justifyString = MY_FALSE;
                        if (fs && (finfo & INFO_A) == INFO_A_JUSTIFY)
                        {
                            /* Flush the string only if it doesn't
                             * end in a NL.
                             * Also, strip off trailing spaces.
                             */
                            for ( ;    slen > 0
                                    && carg->u.string[slen-1] == ' '
                                  ; slen--
                               ) NOOP;

                            if ( slen != 0
                              && (unsigned int)slen <= fs
                              && carg->u.string[slen-1] != '\n'
                               )
                                justifyString = MY_TRUE;
                        }

                        /* not column or table */

                        if (justifyString)
                        {
                            add_justified(st, carg->u.string, slen, fs);
                        }
                        else if (fs && fs > (unsigned int)slen)
                        {
                            add_aligned(st, carg->u.string, slen, pad, fs
                                         , finfo);
                        }
                        else
                        {
                            ADD_STRN(carg->u.string, slen)
                        }
                    }
                    break;
                  }

                case INFO_T_INT:
                case INFO_T_FLOAT:
                  {
                    /* We 'cheat' by using the systems sprintf() to format
                     * the number.
                     */
                    char cheat[6];  /* Synthesized format for sprintf() */
                    char temp[1024];
                      /* The buffer must be big enough to hold the biggest float
                       * in non-exponential representation. 1 KByte is hopefully
                       * far on the safe side.
                       * TODO: Allocate it dynamically?
                       */
                    double value;   /* The value to print */
                    int    numdig;  /* (Estimated) number of digits before the '.' */
                    int tmpl;
                    p_uint i = 1;

                    *cheat = '%';
                    switch (finfo & INFO_PP)
                    {
                        case INFO_PP_SPACE: cheat[i++] = ' '; break;
                        case INFO_PP_PLUS:  cheat[i++] = '+'; break;
                    }
                    if ((finfo & INFO_T) == INFO_T_FLOAT)
                    {
                        if (carg->type != T_FLOAT) /* sigh... */
                        {
                            ERROR1(ERR_INCORRECT_ARG, format_char);
                        }
                        cheat[i++] = '.';
                        cheat[i++] = '*';
                        cheat[i++] = format_char;
                        cheat[i] = '\0';

                        value = READ_DOUBLE(carg);

                        if ('e' == format_char
                         || 'E' == format_char
                         || fabs(value) < 1.0)
                            numdig = 1;
                        else
                            numdig = (int) ceil(log10(fabs(value)));
                        if (value < 0.0)
                            numdig++;

                        if ((size_t)pres > (sizeof(temp) - 12 - numdig))
                        {
                            pres = sizeof(temp) - 12 - numdig;
                        }
                        sprintf(temp, cheat, pres, READ_DOUBLE(carg));
                        tmpl = strlen(temp);
                        if ((size_t)tmpl >= sizeof(temp))
                            fatal("Local buffer overflow in sprintf() for float.\n");
                    }
                    else
                    {
                        if (carg->type != T_NUMBER) /* sigh... */
                        {
                            ERROR1(ERR_INCORRECT_ARG, format_char);
                        }
                        cheat[i++] = format_char;
                        cheat[i] = '\0';
                        sprintf(temp, cheat, carg->u.number);
                        tmpl = strlen(temp);
                        if ((size_t)tmpl >= sizeof(temp))
                            fatal("Local buffer overflow in sprintf() for int.\n");
                        if (pres && tmpl > pres)
                            tmpl = pres; /* well.... */
                    }
                    if ((unsigned int)tmpl < fs)
                    {
                        if ((finfo & INFO_PS_ZERO) != 0
                         && (   temp[0] == ' '
                             || temp[0] == '+'
                             || temp[0] == '-'
                            )
                         && (finfo & INFO_A) != INFO_A_LEFT
                           )
                        {
                            /* Non-left alignment and we're printing
                             * with leading zeroes: preserve the sign
                             * character in the right place.
                             */
                            ADD_STRN(temp, 1);
                            add_aligned(st, temp+1, tmpl-1, pad, fs-1, finfo);
                        }
                        else
                            add_aligned(st, temp, tmpl, pad, fs, finfo);
                    }
                    else
                        ADD_STRN(temp, tmpl)
                    break;
                  }
                default:        /* type not found */
                    ERROR(ERR_UNDEFINED_TYPE);
                }

                if (!(finfo & INFO_ARRAY))
                    break;

                if (nelemno >= VEC_SIZE((argv+arg)->u.vec))
                    break;

                carg = (argv+arg)->u.vec->item+nelemno++;
            } /* end of while (1) */
            fpos--; /* bout to get incremented */
            continue;
        } /* if format entry */

        /* Nothing to format: just copy the character */
        ADD_CHAR(format_str[fpos]);
    } /* for (fpos=0; 1; fpos++) */

    ADD_CHAR('\0'); /* Terminate the formatted string */

    /* Restore characters */
    while (st->saves)
    {
        savechars *tmp;
        *(st->saves->where) = st->saves->what;
        tmp = st->saves;
        st->saves = st->saves->next;
        xfree(tmp);
    }

    /* Free the temp string */
    if (st->clean.u.string)
        xfree(st->clean.u.string);

    /* Copy over the result */
    strcpy(buff, st->buff);
    xfree(st);

    /* Done */
    return buff;

#undef GET_NEXT_ARG
#undef SAVE_CHAR

} /* string_print_formatted() */

/***************************************************************************/
