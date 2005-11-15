#ifndef SVALUE_H__
#define SVALUE_H__ 1

/*---------------------------------------------------------------------------
 * Stack-value datatypes used by the virtual machine.
 *
 *---------------------------------------------------------------------------
 * This file refers to, but does not define 'basic' types like mappings,
 * arrays or lambdas.
 */

#include "driver.h"
#include "typedefs.h"

/* --- Types --- */

/* --- union u: the hold-all type ---
 *
 * This union is used to hold the data referenced by a svalue.
 * It contains no type information, which must be provided by the
 * containing svalue.
 *
 * Lots of code assumes that '.number' is big enough to contain the whole
 * union regardless of its actual content, therefore it is of type 'p_int'.
 */
union u {
    char *string;
      /* T_STRING: pointer to the first character of the string.
       * T_SYMBOL: pointer to the shared symbol string.
       * T_CHAR_LVALUE: pointer to the referenced character (referenced
       *           from a T_LVALUE).
       * T_(PROTECTED_)STRING_RANGE_LVALUE: the target string holding
       *   the range.
       */
    p_int number;
      /* T_NUMBER: the number.
       */
    object_t *ob;
      /* T_OBJECT: pointer to the object structure.
       * T_CLOSURE: efun-, simul_efun-, operator closures: the object
       *            the closure is bound to.
       */
    vector_t *vec;
      /* T_POINTER, T_QUOTED_ARRAY: pointer to the vector structure.
       * T_(PROTECTED_)POINTER_RANGE_LVALUE: the target vector holding
       *   the range.
       */
    mapping_t *map;
      /* T_MAPPING: pointer to the mapping structure.
       * T_PROTECTOR_MAPPING: TODO: ???
       */
    lambda_t *lambda;
      /* T_CLOSURE: allocated closures: the closure structure.
       */
    p_int mantissa;
      /* T_FLOAT: The mantissa (or at least one half of the float bitpattern).
       */
    callback_t *cb;
      /* T_CALLBACK: A callback structure referenced from the stack
       *   to allow proper cleanup during error recoveries. The interpreter
       * knows how to free it, but that's all.
       */
      
    svalue_t *lvalue;
      /* T_LVALUE: pointer to a (usually 'normal') svalue which
       *   this lvalue references. Also, lvalues may be chained through
       *   this pointer so that only the last lvalue points to the real
       *   svalue, and the others point to the next lvalue in the chain.
       *   This is necessary when lvalues are passed around as lfun args.
       *
       * For T_LVALUE this may point to one of the protector
       * structures (see interpret.c) which just 'happen' to have a svalue
       * as first element. The actual type of the protected svalue is given
       * by the .type of the referenced protector structure.
       *
       * When creating such a protected T_LVALUE, this .lvalue field is set
       * by assigning one of the following aliases:
       */
    struct protected_lvalue *protected_lvalue;
    struct protected_char_lvalue *protected_char_lvalue;
    struct protected_range_lvalue *protected_range_lvalue;

      /* The following fields are used only in svalues referenced by
       * T_LVALUE svalues:
       */
    void (*error_handler) (svalue_t *);
      /* T_ERROR_HANDLER: this function is
       * executed on a free_svalue(), receiving the T_ERROR_HANDLER svalue*
       * as argument. This allows the transparent implemention of cleanup
       * functions which are called even after runtime errors. In order
       * to pass additional information to the error_handler(), embed
       * the T_ERROR_HANDLER svalue into a larger structure (possible since
       * it has to be referenced by pointer) and let the error_handler()
       * execute the appropriate casts.
       */

#ifndef INITIALIZATION_BY___INIT
    struct const_list_svalue_s *const_list;
      /* Used by the LPC compiler only: when initializing global variables
       * with static arrays, the compiler will collect the array elements
       * in a list of const_list_ts, while letting the initializer (typed
       * as T_LVALUE) point to the head structure of that list.
       * Once complete, the .u.const_list is replaced by the completed .u.vec.
       */
#endif
       
};

