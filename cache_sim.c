#include <ctype.h>
#include <omp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef char byte;
enum CacheState { INVALID, SHARED, EXCLUSIVE, MODIFIED };

enum BusState {
  READ_RQ,  // read request to a cache block requested by another processor that
            // does not already have the block
  READ_RES, // one for read response
  READX,    // write request requested by another processor. On getting this,
            // invalidate the cache copy
  STOP_THREAD // mail sent by cache to its own mailbox to stop its operation
};

struct cache {
  int address;           // This is the address in memory.
  int value;             // This is the value stored in cached memory.
  enum CacheState state; // easier to represent this way
};

struct decoded_inst {
  int type; // 0 is RD, 1 is WR
  int address;
  int value; // Only used for WR
};

struct mail {
  // write cacheline here instead?
  int sender;
  byte address;
  byte value;
  enum CacheState sender_state;
  enum BusState message;
  bool done;
  pthread_mutex_t lock; // is this a good method?
};

typedef struct cache cache;
typedef struct mail mail;
typedef struct decoded_inst decoded;

/*
 * This is a very basic C cache simulator.
 * The input files for each "Core" must be named core_1.txt, core_2.txt,
 * core_3.txt ... core_n.txt Input files consist of the following instructions:
 * - RD <address>
 * - WR <address> <val>
 */

byte *memory;

// Decode instruction lines
decoded decode_inst_line(char *buffer) {
  decoded inst;
  char inst_type[3];
  sscanf(buffer, "%s", inst_type);
  int s = strcmp(inst_type, "RD");
  if (s == 0) {
    inst.type = 0;
    int addr = 0;
    sscanf(buffer, "%s %d", inst_type, &addr);
    inst.value = -1;
    inst.address = addr;
  } else {
    inst.type = 1;
    int addr = 0;
    int val = 0;
    sscanf(buffer, "%s %d %d", inst_type, &addr, &val);
    inst.address = addr;
    inst.value = val;
  }
  return inst;
}

// Helper function to print the cachelines
void print_cachelines(cache *c, int cache_size) {
  for (int i = 0; i < cache_size; i++) {
    cache cacheline = *(c + i);
    printf("Address: %d, State: %d, Value: %d\n", cacheline.address,
           cacheline.state, cacheline.value);
  }
}

mail send_read_message(cache cacheline, int sender) {
  mail mailbox;
  mailbox.sender = sender;
  mailbox.address = cacheline.address;
  mailbox.value = -1;
  mailbox.message = READ_RQ;
  mailbox.sender_state = INVALID;
  mailbox.done = 0;
  return mailbox;
}

mail read_bus_msg(int thread_num, mail *mail) { return mail[thread_num]; }

void handle_msg_from_bus(int thread_num, mail *mailboxes, cache *c,
                         byte *memory, int cache_size) {
  // does not handle read responses because that will be done in the executing
  // thread
  while (1) {
    // ACQUIRE LOCK
    mail mailbox;
    pthread_mutex_lock(&mailboxes[thread_num].lock);
    mailbox = read_bus_msg(thread_num, mailboxes);
    pthread_mutex_unlock(&mailboxes[thread_num].lock);

    // stop this thread when the cache sends a message to its own mailbox to
    // stop
    if (mailbox.message == STOP_THREAD) {
      printf("Stopping thread\n");
      break;
    }
    // if no new message, wait for sometime
    else if (mailbox.done == 1) {
      sleep(0.002);
      continue;
    }

    else if (mailbox.message == READ_RQ) {
      int hash = mailbox.address % cache_size;
      cache cacheline = *(c + hash);
      if (cacheline.address == mailbox.address) {
        // found the requested copy in the cache
        if (cacheline.state == EXCLUSIVE || cacheline.state == MODIFIED) {
          cacheline.state = SHARED;
        }

        if (cacheline.state == INVALID) {
          continue;
        }

        // now find the cache that requested it and write to its mailbox
        int sender = mailbox.sender;
        pthread_mutex_lock(&mailboxes[sender].lock);
        mailboxes[sender].address = cacheline.address;
        mailboxes[sender].value = cacheline.value;
        mailboxes[sender].sender_state = SHARED;
        mailboxes[sender].sender = thread_num;
        mailboxes[sender].message = READ_RES;
        mailboxes[sender].done = 0;
        pthread_mutex_unlock(&mailboxes[sender].lock);
      }
    }

    else if (mailbox.message == READX) {
      // other cache is trying to invalidate your value
      int hash = mailbox.address % cache_size;
      cache cacheline = *(c + hash);
      if (cacheline.address == mailbox.address) {
        // found the requested copy in the cache
        if (cacheline.state == MODIFIED) {
          *(memory + cacheline.address) = cacheline.value;
          cacheline.state = INVALID;
        }

        else {
          cacheline.state = INVALID;
        }
      }
    } else if (mailbox.message == READ_RES) {
      int hash = mailbox.address % cache_size;
      // LOCK THE CACHELINE?
      cache cacheline = *(c + hash);
      cacheline.address = mailbox.address;
      cacheline.value = mailbox.value;
      cacheline.state = SHARED;
      // UNLOCK THE CACHELINE?
    }
    sleep(0.002);
  }
}

