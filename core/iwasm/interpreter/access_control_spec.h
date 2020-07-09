#define EVAL_NO_AEROGEL 1

#ifndef _AEROGEL_
#define _AEROGEL_

#if !EVAL_NO_AEROGEL
// The definition of the below variables are for testing purposes in wasm_runtime.c
// in the same folder.
extern char* device_spec;
/*
"\
name:imu,id:0,address:0x90000000,power:10,concurrent_access:50\n\
name:camera,id:1,address:0x8ffffffc,power:500,concurrent_access:2\n\
name:motion,id:2,address:0x8ffffff0,power:20,concurrent_access:65\n\
name:microphone,id:3,address:0x8FFFFFEC,power:100,concurrent_access:3\n\
name:speaker,id:4,address:0x8FFFFFE4,power:400,concurrent_access:1\n\
name:door_motor,id:5,address:0x8FFFFFE0,power:300,concurrent_access:1\n\
name:window_motor,id:6,address:0x8FFFFFC0,power:350,concurrent_access:1\n\
mcu,power:100\n\
";
*/

extern char* module_spec;
/*
"\
name:regular1,device:imu-10000.motion-9000.speaker-20000.window_motor-5000,mcu:9000,memory:200\n\
name:regular2,device:camera-10000.speaker-30000,mcu:9000,memory:500\n\
name:regular3,device:camera-10000.microphone-5000,mcu:9000,memory:500\n\
name:max_concurrent1,device:camera-10000,mcu:9000,memory:500\n\
name:max_concurrent2,device:microphone-10000,mcu:9000,memory:500\n\
name:max_concurrent3,device:microphone-10000.door_motor-10000,mcu:9000,memory:500\n\
name:max_concurrent4,device:microphone-10000.window_motor-10000,mcu:9000,memory:500\n\
name:low_pow,device:imu-500.camera-1000.door_motor-900,mcu:5000,memory:300\n\
name:low_mcu,device:imu-10000,mcu:200,memory:500\n\
name:low_memory,device:imu-10000,mcu:10000,memory:10\n\
";
*/
#endif

#endif