/* --- struct svalue_s: the LPC data structure ---
 *
 * svalues ('stack values)' are used throughout the driver to hold LPC
 * values. One svalue consists of an instance of union u to hold the
 * actual value, and a type field describing the type of the value.
 *
 * Some values need specific, additional information (e.g. string values
 * distinguish shared from allocated strings), which is stored in the
 * union 'x'.
 *
 * The T_LVALUE type family is special in that the svalue instance does
 * not contain the actual value, but instead a reference to another
 * svalue. lvalues are necessary whenever existing svalue instance
 * are to be assigned. A special case are protected lvalues where the
 * svalue referenced by the T_LVALUE.u.lvalue is not the target svalue,
 * but instead a T_PROTECTED_xxx_LVALUE which then points to the target
 * and its protector.
 *
 * T_LVALUEs are also used to reference meta data, like T_ERROR_HANDLER
 * svalues.
 */
struct svalue_s
{
    ph_int type;  /* Primary type information */
    union {       /* Secondary type information */
        ph_int string_type;  /* Allocation method of the string */
        ph_int exponent;     /* Exponent of a T_FLOAT */
        ph_int closure_type; /* Type of a T_CLOSURE */
        ph_int quotes;       /* Number of quotes of a quoted array or symbol */
        ph_int num_arg;      /* used by call_out.c to for vararg callouts */
        ph_int extern_args;  /* Callbacks: true if the argument memory was
                              * allocated externally */
        ph_int generic;
          /* For types without secondary type information, this is set to
           * a fixed value, usually (u.number << 1).
           * Also, this field is also used as generic 'secondary type field'
           * handle, e.g. when comparing svalues.
           */
    } x;
    union u u;  /* The value */
};

#define SVALUE_FULLTYPE(svp) ((p_int *)(svp))
  /* Return an integer with the primary and secondary type information.
   */
/* TODO: Sanity test: sizeof struct { ph_int, ph_int } <= sizeof p_int */


/* struct svalue_s.type: Primary types */

#define T_INVALID       0x0  /* empty svalue */
#define T_LVALUE        0x1  /* a lvalue */
#define T_NUMBER        0x2  /* an integer number */
#define T_STRING        0x3  /* a string */
#define T_POINTER       0x4  /* a vector */
#define T_OBJECT        0x5  /* an object */
#define T_MAPPING       0x6  /* a mapping */
#define T_FLOAT         0x7  /* a float number */
#define T_CLOSURE       0x8  /* a closure */
#define T_SYMBOL        0x9  /* a symbol */
#define T_QUOTED_ARRAY  0xa  /* a quoted array */

#define T_CHAR_LVALUE                     0xb
  /* .u.string points to the referenced character in a string */
  /* The following types must be used only in svalues referenced
   * by a T_LVALUE svalue.
   */
#define T_STRING_RANGE_LVALUE             0xc /* TODO: ??? */
#define T_POINTER_RANGE_LVALUE            0xd /* TODO: ??? */
#define T_PROTECTED_CHAR_LVALUE           0x0e
  /* A protected character lvalue */
#define T_PROTECTED_STRING_RANGE_LVALUE   0x0f
  /* A protected string range lvalue */
#define T_PROTECTED_POINTER_RANGE_LVALUE  0x10
  /* A protected pointer/mapping range lvalue */
#define T_PROTECTED_LVALUE                0x11
  /* A protected lvalue */
#define T_PROTECTOR_MAPPING               0x12 /* TODO: ??? */

#define T_CALLBACK                        0x13
  /* A callback structure referenced from the stack to allow
   * proper cleanup during error recoveries. The interpreter
   * knows how to free it, but that's all.
   */

#define T_ERROR_HANDLER                   0x14
  /* Not an actual value, this is used internally for cleanup
   * operations. See the description of the error_handler() member
   * for details.
   */

