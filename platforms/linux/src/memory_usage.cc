// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "memory_usage.h"

#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

#define VMRSS_LINE 22
#define VMSIZE_LINE 18
#define PROCESS_ITEM 14
 
typedef struct {
    unsigned long user;
    unsigned long nice;
    unsigned long system;
    unsigned long idle;
}TotalCpuOccupy;
 
 
typedef struct {
    unsigned int pid;
    unsigned long utime;  //user time
    unsigned long stime;  //kernel time
    unsigned long cutime; //all user time
    unsigned long cstime; //all dead time
}ProcCpuOccupy;
 

static unsigned int g_pid=0;
 
//获取第N项开始的指针
const char* GetItems(const char*buffer ,unsigned int item){
    
    const char *p =buffer;
 
    int len = strlen(buffer);
    unsigned int count = 0;
    
    for (int i=0; i<len;i++){
        if (' ' == *p){
            count ++;
            if(count == item -1){
                p++;
                break;
            }
        }
        p++;
    }
 
    return p;
}
 
 
//获取总的CPU时间
unsigned long GetCpuTotalOccypy(){
    
    FILE *fd;
    char buff[1024]={0};
    TotalCpuOccupy t;
 
    fd =fopen("/proc/stat","r");
    if (nullptr == fd){
        return 0;
    }
        
    char* ret = fgets(buff,sizeof(buff),fd);
    if (ret == NULL)
    {
        fclose(fd);
        return 0;
    }
    char name[64]={0};
    sscanf(buff,"%63s %lu %lu %lu %lu", name, &t.user, &t.nice, &t.system, &t.idle);
    fclose(fd);
    
    return (t.user + t.nice + t.system + t.idle);
}
 
 
//获取进程的CPU时间
unsigned long GetCpuProcessOccypy(unsigned int pid){
    
    char file_name[64]={0};
    ProcCpuOccupy t;
    FILE *fd;
    char line_buff[1024]={0};
    sprintf(file_name,"/proc/%d/stat",pid);
    
    fd = fopen(file_name,"r");
    if(nullptr == fd){
        return 0;
    }
    
    char* ret = fgets(line_buff,sizeof(line_buff),fd);
    if (ret == NULL)
    {
        fclose(fd);
        return 0;
    }
     
    sscanf(line_buff,"%u",&t.pid);
    const char *q =GetItems(line_buff,PROCESS_ITEM);
    sscanf(q,"%lu %lu %lu %lu", &t.utime, &t.stime, &t.cutime, &t.cstime);
    fclose(fd);
    
    return (t.utime + t.stime + t.cutime + t.cstime);
}
 
 
// get cpu useage in process
float GetProcessCpu(unsigned int pid){
    
    unsigned long totalcputime1,totalcputime2;
    unsigned long procputime1,procputime2;
    
    totalcputime1=GetCpuTotalOccypy();
    procputime1=GetCpuProcessOccypy(pid);
 
    usleep(2000);
 
    totalcputime2=GetCpuTotalOccypy();
    procputime2=GetCpuProcessOccypy(pid);
    
    float pcpu = 0.0;
    if(0 != totalcputime2-totalcputime1){ 
        pcpu=100.0 * (procputime2-procputime1)/(totalcputime2-totalcputime1);
    }
    
    return pcpu;
}
 
 
// get memory useage in procee (KB)
unsigned int GetProcessMemory(unsigned int pid) {
    
    char file_name[64]={0};
    FILE *fd;
    char line_buff[512]={0};
    sprintf(file_name,"/proc/%d/status",pid);
    
    fd =fopen(file_name,"r");
    if(nullptr == fd){
        return 0;
    }
    
    char name[64];
    int vmrss;
    char* ret = NULL;
    for (int i=0; i<VMRSS_LINE-1;i++){
        ret = fgets(line_buff,sizeof(line_buff),fd);
        if (ret == NULL)
        {
            fclose(fd);
            return 0;
        }
    }
    
    ret = fgets(line_buff,sizeof(line_buff),fd);
    if (ret == NULL)
    {
        fclose(fd);
        return 0;
    }

    sscanf(line_buff,"%63s %d",name,&vmrss);
    fclose(fd);
 
    return vmrss;
}
 
 
// get virtual memory useage in procee (KB)
unsigned int GetProcessVirtualMem(unsigned int pid){
    
    char file_name[64]={0};
    FILE *fd;
    char line_buff[512]={0};
    sprintf(file_name,"/proc/%d/status",pid);
    
    fd =fopen(file_name,"r");
    if(nullptr == fd){
        return 0;
    }
    
    char name[64];
    int vmsize;
    char* ret = NULL;
    for (int i=0; i<VMSIZE_LINE-1;i++){
        ret = fgets(line_buff,sizeof(line_buff),fd);
        if (ret == NULL)
        {
            fclose(fd);
            return 0;
        }
    }
    
    ret = fgets(line_buff,sizeof(line_buff),fd);
    if (ret == NULL)
    {
        fclose(fd);
        return 0;
    }

    sscanf(line_buff,"%511s %d",name,&vmsize);
    fclose(fd);
 
    return vmsize;
}
 
 
int GetPid(const char* process_name = nullptr, const char* user = nullptr)
{
    if (nullptr == user){
        user = getlogin();  
    }

    if (nullptr == process_name) {
        return (int)getpid();
    }
    
    char cmd[512];
    cmd[0] = '\0';
    if (user){
        sprintf(cmd, "pgrep %s -u %s", process_name, user); 
    }
 
    FILE *pstr = popen(cmd,"r");    
    
    if(pstr == nullptr){
        return 0;   
    }
 
    char buff[512];
    ::memset(buff, 0, sizeof(buff));
    if(NULL == fgets(buff, 512, pstr)){
        pclose(pstr);
        return 0;
    }
 
    pclose(pstr);
    return atoi(buff);
}
 
void PrintMemInfo(const char* process_name, const char* user)
{
    if (g_pid == 0)
        g_pid = GetPid(process_name, user);

    printf("[Memory] ******* Memory Usage ********\n");
    printf("[Memory] process name = %s\n", process_name);
    printf("[Memory] pid = %d\n", g_pid);
    printf("[Memory] procmem = %d KB\n", GetProcessMemory(g_pid));
    printf("[Memory] virtualmem = %d KB\n", GetProcessVirtualMem(g_pid)); 
    printf("[Memory] *******     END      ********\n");
}
