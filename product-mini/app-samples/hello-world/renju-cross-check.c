#include <stdio.h>
#include <stdint.h>

#define uint32 uint32_t

typedef struct aerogel_sensor {
  char *sensor_name;
  uint32 freq;
  uint32 duration; //in usec
} aerogel_sensor;

typedef struct aerogel_val {
  char *sensor_name;
  uint32** value;
  uint32 len_value;
  uint32* num_ret_val;
} aerogel_val;

typedef struct aerogel_actuator {
  char *actuator_name;
  uint32* val;
  uint32 len_val;
  uint32 repetition;
  uint32 latency;
} aerogel_actuator;

void test_call_wasm_runtime_native(void);

int main(int argc, char **argv)
{
    printf("Hello Renju!2\n");
    test_call_wasm_runtime_native();
    return 0;
}