#define T_MOD_SWAPPED    0x80
  /* This flag is |-ed to the swapped-out type value if the value
   * data has been swapped out.
   */


/* T_STRING secondary information */

#define STRING_MALLOC    0  /* Allocated by malloc() */
#define STRING_VOLATILE  1  /* Static storage, must not be freed */
#define STRING_SHARED    2  /* Allocated by the shared string module */
  /* Constant strings are handled as volatile strings - they don't
   * appear often enough that special treatment would be valuable.
   */

/* T_CLOSURE secondary information. */

  /* For closures of operators (internal machine codes), efuns and
   * simul_efuns, the x.closure_type is a negative number defining
   * which operator/efun/simul_efun to call.
   * The values given here are just the limits of the usable number
   * ranges.
   * The relations are:
   *   Operator-closure index = CLOSURE_OPERATOR_OFFS + instruction index
   *   Efun-closure index     = CLOSURE_EFUN_OFFS + instruction index
   *   Simul_efun index       = CLOSURE_SIMUL_EFUN_OFFS + function index
   * Yes, the operator range can be overlaid onto the efun range without
   * collision, distinguishing them by the struct instr[].Default fields
   * (the 'closure' instruction does that to save space), but this way they
   * are easier to distinguish.
   * TODO: Note: Some code interprets these values as unsigned shorts.
   */

#define CLOSURE_OPERATOR        (-0x1800)  /* == 0xe800 */
#define CLOSURE_EFUN            (-0x1000)  /* == 0xf000 */
#define CLOSURE_SIMUL_EFUN      (-0x0800)  /* == 0xf800 */

#define CLOSURE_OPERATOR_OFFS   (CLOSURE_OPERATOR & 0xffff)
#define CLOSURE_EFUN_OFFS       (CLOSURE_EFUN & 0xffff)
#define CLOSURE_SIMUL_EFUN_OFFS (CLOSURE_SIMUL_EFUN & 0xffff)

  /* The other closure types are created from actual objects and lambdas,
   * the detailed information is stored in the u.lambda field.
   * The first types are 'just' references to existing lfuns and variables,
   * the others actually point to code.
   */

#define CLOSURE_LFUN            0  /* lfun in this object */
#define CLOSURE_ALIEN_LFUN      1  /* lfun in an other object */
#define CLOSURE_IDENTIFIER      2  /* variable in this object */
   
#define CLOSURE_PRELIMINARY     3
    /* Efun closure used in a static initialization */

#define CLOSURE_BOUND_LAMBDA    4  /* Bound unbound-lambda closure */
#define CLOSURE_LAMBDA          5  /* normal lambda closure */
#define CLOSURE_UNBOUND_LAMBDA  6  /* unbound lambda closure. */


#define CLOSURE_IDENTIFIER_OFFS 0xe800
  /* When creating lfun/variable closures, the lfun/variable to bind
   * is given as unsigned number:
   *   number < C_I_OFFS:  number is index into function table
   *   number >= C_I_OFFS: (number-C_I_OFFS) is index into variable table.
   */

/* Predicates operating on T_CLOSURE secondary information */

#define CLOSURE_MALLOCED(c) ((c) >= 0)
  /* TRUE if the closure was created at LPC runtime, ie is not an operator,
   * efun or simul_efun.
   */

#define CLOSURE_IS_LFUN(c)        (((c)&~1) == 0)
  /* TRUE if the closure is of the #'<lfun> type.
   */

#define CLOSURE_REFERENCES_CODE(c) ((c) > CLOSURE_PRELIMINARY)
  /* TRUE if the closure may have or reference code.
   */

#define CLOSURE_HAS_CODE(c) ((c) > CLOSURE_BOUND_LAMBDA)
  /* TRUE if the closure points to actual code.
   */

