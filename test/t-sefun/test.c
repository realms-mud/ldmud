#include "/inc/deep_eq.inc"
#include "/inc/msg.inc"
#include "/inc/testarray.inc"

mixed* fun(string arg, int num)
{
    return ({arg, num});
}

void run_test()
{
    msg("\nRunning test for simul-efuns:\n"
          "-----------------------------\n");

    run_array(({
        ({ "Calling sefun0000", 0,
            (:
                return sefun0000() == 0;
            :)
        }),
        ({ "Calling sefunFFF0", 0,
            (:
                return sefunFFF0() == 0xfff0;
            :)
        }),
        ({ "Calling increment()", 0,
            (:
                return increment(100) == 102;
            :)
        }),
        ({ "Calling simul-efun call_strict", 0,
            (:
                object ob = this_object();
                return deep_eq(ob.fun(10), ({"sefun", 10}));
            :)
        }),
        ({ "Calling simul-efun call_other", 0,
            (:
                object ob = this_object();
                return deep_eq(ob->fun(20), ({"sefun", 20}));
            :)
        }),
        ({ "Calling simul-efun with optional arguments", 0,
            (:
                return opt_args_sefun(1, 3) == 531;
            :)
        }),
        ({ "Calling simul-efun closure with optional arguments", 0,
            (:
                return funcall(#'opt_args_sefun, 1, 3) == 531;
            :)
        }),
        ({ "Lambda: Calling simul-efun with optional arguments", 0,
            lambda(0, ({#'==, ({#'opt_args_sefun, 1, 3}), 531})),
        }),
        ({
           "Loading master using simul-efun closure", 0,
           (:
               object ob = find_object("/master");
               rename("/master.c", "/master-old.c");
               copy_file("/master-new.c", "/master.c");

               destruct(ob);

               catch(master());
               rm("/master.c");
               rename("/master-old.c", "/master.c");

               return find_object("/master").is_old_master();
           :)
        }),
    }), #'shutdown);

}
