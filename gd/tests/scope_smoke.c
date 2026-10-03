#include "../src/gdb.h"
#include <stdio.h>
#include <string.h>

static GdbChild *find(GdbChild *items,int count,const char *name) {
    for(int i=0;i<count;i++)if(!strcmp(items[i].name,name))return &items[i];
    return NULL;
}

int main(int argc,char **argv) {
    if(argc!=3)return 2;
    Gdb g;GdbChild items[GD_MAX_CHILDREN];int count=0,rc=1;
    if(gdb_start(&g,argv[1],NULL))return 1;
    if(gdb_scope_candidates(&g,argv[2],10,"",items,GD_MAX_CHILDREN,&count))goto done;
    GdbChild *dev=find(items,count,"dev"),*value=find(items,count,"value"),*cursor=find(items,count,"cursor");
    if(!dev||!value||!cursor||!strstr(dev->type,"Device")||!strchr(dev->type,'*')||strcmp(value->type,"int"))goto done;
    if(g.state!=GDB_NOT_STARTED||g.arg_count||g.local_count||g.break_count)goto done;
    if(gdb_scope_candidates(&g,argv[2],10,"dev",items,GD_MAX_CHILDREN,&count))goto done;
    GdbChild *status=find(items,count,"status"),*history=find(items,count,"history");
    if(!status||!history||strcmp(status->type,"int")||strcmp(status->expression,"(dev)->status"))goto done;
    if(gdb_scope_candidates(&g,argv[2],22,"",items,GD_MAX_CHILDREN,&count))goto done;
    dev=find(items,count,"dev");
    if(!dev||strchr(dev->type,'*')||find(items,count,"value"))goto done;
    if(gdb_scope_candidates(&g,argv[2],22,"dev",items,GD_MAX_CHILDREN,&count))goto done;
    status=find(items,count,"status");
    if(!status||strcmp(status->expression,"(dev).status"))goto done;
    if(g.state!=GDB_NOT_STARTED)goto done;
    rc=0;
done:
    if(rc)fprintf(stderr,"scope smoke failed: %s\n",g.message);
    gdb_shutdown(&g);
    if(!rc)puts("scope smoke: ok");
    return rc;
}
