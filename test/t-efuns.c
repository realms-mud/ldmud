#include "/inc/base.inc"
#include "/inc/testarray.inc"
#include "/inc/gc.inc"

mixed *tests = ({
    // TODO: Add cases for indexing at string end ("abc"[3])
    ({ "[ 1", 0,        (: ({1,2,3,4,5})[4] == 5 :) }),
    ({ "[ 2", TF_ERROR, (: ({1,2,3,4,5})[5] :) }),
    ({ "[ 3", TF_ERROR, (: ({1,2,3,4,5})[-1] :) }),
    ({ "[ 4", TF_ERROR, (: ({})[0] :) }),
    ({ "[ 5", 0,        (: "Bla, bla, blup."[0] == 'B' :) }),
    ({ "[ 6", TF_ERROR, (: "Bla."[5] :) }),
    ({ "[ 7", TF_ERROR, (: "Bla."[-1] :) }),
    ({ "[< 1", 0,        (: ({1,2,3,4,5})[<4] == 2 :) }),
    ({ "[< 2", TF_ERROR, (: ({1,2,3,4,5})[<6] :) }),
    ({ "[< 3", TF_ERROR, (: ({1,2,3,4,5})[<-1] :) }),
    ({ "[< 4", TF_ERROR, (: ({1,2,3,4,5})[<0] :) }),
    ({ "[< 5", TF_ERROR, (: ({})[<1] :) }),
    ({ "[< 6", 0,        (: "Bla, bla, blup."[<1] == '.' :) }),
    ({ "[< 7", TF_ERROR, (: "Bla."[<-1] :) }),
    // TODO: #'[..], #'[<..], #'[..<], #'[<..<]
    ({ "abs 1", 0,        (: abs(20) == 20 :) }),
    ({ "abs 2", 0,        (: abs(-20) == 20 :) }),
    ({ "abs 3", 0,        (: abs(__INT_MAX__) == __INT_MAX__ :) }),
    ({ "abs 4", 0,        (: abs(-__INT_MAX__) == __INT_MAX__ :) }),
    ({ "abs 5", TF_ERROR, (: funcall(#'abs,"20") :) }),
    ({ "abs 6", TF_ERROR, (: funcall(#'abs, 1, 2) :) }),
    ({ "abs 7", TF_ERROR, (: funcall(#'abs) :) }),
    ({ "abs 8", 0,        (: abs(20.3) == 20.3 :) }),
    ({ "abs 9", 0,        (: abs(-20.3) == 20.3 :) }),
    ({ "abs 10", 0,        (: abs(__FLOAT_MAX__) == __FLOAT_MAX__ :) }),
    ({ "abs 11", 0,        (: abs(-__FLOAT_MAX__) == __FLOAT_MAX__ :) }),
    ({ "abs 12", TF_ERROR, (: funcall(#'abs, 1.0, 2) :) }),
    ({ "acos 1", 0,        (: acos(1.0) == 0 :) }),
    ({ "acos 2", TF_ERROR, (: funcall(#'acos, "1.0") :) }),
    ({ "acos 3", TF_ERROR, (: acos(1.1) :) }),
    ({ "acos 4", TF_ERROR, (: acos(-1.1) :) }),
    ({ "all_environment 1", 0,
	(:
	    object o = clone_object(this_object());
	    mixed res;
            set_environment(o, this_object());
	    res = all_environment(o);
	    destruct(o);
	    return sizeof(res) == 1 && res[0] == this_object();
	:)
    }),
/* Doku - Korrektur: all_environment liefert bei zerstoerten Objekten einen Fehler und keine 0
    ({ "all_environment", 0, 
	(:
	    object o = clone_object(this_object());
            mixed arr = ({o});
            set_environment(o, this_object());
	    destruct(o);
	    return !all_environment(arr[0]);
	:)
    }),
*/
    ({ "all_environment 2", 0, (: all_environment(this_object()) == 0 :) }),
    ({ "all_inventory 1", 0,
	(:
	    mixed res = all_inventory(this_object());
	    return pointerp(res) && !sizeof(res);
	:)
    }),
    ({ "all_inventory 2", 0,
	(:
	    object o = clone_object(this_object());
	    mixed inv, res;

            set_environment(o, this_object());
	    inv = all_inventory(this_object());
	    res = sizeof(inv)==1 && clonep(inv[0]);
	    if(res)
		destruct(inv[0]);
	    return res;
	:)
    }),
    ({ "all_inventory 3", 0, 
	(:
	    object o = clone_object(this_object());
	    set_environment(o, this_object());
	    destruct(o);
	    return sizeof(all_inventory(this_object())) == 0;
	:)
    }),
    ({ "asin 1", 0,        (: asin(0.0) == 0 :) }),
    ({ "asin 2", TF_ERROR, (: funcall(#'asin,"1.0") :) }),
    ({ "asin 3", TF_ERROR, (: asin(1.1) :) }),
    ({ "asin 4", TF_ERROR, (: asin(-1.1) :) }),
});

void run_test()
{
    msg("\nRunning test suite for efuns:\n"
          "-----------------------------\n");

    run_array(tests,
        (:
            if($1)
                shutdown(1);
            else
                start_gc(#'shutdown);

            return 0;
        :));
}

string *epilog(int eflag)
{
    run_test();
    return 0;
}