virtual inherit "a";

private string c_var = "c";

int c_calc(int x)
{
    return (calc(x) + a::calc(x))/2;
}

private string get_program()
{
    return "c";
}

string get_c_var()
{
    return c_var;
}