#define CLOSURE_CALLABLE(c) ((c) >= CLOSURE_EFUN && (c) <= CLOSURE_LAMBDA)
  /* TRUE if the closure is callable.
   */

/* --- Float Support --- */

/* The driver uses a 48-Bit floating point format with a 32 bit mantissa
 * and a 16 bit exponent, stored in u.mantissa and x.exponent. This should
 * be well below all existing host floating point formats so that we get
 * the same accuracy on all platforms.
 *
 * The functions to encode/decode float numbers exist in two version, one
 * fast one using internal knowledge about how the compiler stores its
 * numbers, and a second portable one. To keep the implementation
 * transparent, the following macros/functions are defined:
 *
 *   int FLOAT_FORMAT:
 *     0 for the portable format, 1 for the fast format.
 *     Additional numbers may be defined for more formats.
 *
 *   double READ_DOUBLE(struct svalue * sp)
 *     Return the floating point number stored in *sp.
 *
 *   long SPLIT_DOUBLE (double d, int * p)
 *     Store the bytes 4..5 of <d> to the address given in <p>, and
 *     return the bytes 0..3 of <d> as a long.
 *     Used by the compiler to generate F_FLOAT instructions.
 *     TODO: This code makes heave assumptions about data sizes and layout
 *     TODO:: of integral types.
 *
 *   unknown STORE_DOUBLE (struct svalue * dest, double d)
 *     Store the float <d> into the svalue *dest.
 *
 *   STORE_DOUBLE_USED
 *     Declaration of a local variable which STORE_DOUBLE needs.
 */

/* --- The fast format */

#if defined(atarist) || (defined(AMIGA) && defined(_DCC))

#define FLOAT_FORMAT_1

/* Faster routines, using inline and knowlegde about double format.
 * The exponent isn't actually in 'exponent', but that doesn't really matter
 * as long as the accesses are consistent.
 *
 * The DICE compiler for the Amiga lacks the ldexp() and frexp() functions,
 * therefore these functions here are the only way to get things done.
 *
 * STORE_DOUBLE doesn't do any rounding, but truncates off the least
 * significant bits of the mantissa that won't fit together with the exponent
 * into 48 bits. To compensate for this, we initialise the unknown bits of
 * the mantissa with 0x7fff in READ_DOUBLE . This keeps the maximum precision
 * loss of a store/read pair to the same value as rounding, while being faster
 * and being more stable.
 */

static
#ifdef atarist
inline
#endif
double READ_DOUBLE(struct svalue *svalue_pnt)
{        double tmp;
        (*(long*)&tmp) = svalue_pnt->u.mantissa;
        ((short*)&tmp)[2] = svalue_pnt->x.exponent;
        ((short*)&tmp)[3] = 0x7fff;
        return tmp;
}

#define SPLIT_DOUBLE(double_value, int_pnt) (\
            (*(int_pnt) = ((short*)&double_value)[2]),\
            *((long*)&double_value)\
        )

#define STORE_DOUBLE_USED
#define STORE_DOUBLE(dest, double_value) (\
            (dest)->u.mantissa = *((long*)&double_value),\
            (dest)->x.exponent = ((short*)&double_value)[2]\
        )
#endif

/* --- The portable format, used if no other format is defined */

#ifndef STORE_DOUBLE

#define FLOAT_FORMAT_0

#define READ_DOUBLE(svalue_pnt) ( ldexp( (double)((svalue_pnt)->u.mantissa) , \
                (svalue_pnt)->x.exponent-31 ) )

/* if your machine doesn't use the exponent to designate powers of two,
   the use of ldexp in SPLIT_DOUBLE won't work; you'll have to mulitply
   with 32768. in this case */

#define SPLIT_DOUBLE(double_value, int_pnt) \
( (long)ldexp( frexp( (double_value), (int_pnt) ), 31) )