// This function implements the mock CPU loop that reads and writes data.
void cpu_loop(int num_threads) {
  // Initialize a CPU level cache that holds about 2 bytes of data.
  mail *mailboxes = (mail *)malloc(sizeof(mail) * num_threads);
  for (int i = 0; i < num_threads; i++) {
    pthread_mutex_init(&mailboxes[i].lock, NULL);
  }

  omp_set_nested(2);
#pragma omp parallel num_threads(num_threads) shared(memory, mailboxes)
  {
    int cache_size = 2;
    cache *c = (cache *)malloc(sizeof(cache) * cache_size);

// share cache between executing thread and bus reading thread
#pragma omp parallel shared(c, mailboxes)
    {
#pragma omp sections
      {
// one section for executing the commands
#pragma omp section
        {
          int thread_num = omp_get_ancestor_thread_num(1);
          printf("Executing thread number %d\n", thread_num);
          char filename[15];
          sprintf(filename, "input_%d.txt", thread_num);
          FILE *inst_file = fopen(filename, "r");
          // FILE* inst_file = fopen(("input_%d.txt",thread_num), "r");

          char inst_line[20];
          // Decode instructions and execute them.
          while (fgets(inst_line, sizeof(inst_line), inst_file)) {
            decoded inst = decode_inst_line(inst_line);
            int hash = inst.address % cache_size;
            cache cacheline = *(c + hash);
            // print_cachelines(c, cache_size);
            // printf("\n");

            if (cacheline.address == inst.address) {
              if (inst.type == 0) {
                // READ HIT
                if (cacheline.state == MODIFIED ||
                    cacheline.state == EXCLUSIVE || cacheline.state == SHARED) {
                  // printf("Reading from address %d: %d\n", cacheline.address,
                  // cacheline.value);
                  // printf("Read hit for %d\n", inst.address);
                }

                // READ MISS
                else {
                  // broadcast read message
                  // printf("Read miss for %d\n", inst.address);
                  for (int i = 0; i < cache_size; i++) {
                    if (i != thread_num) {
                      mail request = send_read_message(cacheline, thread_num);
                      pthread_mutex_lock(&mailboxes[i].lock);
                      mailboxes[i] = request;
                      pthread_mutex_unlock(&mailboxes[i].lock);
                    }
                  }

                  // when the response is sent by another cache it is read in
                  // handle_bus_messages() but if no cache has it (check after 2
                  // seconds)

                  sleep(2);
                  if (cacheline.state != SHARED) {
                    // this means no other cache has it
                    // read from memory
                    cacheline.value = *(memory + cacheline.address);
                    cacheline.state = EXCLUSIVE;
                  }
                }
                // printf("Reading from address %d: %d\n", cacheline.address,
                // cacheline.value);

              }

              else {
                // WRITE HIT
                // printf("Write hit for %d\n", inst.address);

                if (cacheline.state == MODIFIED) {
                  cacheline.value = inst.value;
                }

                else if (cacheline.state == EXCLUSIVE) {
                  cacheline.value = inst.value;
                  cacheline.state = MODIFIED;
                } else if (cacheline.state == SHARED) {
                  // broadcast invalidate address on bus
                  //  acquire lock
                  for (int i = 0; i < cache_size; i++) {
                    if (i != thread_num) {
                      pthread_mutex_lock(&mailboxes[i].lock);
                      mailboxes[i].address = cacheline.address;
                      mailboxes[i].value = -1;
                      mailboxes[i].message = READX;
                      mailboxes[i].done = 0;
                      pthread_mutex_unlock(&mailboxes[i].lock);
                    }
                  }
                  // release lock
                  cacheline.value = inst.value;
                  cacheline.state = MODIFIED;
                }

                else {
                  // WRITE MISS
                  // printf("Write miss for %d\n", inst.address);

                  // broadcast invalidate message to all other caches
                  for (int i = 0; i < num_threads; i++) {
                    if (i != thread_num) {
                      pthread_mutex_lock(&mailboxes[i].lock);
                      mailboxes[i].address = cacheline.address;
                      mailboxes[i].value = -1;
                      mailboxes[i].sender_state = MODIFIED;
                      mailboxes[i].sender = thread_num;
                      mailboxes[i].message = READX;
                      mailboxes[i].done = 0;
                      pthread_mutex_unlock(&mailboxes[i].lock);
                    }
                  }
                  cacheline.address = inst.address;
                  cacheline.value = inst.value;
                  cacheline.state = MODIFIED;
                }
              }
            }

            else {
              // READ MISS
              if (inst.type == 0) {
                printf("This is a read miss\n");
                // broadcast read message
                for (int i = 0; i < cache_size; i++) {
                  if (i != thread_num) {
                    mail request = send_read_message(cacheline, thread_num);
                    pthread_mutex_lock(&mailboxes[i].lock);
                    mailboxes[i] = request;
                    pthread_mutex_unlock(&mailboxes[i].lock);
                  }
                }

                // when the response is sent by another cache it is read in
                // handle_bus_messages() but if no cache has it (check after 2
                // seconds)

                sleep(2);
                if (cacheline.state != SHARED) {
                  // this means no other cache has it
                  // read from memory
                  cacheline.address = inst.address;
                  cacheline.value = *(memory + cacheline.address);
                  cacheline.state = EXCLUSIVE;
                }
              }

              // WRITE MISS
              else {
                // printf("This is a write miss\n");
                for (int i = 0; i < num_threads; i++) {
                  if (i != thread_num) {
                    pthread_mutex_lock(&mailboxes[i].lock);
                    mailboxes[i].address = cacheline.address;
                    mailboxes[i].value = -1;
                    mailboxes[i].sender_state = MODIFIED;
                    mailboxes[i].sender = thread_num;
                    mailboxes[i].message = READX;
                    mailboxes[i].done = 0;
                    pthread_mutex_unlock(&mailboxes[i].lock);
                  }
                }
                // printf("Modifying cacheline\n");
                cacheline.address = inst.address;
                cacheline.value = inst.value;
                cacheline.state = MODIFIED;
                // printf("cacheline.address = %d\n", cacheline.address);
              }
            }
            // printf("Writing to address %d: %d\n", cacheline.address,
            // cacheline.value);
            printf("Thread %d: %s %d: %d\n", thread_num,
                   inst.type == 0 ? "RD" : "WR", cacheline.address,
                   cacheline.value);
            *(c + hash) = cacheline;
          }
          pthread_mutex_lock(&mailboxes[thread_num].lock);
          mailboxes[thread_num].message = STOP_THREAD;
          pthread_mutex_unlock(&mailboxes[thread_num].lock);
          free(c);
        }

#pragma omp section
        {
          int thread_num = omp_get_ancestor_thread_num(1);

          handle_msg_from_bus(thread_num, mailboxes, c, memory, cache_size);
        }
      }
    }
  }
}

int main(int c, char *argv[]) {
  // Initialize Global memory
  // Let's assume the memory module holds about 24 bytes of data.
  int memory_size = 24;
  memory = (byte *)malloc(sizeof(byte) * memory_size);
  cpu_loop(2);
  free(memory);
}

/*
// Structure:
#pragma omp parallel num_threads(100) shared(bus)
{
    #pragma omp parallel num_threads(2) shared(bus)
    {
        // bus shared inside one cache and its bus access
        #pragma omp section
        {
            //cache read write

            #pragma omp critical
            send_to_bus()
        }
        #pragma omp section
        {
            // bus snooping
            // when to stop this?

            // while

        }
    }
}
*/

// Read Input file
/* // OR
char filename[15];
sprintf(filename, "input_%d.txt", omp_get_thread_num());
FILE * inst_file = fopen(filename, "r");
*/
