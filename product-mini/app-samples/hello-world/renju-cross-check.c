#include <stdio.h>
#include <stdint.h>

#define uint8 uint8_t

void test_call_wasm_runtime_native(void);
void aerogel_sensor_native(
  uint8* sensor_name_list, // Name with '\t' separation and '\0' end.
  int len_sensor_name_list,
  uint8* frequency, // This nneds to be cast to uint32
  int len_frequency,
  uint8* duration, // This needs to be cast to uint32
  int len_duration,
  uint8* ret_list, // return value of each sensor. Let's just flatten it.
  int len_ret_list);

void aerogetl_actuator_native(
  uint8* actuator_name_list, // Name with '\t' separation and '\0' end
  int len_actuator_name_list,
  uint8* val_list,
  int len_val_list,
  uint8* len_val,
  int len_len_val,
  uint8* repetition,
  int len_repetition,
  uint8* latency,
  int len_latency);

int main(int argc, char **argv)
{
    printf("Hello Renju!2\n");
    test_call_wasm_runtime_native();
    return 0;
}