#define STORE_DOUBLE_USED int __store_double_int_;
#define STORE_DOUBLE(dest, double_value) (\
((dest)->u.mantissa = SPLIT_DOUBLE((double_value), &__store_double_int_)),\
 (dest)->x.exponent = (short)__store_double_int_\
)

#endif /* ifndef STORE_DOUBLE */

/* --- svalue macros --- */

#define addref_closure(sp, from) \
  MACRO( svalue_t * p = sp; \
         if (CLOSURE_MALLOCED(p->x.closure_type)) \
             p->u.lambda->ref++; \
         else \
             (void)ref_object(p->u.ob, from); \
  )
  /* void addref_closure(sp, from): Add one ref to the closure svalue <sp>
   *   in the function <from>.
   */

  
/* void put_<type>(sp, value): Initialise svalue *sp with value of <type>.
 *   'sp' is evaluated several times and must point to an otherwise
 *   empty svalue. If <value> is a refcounted value, its refcount is
 *   NOT incremented.
 *
 * void put_ref_<type>(sp, value): Initialise svalue *sp with value
 *   of <type>. 'sp' is evaluated several times and must point to an
 *   otherwise empty svalue. <value> must be a refcounted value, and
 *   its refcount is incremented.
 *
 * TODO: Add push_xxx() macros, see MudOS:interpret.h. In general, get
 * TODO:: rid of the local sp/pc copies since they make error handling
 * TODO:: very difficult.
 * TODO: Also add 'adopt_<type>()' and 'pop_<type>()'macros, e.g.
 * TODO:: 'adopt_object(sp)', which
 * TODO:: retrieve the type from the *sp and set sp->type to T_INVALID.
 */

#define put_number(sp,num) \
    ( (sp)->type = T_NUMBER, (sp)->u.number = (num) )

#define put_ref_array(sp,arr) \
    ( (sp)->type = T_POINTER, (sp)->u.vec = ref_array(arr) )
#define put_array(sp,arr) \
    ( (sp)->type = T_POINTER, (sp)->u.vec = arr )

#define put_ref_mapping(sp,val) \
    ( (sp)->type = T_MAPPING, (sp)->u.map = ref_mapping(val) )
#define put_mapping(sp,val) \
    ( (sp)->type = T_MAPPING, (sp)->u.map = val )

#define put_ref_object(sp,val,from) \
    ( (sp)->type = T_OBJECT, (sp)->u.ob = ref_object(val, from) )
#define put_object(sp,val) \
    ( (sp)->type = T_OBJECT, (sp)->u.ob = val )

#define put_volatile_string(sp,val) \
    ( (sp)->type = T_STRING, (sp)->x.string_type = STRING_VOLATILE, \
      (sp)->u.string = val )
#define put_malloced_string(sp,val) \
    ( (sp)->type = T_STRING, (sp)->x.string_type = STRING_MALLOC, \
      (sp)->u.string = val )
#define put_ref_string(sp,val) \
    ( (sp)->type = T_STRING, (sp)->x.string_type = STRING_SHARED, \
      (sp)->u.string = ref_string(val) )
#define put_string(sp,val) \
    ( (sp)->type = T_STRING, (sp)->x.string_type = STRING_SHARED, \
      (sp)->u.string = val )

#define put_callback(sp,val) \
    ( (sp)->type = T_CALLBACK, (sp)->u.cb = val )

/* --- Prototypes (in interpret.c) --- */

extern void free_string_svalue(svalue_t *v);
extern void free_object_svalue(svalue_t *v);
extern void zero_object_svalue(svalue_t *v);
extern void free_svalue(svalue_t *v);
extern void assign_svalue_no_free(svalue_t *to, svalue_t *from);
extern void assign_svalue(svalue_t *dest, svalue_t *v);
extern void transfer_svalue_no_free(svalue_t *dest, svalue_t *v);
extern void transfer_svalue(svalue_t *dest, svalue_t *v);

#endif /* SVALUE_H__ */