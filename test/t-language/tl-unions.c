/* Tests for union types (#721).
 *
 * We're only checking things, that should work.
 * Because the strength of type error detection might
 * vary in the future.
 */

#pragma strong_types
#pragma rtt_checks

struct base
{
    int member;
};

struct sub1(base)
{
    int m1;
};

struct sub2(base)
{
    int m2;
};

struct indep
{
    int member;
};

string          g_str;
int             g_int;
int|string      g_int_str;
<int|string>*   g_int_str_arr;
<int|string>**  g_int_str_arrr;
int*|string*    g_intarr_strarr;
<int*|string*>* g_intarr_strarr_arr;
< <int|string>*| symbol>* g_int_str_arr_symbol_arr;
int|float       g_nr;
struct base     g_s1;

string          f_return_str              (string val)          { return val; }
int             f_return_int              (int val)             { return val; }
int|string      f_return_int_str          (int|string val)      { return val; }
<int|string>*   f_return_int_str_arr      (<int|string>* val)   { return val; }
<int|string>**  f_return_int_str_arrr     (<int|string>** val)  { return val; }
int*|string*    f_return_intarr_strarr    (int*|string* val)    { return val; }
<int*|string*>* f_return_intarr_strarr_arr(<int*|string*>* val) { return val; }
< <int|string>*| symbol>* f_return_int_str_arr_symbol_arr(< <int|string>*| symbol>* val) { return val; }
int|float       f_return_nr               (int|float val)       { return val; }
int             f_return_member           (struct sub1|struct sub2|struct indep val) { return val->member; }

int|string|mixed* multiply(int|string|mixed* arg, int factor)   {return arg*factor;}

#pragma weak_types
just_compile_this(arg)
{
    /* Initialization and simple operations. */
    g_str = arg;
    g_str = g_int + arg;
    arg = arg + g_int;
    arg = arg[0];
    arg = multiply(arg, arg);

    return arg;
}

#pragma strong_types
int call_the_weak()
{
    /* Check mixing of available type information
     * with not-available type information.
     * Should not result in a compiler error or segfault.
     */
    return just_compile_this(10);
}

int run_test()
{
    /* Initialize variables of different types with zero.
     */
    g_str = g_int = g_int_str_arr = g_int_str_arr_symbol_arr = 0;

    /* Initialize them with values of their own kind.
     */
    g_str = "Hello!";
    g_int = 42;
    g_int_str = "Nobody here!";
    g_int_str_arr = ({ 1,2,3,"me" });
    g_int_str_arrr = ({ g_int_str_arr, ({}) });
    g_intarr_strarr = ({ 1, 1, 2, 3, 5, 8, 13 });
    g_intarr_strarr_arr = ({ g_intarr_strarr, ({ 3, 1, 4, 1 }), ({ "you" }), ({}) });
    g_int_str_arr_symbol_arr = ({ ({ 1, 2, 3, "hey" }) });
    g_nr = -1.5;
    g_s1 = (<sub1> 42, 1);

    /* Pass the values through a function.
     */
    g_str               = f_return_str(g_str);
    g_int               = f_return_int(g_int);
    g_int_str           = f_return_int_str(g_int_str);
    g_int_str_arr       = f_return_int_str_arr(g_int_str_arr);
    g_int_str_arrr      = f_return_int_str_arrr(g_int_str_arrr);
    g_intarr_strarr     = f_return_intarr_strarr(g_intarr_strarr);
    g_intarr_strarr_arr = f_return_intarr_strarr_arr(g_intarr_strarr_arr);
    g_int_str_arr_symbol_arr = f_return_int_str_arr_symbol_arr(g_int_str_arr_symbol_arr);
    g_nr                = f_return_nr(g_nr) + f_return_member(g_s1);

    /* Mix compatible types. */
    g_str = g_int_str; /* We had a string assigned to g_int_str above. */
    g_int_str = g_int;
    g_int_str_arr = g_intarr_strarr;
    g_intarr_strarr = ({int*|string*})g_int_str_arr;
                      /* Would work without a cast, just for testing. */
    g_int_str_arr_symbol_arr = g_int_str_arrr;

    /* Try some selected operations */
    g_nr += 10;
    g_int_str = g_str + g_int;

    g_int = multiply(g_int, -1);
    g_str = multiply(g_str, 1);
    g_intarr_strarr = multiply(g_intarr_strarr, 5);

    g_int_str = g_str & "o";
    g_intarr_strarr = g_intarr_strarr & (g_int_str_arr || ({}));

    /* Array indexing. */
    g_int_str = g_intarr_strarr[1];
    g_intarr_strarr = g_int_str_arr[1..5];
    g_int_str = 3;
    g_int_str = g_intarr_strarr[g_int_str];
    
    /* Refcounting. */
    {
        mapping ** vals = ({});
        foreach(mapping * val: vals) {}
    }

    return 1;
}
