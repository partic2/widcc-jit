#include <widcc_jit.h>
#include <stdio.h>
#include <time.h>

int main(int argc,char **argv){
    if(argc<2){
        printf("Usage:run_c_file filename.c\n");
        return 1;
    }
    const char *err=NULL;
    WidccRT *rt=widcc_rt_new();
    err=widcc_rt_load_code_file(rt, argv[1], NULL);
    if(err!=NULL){
        printf("Error %s\n",err);
        return 2;
    }
    void (*entry)();
    entry=(void (*)())widcc_rt_lookup(rt, "main");
    if(entry==NULL){
        printf("Error no main function.");
        return 3;
    }
    clock_t c=clock();
    entry();
    printf("time consumed:%d ms\n",(int)((clock()-c)/(CLOCKS_PER_SEC/1000)));
    widcc_rt_free(rt);
    return 0;
}

