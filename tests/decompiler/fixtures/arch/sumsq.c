int sq(int x){return x*x;}
int main(int argc, char**argv){int s=0; for(int i=0;i<argc;i++) s+=sq(i); return s;}
