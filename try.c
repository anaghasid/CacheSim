#include<stdlib.h>
#include<stdio.h>
#include<omp.h>
#include<string.h>
#include<ctype.h>

typedef char byte;

struct cache {
    byte address; // This is the address in memory.
    byte value; // This is the value stored in cached memory.
    // State for you to implement MESI protocol.
    byte state;
};

struct decoded_inst {
    int type; // 0 is RD, 1 is WR
    byte address;
    byte value; // Only used for WR 
};

typedef struct cache cache;
typedef struct decoded_inst decoded;

byte* memory;

decoded decode_inst_line(char * buffer){
    decoded inst;
    char inst_type[3]; // Increased size to accommodate 'WR' and 'RD'
    sscanf(buffer, "%s", inst_type);
    if(!strcmp(inst_type, "RD")){
        inst.type = 0;
        int addr = 0;
        sscanf(buffer, "%s %d", inst_type, &addr);
        inst.value = -1;
        inst.address = addr;
    } else if(!strcmp(inst_type, "WR")){
        inst.type = 1;
        int addr = 0;
        int val = 0;
        sscanf(buffer, "%s %d %d", inst_type, &addr, &val);
        inst.address = addr;
        inst.value = val;
    }
    return inst;
}

void cpu_loop(int num_threads){
    int cache_size = 2;
    cache * c = (cache *) malloc(sizeof(cache) * cache_size);
    
    #pragma omp parallel num_threads(num_threads) shared(c, memory)
    {
        int thread_id = omp_get_thread_num();
        char filename[15];
        sprintf(filename, "input_%d.txt", thread_id);
        FILE * inst_file = fopen(filename, "r");
        char inst_line[20];
        
        while (fgets(inst_line, sizeof(inst_line), inst_file)){
            decoded inst = decode_inst_line(inst_line);
            int hash = inst.address % cache_size;
            cache cacheline = *(c + hash);

            if(cacheline.address != inst.address){
                #pragma omp critical
                {
                    *(memory + cacheline.address) = cacheline.value;
                    cacheline.address = inst.address;
                    cacheline.state = -1;
                    cacheline.value = *(memory + inst.address);
                    if(inst.type == 1){
                        cacheline.value = inst.value;
                    }
                    *(c + hash) = cacheline;
                }
            }

            switch(inst.type){
                case 0:
                    printf("Thread %d: RD %d: %d\n", thread_id, cacheline.address, cacheline.value);
                    break;
                
                case 1:
                    printf("Thread %d: WR %d: %d\n", thread_id, cacheline.address, cacheline.value);
                    break;
            }
        }
        fclose(inst_file);
    }

    free(c);
}

int main(int argc, char * argv[]){
    int memory_size = 24;
    memory = (byte *) malloc(sizeof(byte) * memory_size);
    cpu_loop(2); // Change the argument to specify the number of threads
    free(memory);
    return 0;
}
