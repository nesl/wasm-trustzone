/*
  The example spec sheet parsed for the access control.
*/

char* device_spec =
"\
name:imu,power:10,concurrent_access:50\n\
name:camera,power:500,concurrent_access:2\n\
name:motion,power:20,concurrent_access:65\n\
name:microphone,power:100,concurrent_access:3\n\
name:speaker,power:400,concurrent_access:1\n\
name:door_motor,power:300,concurrent_access:1\n\
name:window_motor,power:350,concurrent_access:1\n\
mcu,power:100\n\
";

char* module_spec =
"\
name:regular1,device:imu-10000.motion-9000.speaker-20000.window_motor-5000,mcu:9000,memory:200\n\
name:regular2,device:camera-10000.speaker-30000,mcu:9000,memory:500\n\
name:regular3,device:camera-10000.microphone-5000,mcu:9000,memory:500\n\
name:max_concurrent1,device:camera-10000,mcu:9000,memory:500\n\
name:max_concurrent2,device:microphone-10000,mcu:9000,memory:500\n\
name:max_concurrent3,device:microphone-10000.door_motor:10000,mcu:9000,memory:500\n\
name:max_concurrent4,device:microphone-10000.window_motor:10000,mcu:9000,memory:500\n\
name:low_pow,device:imu-500.camera-1000.door_motor-900,mcu:5000,memory:300\n\
name:low_mcu,device:imu-10000,mcu:200,memory:500\n\
name:low_memory,device:imu-10000,mcu:10000,memory:10\n\
";
