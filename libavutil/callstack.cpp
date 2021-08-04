#if CONFIG_USE_CALLSTACK
#include <stdlib.h>
#include <stdio.h>
#include "utils/CallStack.h"
#ifdef __cplusplus
extern "C"
{
#endif

long long getTimestamp() {
    struct timeval begin;
    gettimeofday(&begin,NULL);
    return (long long)begin.tv_sec * 1000000 + (long long)begin.tv_usec;
}

void writeMemDebugCallStack(FILE* file, void* ptr, size_t size) {
    android::CallStack stack;
    stack.update();
    char ptr_str[256]={0};
    int len = sprintf(ptr_str,"[%lld] %16p [%d]\r\n",getTimestamp(),ptr, size);
    fwrite(ptr_str,1,len,file);
    android::String8 stack_str = android::CallStack::stackToString(NULL,&stack);
    fwrite(stack_str.c_str(),1,stack_str.size(),file);
    fwrite("\r\n",1,2,file);
}


#ifdef __cplusplus
}
#endif


#endif //CONFIG_USE_CALLSTACK

