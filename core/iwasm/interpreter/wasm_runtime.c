/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_runtime.h"
#include "wasm_loader.h"
#include "wasm_interp.h"
#include "bh_common.h"
#include "bh_log.h"
#include "mem_alloc.h"
#include "../common/wasm_runtime_common.h"
#include "access_control_spec.h"
#include "../include/access_control_global_variable.h"

// Drone's first.
char* device_spec =
"\
name:imu,id:0,address:0x90000000,power:10,concurrent_access:50\n\
name:camera,id:1,address:0x8ffffffc,power:500,concurrent_access:2\n\
name:motion,id:2,address:0x8ffffff0,power:20,concurrent_access:65\n\
name:microphone,id:3,address:0x8FFFFFEC,power:100,concurrent_access:3\n\
name:speaker,id:4,address:0x8FFFFFE4,power:400,concurrent_access:1\n\
name:door_motor,id:5,address:0x8FFFFFE0,power:300,concurrent_access:1\n\
name:propeller,id:6,address:0x8FFFFFC0,power:350,concurrent_access:1\n\
name:home_camera_control,id:7,address:0x8FFFFFC4,power:400,concurrent_access:1\n\
name:home_camera_image,id:8,address:0x8FFFFFCC,power:500,concurrent_access:30\n\
name:door_battery,id:9,address:0x8FFFFFC8,power:20,concurrent_access:30\n\
mcu,power:1\n\
";

//first 3 are for the tests
char* module_spec =
"\
name:regular1,device:camera-10000.speaker-30000,mcu:9000,memory:200000\n\
name:regular2,device:imu-10000.motion-9000.camera-100000.speaker-20000.propeller-5000.door_motor-100000,mcu:1000000,memory:200000\n\
name:regular3,device:camera-10000.microphone-5000,mcu:9000,memory:200000\n\
name:regular_uav1,device:imu-10000.camera-10000.propeller-10000,mcu:100000,memory:200000\n\
name:regular_uav2,device:imu-10000.camera-10000,mcu:100000,memory:200000\n\
name:regular_smarthome1,device:microphone-10000.speaker-10000.door_battery-10000.home_camera_image-50000,mcu:100000,memory:200000\n\
name:regular_smarthome2,device:motion-100000.home_camera_image-50000.home_camera_control-50000.door_motor-50000,mcu:100000,memory:200000\n\
name:shortage_camera_power,device:home_camera_image-50,mcu:100000,memory:200000\n\
name:shortage_memory_usage,device:imu-10000.camera-10000,mcu:100000,memory:20\n\
name:shortage_mcu_power,device:microphone-10000,mcu:10,memory:200000\n\
name:init_access_denial,device:camera-10000.speaker-30000,mcu:9000,memory:200000\n\
name:max_concurrent_access,device:camera-10000.propeller-10000,mcu:100000,memory:200000\n\
";
/*
name:max_concurrent1,device:camera-10000,mcu:9000,memory:200000\n\
name:max_concurrent2,device:microphone-10000,mcu:9000,memory:200000\n\
name:max_concurrent3,device:microphone-10000.door_motor-10000,mcu:9000,memory:500000\n\
name:max_concurrent4,device:microphone-10000.propeller-10000,mcu:9000,memory:500000\n\
name:low_pow,device:imu-500.camera-1000.door_motor-900,mcu:5000,memory:300000\n\
name:low_mcu,device:imu-10000,mcu:200,memory:500000\n\
name:low_memory,device:imu-10000,mcu:10000,memory:10\n\
";
*/

char* sensor_index_mapping[] = {"imu",
      "camera", "motion",
      "microphone", "speaker",
      "door_motor", "propeller",
      "home_camera_control", "home_camera_image",
      "door_battery"};
uint32 sensor_index_mapping_len = 10;

static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL)
        snprintf(error_buf, error_buf_size, "%s", string);
}

WASMModule*
wasm_load(const uint8 *buf, uint32 size,
          char *error_buf, uint32 error_buf_size)
{
    return wasm_loader_load(buf, size, error_buf, error_buf_size);
}

WASMModule*
wasm_load_from_sections(WASMSection *section_list,
                        char *error_buf, uint32_t error_buf_size)
{
    return wasm_loader_load_from_sections(section_list,
                                          error_buf, error_buf_size);
}

void
wasm_unload(WASMModule *module)
{
    wasm_loader_unload(module);
}

/**
 * Destroy memory instances.
 */
static void
memories_deinstantiate(WASMMemoryInstance **memories, uint32 count)
{
    uint32 i;
    if (memories) {
        for (i = 0; i < count; i++)
            if (memories[i]) {
                if (memories[i]->heap_handle)
                    mem_allocator_destroy(memories[i]->heap_handle);
                wasm_runtime_free(memories[i]);
            }
        wasm_runtime_free(memories);
  }
}


static bool
check_illegal_memory_boundary(WASMMemoryInstance* memory)
{
  int total_address = 0;
  char* tmp = device_spec;
  for(int j = 0 ; j < strlen(device_spec) ; j++) {
    if(device_spec[j] == '\n') ++total_address;
  }
  total_address -= 1;

  uint32 addresses[total_address];
  tmp = device_spec;
  int i = 0;

  while((tmp = strstr(tmp, "address:"))){
    tmp += 8;
    char address[11];
    memset(address, 0, 11);
    strncpy(address, tmp, 10);
    addresses[i++] = (uint32)strtoul(address, NULL, 16);
  }

  for(i = 0 ; i < total_address; i++){
    if((uint32)(&(*(memory->base_addr))) < addresses[i]
                && (uint32)(&(*(memory->end_addr))) > addresses[i])
    {
      return false;
    }
  }
  return true;
}

static WASMMemoryInstance*
memory_instantiate(uint32 num_bytes_per_page,
                   uint32 init_page_count, uint32 max_page_count,
                   uint32 global_data_size,
                   uint32 heap_size,
                   char *error_buf, uint32 error_buf_size)
{
    WASMMemoryInstance *memory;
    uint64 total_size = offsetof(WASMMemoryInstance, base_addr) +
                        (uint64)heap_size +
                        num_bytes_per_page * (uint64)init_page_count +
                        global_data_size;

    /* Allocate memory space, addr data and global data */
    if (total_size >= UINT32_MAX
        || !(memory = wasm_runtime_malloc((uint32)total_size))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate memory failed: allocate memory failed.");
        return NULL;
    }

    memset(memory, 0, (uint32)total_size);

    memory->total_size = (uint32)total_size;

    // Check error buffer. Then reallocate the memory.
    if(!check_illegal_memory_boundary(memory) &&
      (memory = aerogel_wasm_safe_allocation(memory->base_addr, total_size))
    ) {
      set_error_buf(error_buf, error_buf_size,
        "memory address illegally accesses the sensors and actuators. fml!\n");
      wasm_runtime_free(memory);
      return NULL;
    }

    memory->num_bytes_per_page = num_bytes_per_page;
    memory->cur_page_count = init_page_count;
    memory->max_page_count = max_page_count;

    memory->heap_data = memory->base_addr;
    memory->memory_data = memory->heap_data + heap_size;
    memory->global_data = memory->memory_data +
                          num_bytes_per_page * memory->cur_page_count;
    memory->global_data_size = global_data_size;
    memory->end_addr = memory->global_data + global_data_size;

    bh_assert(memory->end_addr - (uint8*)memory == (uint32)total_size);

    /* Initialize heap */
    if (!(memory->heap_handle = mem_allocator_create
                (memory->heap_data, heap_size))) {
        wasm_runtime_free(memory);
        return NULL;
    }

#if WASM_ENABLE_SPEC_TEST == 0
    memory->heap_base_offset = -(int32)heap_size;
#else
    memory->heap_base_offset = 0;
#endif

    return memory;
}

/**
 * Instantiate memories in a module.
 */
static WASMMemoryInstance**
memories_instantiate(const WASMModule *module,
                     uint32 global_data_size, uint32 heap_size,
                     char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 mem_index = 0, i, memory_count =
        module->import_memory_count + module->memory_count;
    uint64 total_size;
    WASMMemoryInstance **memories, *memory;

    if (memory_count == 0 && global_data_size > 0)
        memory_count = 1;

    total_size = sizeof(WASMMemoryInstance*) * (uint64)memory_count;

    if (total_size >= UINT32_MAX
        || !(memories = wasm_runtime_malloc((uint32)total_size))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate memory failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(memories, 0, (uint32)total_size);

    /* instantiate memories from import section */
    import = module->import_memories;
    for (i = 0; i < module->import_memory_count; i++, import++) {
        if (!(memory = memories[mem_index++] =
                    memory_instantiate(import->u.memory.num_bytes_per_page,
                                       import->u.memory.init_page_count,
                                       import->u.memory.max_page_count,
                                       global_data_size,
                                       heap_size, error_buf, error_buf_size))) {
            set_error_buf(error_buf, error_buf_size,
                         "Instantiate memory failed: "
                         "allocate memory failed.");
            memories_deinstantiate(memories, memory_count);
            return NULL;
        }
    }

    /* instantiate memories from memory section */
    for (i = 0; i < module->memory_count; i++) {
        if (!(memory = memories[mem_index++] =
                    memory_instantiate(module->memories[i].num_bytes_per_page,
                                       module->memories[i].init_page_count,
                                       module->memories[i].max_page_count,
                                       global_data_size,
                                       heap_size, error_buf, error_buf_size))) {
            set_error_buf(error_buf, error_buf_size,
                          "Instantiate memory failed: "
                          "allocate memory failed.");
            memories_deinstantiate(memories, memory_count);
            return NULL;
        }
    }

    if (mem_index == 0) {
        /* no import memory and define memory, but has global variables */
        if (!(memory = memories[mem_index++] =
                    memory_instantiate(0, 0, 0, global_data_size,
                                       heap_size, error_buf, error_buf_size))) {
            set_error_buf(error_buf, error_buf_size,
                          "Instantiate memory failed: "
                          "allocate memory failed.\n");
            memories_deinstantiate(memories, memory_count);
            return NULL;
        }
    }

    bh_assert(mem_index == memory_count);
    return memories;
}

/**
 * Destroy table instances.
 */
static void
tables_deinstantiate(WASMTableInstance **tables, uint32 count)
{
    uint32 i;
    if (tables) {
        for (i = 0; i < count; i++)
            if (tables[i])
                wasm_runtime_free(tables[i]);
        wasm_runtime_free(tables);
    }
}

/**
 * Instantiate tables in a module.
 */
static WASMTableInstance**
tables_instantiate(const WASMModule *module,
                   char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 table_index = 0, i, table_count =
        module->import_table_count + module->table_count;
    uint64 total_size = sizeof(WASMTableInstance*) * (uint64)table_count;
    WASMTableInstance **tables, *table;

    if (total_size >= UINT32_MAX
        || !(tables = wasm_runtime_malloc((uint32)total_size))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate table failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(tables, 0, (uint32)total_size);

    /* instantiate tables from import section */
    import = module->import_tables;
    for (i = 0; i < module->import_table_count; i++, import++) {
        total_size = offsetof(WASMTableInstance, base_addr) +
                     sizeof(uint32) * (uint64)import->u.table.init_size;
        if (total_size >= UINT32_MAX
            || !(table = tables[table_index++] =
                        wasm_runtime_malloc((uint32)total_size))) {
            set_error_buf(error_buf, error_buf_size,
                          "Instantiate table failed: "
                          "allocate memory failed.");
            tables_deinstantiate(tables, table_count);
            return NULL;
        }

        /* Set all elements to -1 to mark them as uninitialized elements */
        memset(table, -1, (uint32)total_size);
        table->total_size = (uint32)total_size;
        table->elem_type = import->u.table.elem_type;
        table->cur_size = import->u.table.init_size;
        table->max_size = import->u.table.max_size;
    }

    /* instantiate tables from table section */
    for (i = 0; i < module->table_count; i++) {
        total_size = offsetof(WASMTableInstance, base_addr) +
                     sizeof(uint32) * (uint64)module->tables[i].init_size;
        if (total_size >= UINT32_MAX
            || !(table = tables[table_index++] =
                        wasm_runtime_malloc((uint32)total_size))) {
            set_error_buf(error_buf, error_buf_size,
                          "Instantiate table failed: "
                          "allocate memory failed.");
            tables_deinstantiate(tables, table_count);
            return NULL;
        }

        /* Set all elements to -1 to mark them as uninitialized elements */
        memset(table, -1, (uint32)total_size);
        table->total_size = (uint32)total_size;
        table->elem_type = module->tables[i].elem_type;
        table->cur_size = module->tables[i].init_size;
        table->max_size = module->tables[i].max_size;
    }

    bh_assert(table_index == table_count);
    return tables;
}

/**
 * Destroy function instances.
 */
static void
functions_deinstantiate(WASMFunctionInstance *functions, uint32 count)
{
    if (functions) {
        wasm_runtime_free(functions);
    }
}

/**
 * Instantiate functions in a module.
 */
static WASMFunctionInstance*
functions_instantiate(const WASMModule *module,
                      char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 i, function_count =
        module->import_function_count + module->function_count;
    uint64 total_size = sizeof(WASMFunctionInstance) * (uint64)function_count;
    WASMFunctionInstance *functions, *function;

    if (total_size >= UINT32_MAX
        || !(functions = wasm_runtime_malloc((uint32)total_size))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate function failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(functions, 0, (uint32)total_size);

    /* instantiate functions from import section */
    function = functions;
    import = module->import_functions;
    for (i = 0; i < module->import_function_count; i++, import++) {
        function->is_import_func = true;
        function->u.func_import = &import->u.function;

        function->param_cell_num =
            wasm_type_param_cell_num(import->u.function.func_type);
        function->ret_cell_num =
            wasm_type_return_cell_num(import->u.function.func_type);
        function->local_cell_num = 0;

        function->param_count =
            (uint16)function->u.func_import->func_type->param_count;
        function->local_count = 0;
        function->param_types = function->u.func_import->func_type->types;
        function->local_types = NULL;

        function++;
    }

    /* instantiate functions from function section */
    for (i = 0; i < module->function_count; i++) {
        function->is_import_func = false;
        function->u.func = module->functions[i];

        function->param_cell_num = function->u.func->param_cell_num;
        function->ret_cell_num = function->u.func->ret_cell_num;
        function->local_cell_num = function->u.func->local_cell_num;

        function->param_count = (uint16)function->u.func->func_type->param_count;
        function->local_count = (uint16)function->u.func->local_count;
        function->param_types = function->u.func->func_type->types;
        function->local_types = function->u.func->local_types;

        function->local_offsets = function->u.func->local_offsets;

#if WASM_ENABLE_FAST_INTERP != 0
        function->const_cell_num = function->u.func->const_cell_num;
#endif

        function++;
    }

    bh_assert((uint32)(function - functions) == function_count);
    return functions;
}

/**
 * Destroy global instances.
 */
static void
globals_deinstantiate(WASMGlobalInstance *globals)
{
    if (globals)
        wasm_runtime_free(globals);
}

/**
 * Instantiate globals in a module.
 */
static WASMGlobalInstance*
globals_instantiate(const WASMModule *module,
                    uint32 *p_global_data_size,
                    char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 global_data_offset = 0;
    uint32 i, global_count =
        module->import_global_count + module->global_count;
    uint64 total_size = sizeof(WASMGlobalInstance) * (uint64)global_count;
    WASMGlobalInstance *globals, *global;

    if (total_size >= UINT32_MAX
        || !(globals = wasm_runtime_malloc((uint32)total_size))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate global failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(globals, 0, (uint32)total_size);

    /* instantiate globals from import section */
    global = globals;
    import = module->import_globals;
    for (i = 0; i < module->import_global_count; i++, import++) {
        WASMGlobalImport *global_import = &import->u.global;
        global->type = global_import->type;
        global->is_mutable = global_import->is_mutable;
        global->initial_value = global_import->global_data_linked;
        global->data_offset = global_data_offset;
        global_data_offset += wasm_value_type_size(global->type);

        global++;
    }

    /* instantiate globals from global section */
    for (i = 0; i < module->global_count; i++) {
        global->type = module->globals[i].type;
        global->is_mutable = module->globals[i].is_mutable;

        global->data_offset = global_data_offset;
        global_data_offset += wasm_value_type_size(global->type);

        global++;
    }

    bh_assert((uint32)(global - globals) == global_count);
    *p_global_data_size = global_data_offset;
    return globals;
}

static void
globals_instantiate_fix(WASMGlobalInstance *globals,
                        const WASMModule *module,
                        WASMModuleInstance *module_inst)
{
    WASMGlobalInstance *global = globals;
    WASMImport *import = module->import_globals;
    uint32 i;

    /* Fix globals from import section */
    for (i = 0; i < module->import_global_count; i++, import++, global++) {
        if (!strcmp(import->u.names.module_name, "env")) {
            if (!strcmp(import->u.names.field_name, "memoryBase")
                || !strcmp(import->u.names.field_name, "__memory_base")) {
                global->initial_value.addr = 0;
            }
            else if (!strcmp(import->u.names.field_name, "tableBase")
                     || !strcmp(import->u.names.field_name, "__table_base")) {
                global->initial_value.addr = 0;
            }
            else if (!strcmp(import->u.names.field_name, "DYNAMICTOP_PTR")) {
                global->initial_value.i32 = (int32)
                    (module_inst->default_memory->num_bytes_per_page
                     * module_inst->default_memory->cur_page_count);
                module_inst->DYNAMICTOP_PTR_offset = global->data_offset;
            }
            else if (!strcmp(import->u.names.field_name, "STACKTOP")) {
                global->initial_value.i32 = 0;
            }
            else if (!strcmp(import->u.names.field_name, "STACK_MAX")) {
                /* Unused in emcc wasm bin actually. */
                global->initial_value.i32 = 0;
            }
        }
    }

    for (i = 0; i < module->global_count; i++) {
        InitializerExpression *init_expr = &module->globals[i].init_expr;

        if (init_expr->init_expr_type == INIT_EXPR_TYPE_GET_GLOBAL) {
            bh_assert(init_expr->u.global_index < module->import_global_count);
            global->initial_value = globals[init_expr->u.global_index].initial_value;
        }
        else {
            bh_memcpy_s(&global->initial_value, sizeof(WASMValue),
                        &init_expr->u, sizeof(init_expr->u));
        }
        global++;
    }
}

/**
 * Return export function count in module export section.
 */
static uint32
get_export_function_count(const WASMModule *module)
{
    WASMExport *export = module->exports;
    uint32 count = 0, i;

    for (i = 0; i < module->export_count; i++, export++)
        if (export->kind == EXPORT_KIND_FUNC)
            count++;

    return count;
}

/**
 * Destroy export function instances.
 */
static void
export_functions_deinstantiate(WASMExportFuncInstance *functions)
{
    if (functions)
        wasm_runtime_free(functions);
}

/**
 * Instantiate export functions in a module.
 */
static WASMExportFuncInstance*
export_functions_instantiate(const WASMModule *module,
                             WASMModuleInstance *module_inst,
                             uint32 export_func_count,
                             char *error_buf, uint32 error_buf_size)
{
    WASMExportFuncInstance *export_funcs, *export_func;
    WASMExport *export = module->exports;
    uint32 i;
    uint64 total_size = sizeof(WASMExportFuncInstance) * (uint64)export_func_count;

    if (total_size >= UINT32_MAX
        || !(export_func = export_funcs = wasm_runtime_malloc((uint32)total_size))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate export function failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(export_funcs, 0, (uint32)total_size);

    for (i = 0; i < module->export_count; i++, export++)
        if (export->kind == EXPORT_KIND_FUNC) {
            export_func->name = export->name;
            export_func->function = &module_inst->functions[export->index];
            export_func++;
        }

    bh_assert((uint32)(export_func - export_funcs) == export_func_count);
    return export_funcs;
}

static bool
execute_post_inst_function(WASMModuleInstance *module_inst)
{
    WASMFunctionInstance *post_inst_func = NULL;
    WASMType *post_inst_func_type;
    uint32 i;

    for (i = 0; i < module_inst->export_func_count; i++)
        if (!strcmp(module_inst->export_functions[i].name, "__post_instantiate")) {
            post_inst_func = module_inst->export_functions[i].function;
            break;
        }

    if (!post_inst_func)
        /* Not found */
        return true;

    post_inst_func_type = post_inst_func->u.func->func_type;
    if (post_inst_func_type->param_count != 0
        || post_inst_func_type->result_count != 0)
        /* Not a valid function type, ignore it */
        return true;

    return wasm_create_exec_env_and_call_function(module_inst, post_inst_func,
                                                  0, NULL);
}

static bool
execute_start_function(WASMModuleInstance *module_inst)
{
    WASMFunctionInstance *func = module_inst->start_function;

    if (!func)
        return true;

    bh_assert(!func->is_import_func && func->param_cell_num == 0
              && func->ret_cell_num == 0);

    return wasm_create_exec_env_and_call_function(module_inst, func, 0, NULL);
}

/*
  Initialize the sensor state information.
*/
void
init_sensor_access(void)
{
  int size = strlen(device_spec) - 1;
  int i = 0;
  int j = 0;
  int k = 0;
  char tmp[150];
  char* temp;
  for(; i < size ; i ++){
    if(device_spec[i] == '\n'){
      ++num_sensor_actuator_concurrent_access;
    }
  }

  for(i = 0; i < size ; i ++){
    if(device_spec[i] == '\n'){
      j = 0;
      if(!(temp = strstr(tmp, "concurrent_access:"))) {
        break;
      }

      temp += strlen("concurrent_access:");
      sensor_actuator_concurrent_access[k++] = 0;
      memset(tmp, 0, 150);
    }
    else {
      tmp[j++] = device_spec[i];
    }
  }
}

/**
 * Parse the sensor actuator information
 */
SensorActuatorInfo*
parse_sensor_actuator_info(char* name, uint32 allowed_power_consumption)
{
  SensorActuatorInfo* sensor_actuator_info = wasm_runtime_malloc(sizeof(SensorActuatorInfo));
  char* tmp;
  if((tmp=strstr(device_spec, name))) {
    // Get id
    tmp = strstr(tmp, "id:");
    sensor_actuator_info->id = tmp[3]-'0';

    // Get mmio address
    tmp = strstr(tmp, "address:");
    tmp += 8;
    char address[11];
    memset(address, 0, 11);
    strncpy(address, tmp, 10);
    sensor_actuator_info->mmio_address = (uint32)strtoul(address, NULL, 16);

    // Get power
    tmp = strstr(tmp, "power:");
    tmp += 6;
    char power[4];
    memset(power, 0, 4);
    for(int i = 0; i < 4; i++) {
      if(tmp[i] == ',') {
        break;
      }
      power[i] = tmp[i];
    }
    sensor_actuator_info->power = atoi(power);

    //Get concurrent_access
    tmp = strstr(tmp, "concurrent_access:");
    tmp += strlen("concurrent_access:");
    char concurrent[3];
    memset(concurrent, 0, 3);
    for(int i = 0; i < 3; i++) {
      if(tmp[i] == '\n') {
        break;
      }
      concurrent[i] = tmp[i];
    }
    sensor_actuator_info->num_concurrent_access = atoi(concurrent);

    // Get max power consumption.
    sensor_actuator_info->allowed_power_consumption = allowed_power_consumption;
    sensor_actuator_info->used_power = 0;
  }
  else {
    printf("bad name\n");
    return NULL;
  }
  return sensor_actuator_info;
}

SensorActuatorInfo**
parse_sensor_actuator_info_list(char* list)
{
  SensorActuatorInfo** sensor_actuator_info_list =
          wasm_runtime_malloc(sizeof(SensorActuatorInfo*)*10);
  int i = 0;
  int num = 0;
  char name[20];
  char number[10];
  int j = 0;
  memset(name, 0, 20);
  memset(number, 0, 10);

  while(*list) {
    if(!num) {
      if(*list != '-'){
        name[j++] = *list;
      }
      else {
        j = 0;
        num = 1;
        memset(number, 0, 10);
      }
    } else {
      if(*list == '.') {
        j = 0;
        num = 0;
        sensor_actuator_info_list[i++] = parse_sensor_actuator_info(name, (uint32)atoi(number));
        memset(name, 0, 20);
      }
      else {
        number[j++] = *list;
      }
    }
    ++list;
  }
  sensor_actuator_info_list[i++] = parse_sensor_actuator_info(name, (uint32)atoi(number));
  return sensor_actuator_info_list;
}

/*
 * Parse the access control module information.
 */
AccessControlModule**
parse_access_control_module(void)
{
  AccessControlModule** access_control_module_list = wasm_runtime_malloc(sizeof(AccessControlModule*) * 8);
  char* tmp = module_spec;
  int j = 0;
  while((tmp = strstr(tmp, "name:"))){
    tmp += 5;
    AccessControlModule* access_control_module = wasm_runtime_malloc(sizeof(AccessControlModule));
    // Parse name
    char* name = wasm_runtime_malloc(20);
    memset(name, 0, 20);
    for(int i = 0; i < 20; i++) {
      if(tmp[i] == ',') {
        break;
      }
      name[i] = tmp[i];
    }
    access_control_module->name = name;

    // Parse device
    char* device_list = wasm_runtime_malloc(100);
    memset(device_list, 0, 100);
    tmp = strstr(tmp, "device:");
    tmp += strlen("device:");
    for(int i = 0; i < 100; i++) {
      if(tmp[i] == ',') {
        break;
      }
      device_list[i] = tmp[i];
    }

    int num_authorized_sensor_actuator = 1;
    for(int i = 0; i < 100; i++) {
      if(!device_list[i]) break;
      if(device_list[i] == '.') ++num_authorized_sensor_actuator;
    }

    SensorActuatorInfo** authorized_sensor_actuator = parse_sensor_actuator_info_list(device_list);
    access_control_module->authorized_sensor_actuator = authorized_sensor_actuator;
    access_control_module->num_authorized_sensor_actuator = num_authorized_sensor_actuator;

    //Parse processor power
    tmp = strstr(tmp, "mcu:");
    tmp += strlen("mcu:");
    char mcu_power[8];
    memset(mcu_power, 0, 8);
    for(int i = 0; i < 8; i++) {
      if(tmp[i] == ',') {
        break;
      }
      mcu_power[i] = tmp[i];
    }
    access_control_module->processor_power_consumption = atoi(mcu_power);

    //Parse memory consumption
    tmp = strstr(tmp, "memory:");
    tmp += strlen("memory:");
    char memory_consumption[16];
    memset(memory_consumption, 0, 16);
    for(int i = 0; i < 16; i++) {
      if(tmp[i] == '\n') break;
      memory_consumption[i] = tmp[i];
    }
    access_control_module->memory_consumption = atoi(memory_consumption);
    access_control_module_list[j++] = access_control_module;
  }
  return access_control_module_list;
}

/**
 * Parse the access control spec sheet.
 */
AccessControl*
parse_access_control_spec(void)
{
  AccessControl* access_control = wasm_runtime_malloc(sizeof(AccessControl));
  memset(access_control, 0, sizeof(AccessControl));

  if(num_sensor_actuator_concurrent_access == 0) {
    init_sensor_access();
  }
  access_control->module_info = parse_access_control_module();

  // Get number of module info
  int num_module_info = 0;
  for(int i = 0; i < strlen(module_spec) + 1; i++) {
    if(module_spec[i] == '\n') {
      ++num_module_info;
    }
  }
  access_control->num_module_info = num_module_info;

  // Parse process mcu power info
  char* tmp = device_spec;
  tmp = strstr(tmp, "mcu,power:");
  tmp += strlen("mcu,power:");
  char mcu_power[8];
  memset(mcu_power, 0, 8);
  for(int i = 0; i < 8; i++) {
    if(tmp[i] == '\n') break;
    mcu_power[i] = tmp[i];
  }

  access_control->processor_power = atoi(mcu_power);
  return access_control;
}

// Debugging purpose.
void print_access_control(AccessControl* access_control) {
  printf("Processor power: %u\n", access_control->processor_power);
  AccessControlModule** module_info = access_control->module_info;
  printf("num module info: %u\n",access_control->num_module_info);
  for(int i = 0; i < access_control->num_module_info; i++){
    printf("name: %s\n", module_info[i]->name);
    printf("processor consumption: %u\n", module_info[i]->processor_power_consumption);
    // printf("used processor consumption: %u\n", module_info[i]->used_processor_power);
    printf("memory consumption: %u\n", module_info[i]->memory_consumption);
    // printf("used memory: %u\n", module_info[i]->used_memory);
    printf("Authorized sensor Info\n");
    for(int j = 0; j < module_info[i]->num_authorized_sensor_actuator; j++){
      SensorActuatorInfo* authorized = (module_info[i]->authorized_sensor_actuator)[j];
      printf("\tid: %u\n", authorized->id);
      printf("\tmmio: %x\n", authorized->mmio_address);
      printf("\tnum_concurrent_access: %u\n", authorized->num_concurrent_access);
      printf("\tpower: %u\n", authorized->power);
      printf("\tallowed_power_consumption: %u\n", authorized->allowed_power_consumption);
      printf("\tused_power: %u\n", authorized->used_power);
    }
    printf("\n\n");
  }
}

// RL: Check whether the memory has violated the used-defined access control rules
bool
check_memory_usage(WASMModuleInstance* module_inst)
{
  uint32 total_memory = 0;
  WASMMemoryInstance** memory_inst = module_inst -> memories;
  WASMTableInstance** table_inst = module_inst -> tables;

  for(int i = 0; i < module_inst->memory_count; i++)
  {
    total_memory += memory_inst[i]->total_size;
  }

  for(int i = 0; i < module_inst->table_count; i++)
  {
    total_memory += table_inst[i]->total_size;
  }

  uint32 module_index = module_inst->access_control->module_index;
  module_inst->memory_usage_bytes = total_memory;
  uint32 specified_memory = module_inst->access_control->module_info[module_index]->memory_consumption;
  return module_inst->memory_usage_bytes <= specified_memory;
}

// add the index of the module from its name
void wasm_add_index_from_name(WASMModuleInstance* module_inst, char* name)
{
  char* tmp = module_spec;
  char* start_tmp = module_spec; // at the start of module_spec, used to count '\n'
  uint32 module_index = 0;
  tmp = strstr(tmp, name);

  while(start_tmp != tmp) {
    if(*start_tmp == '\n') {
      ++module_index;
    }
    ++start_tmp;
  }

  module_inst->access_control->module_index = module_index;
  module_inst->name = wasm_runtime_malloc(strlen(name) + 1);
  strcpy(module_inst->name, name);
  // printf("Calling here name: %s, index: %u\n", module_inst->name, module_inst->access_control->module_index);
}

/**
 * Instantiate module
 */
WASMModuleInstance*
wasm_instantiate(WASMModule *module,
                 uint32 stack_size, uint32 heap_size,
                 char *error_buf, uint32 error_buf_size)
{
    WASMModuleInstance *module_inst;
    WASMTableSeg *table_seg;
    WASMDataSeg *data_seg;
    WASMGlobalInstance *globals = NULL, *global;
    uint32 global_count, global_data_size = 0, i, j;
    uint32 base_offset, length, memory_size;
    uint8 *global_data, *global_data_end;
    uint8 *memory_data;
    uint32 *table_data;
    AccessControl* access_control;

    if (!module)
        return NULL;

    /* Check heap size */
    heap_size = align_uint(heap_size, 8);
    if (heap_size == 0)
        heap_size = APP_HEAP_SIZE_DEFAULT;
    if (heap_size < APP_HEAP_SIZE_MIN)
        heap_size = APP_HEAP_SIZE_MIN;
    if (heap_size > APP_HEAP_SIZE_MAX)
        heap_size = APP_HEAP_SIZE_MAX;

    /* Instantiate global firstly to get the mutable data size */
    global_count = module->import_global_count + module->global_count;
    if (global_count &&
        !(globals = globals_instantiate(module,
                                        &global_data_size,
                                        error_buf, error_buf_size)))
        return NULL;
    //(RENJU: Parse the spec sheet for the access control.)
    if(!(access_control = parse_access_control_spec())){
      set_error_buf(error_buf, error_buf_size, "Access control parsing fail.");
      globals_deinstantiate(globals);
      return NULL;
    }

    /* Allocate the memory */
    if (!(module_inst = wasm_runtime_malloc((uint32)sizeof(WASMModuleInstance)))) {
        set_error_buf(error_buf, error_buf_size,
                      "Instantiate module failed: allocate memory failed.");
        globals_deinstantiate(globals);
        return NULL;
    }

    memset(module_inst, 0, (uint32)sizeof(WASMModuleInstance));
    module_inst->access_control = access_control;
    module_inst->global_count = global_count;
    module_inst->globals = globals;

    module_inst->memory_count =
        module->import_memory_count + module->memory_count;
    module_inst->table_count =
        module->import_table_count + module->table_count;
    module_inst->function_count =
        module->import_function_count + module->function_count;
    module_inst->export_func_count = get_export_function_count(module);

    /* Instantiate memories/tables/functions */
    if (((module_inst->memory_count > 0 || global_count > 0)
         && !(module_inst->memories =
             memories_instantiate(module, global_data_size,
                                  heap_size, error_buf, error_buf_size)))
        || (module_inst->table_count > 0
            && !(module_inst->tables = tables_instantiate(module,
                                                          error_buf,
                                                          error_buf_size)))
        || (module_inst->function_count > 0
            && !(module_inst->functions = functions_instantiate(module,
                                                                error_buf,
                                                                error_buf_size)))
        || (module_inst->export_func_count > 0
            && !(module_inst->export_functions = export_functions_instantiate(
                    module, module_inst, module_inst->export_func_count,
                    error_buf, error_buf_size)))) {
        wasm_deinstantiate(module_inst);
        return NULL;
    }

    // Make sure the memory has not violated the rule.
    if (!check_memory_usage(module_inst)) {
      set_error_buf(error_buf, error_buf_size,
                    "Memory usage exceeds.");
      wasm_deinstantiate(module_inst);
      return NULL;
    }

    if (module_inst->memory_count || global_count > 0) {
        WASMMemoryInstance *memory;

        memory = module_inst->default_memory = module_inst->memories[0];
        memory_data = module_inst->default_memory->memory_data;

        /* fix import memoryBase */
        globals_instantiate_fix(globals, module, module_inst);

        /* Initialize the global data */
        global_data = memory->global_data;
        global_data_end = global_data + global_data_size;
        global = globals;
        for (i = 0; i < global_count; i++, global++) {
            switch (global->type) {
                case VALUE_TYPE_I32:
                case VALUE_TYPE_F32:
                    *(int32*)global_data = global->initial_value.i32;
                    global_data += sizeof(int32);
                    break;
                case VALUE_TYPE_I64:
                case VALUE_TYPE_F64:
                    bh_memcpy_s(global_data, (uint32)(global_data_end - global_data),
                                &global->initial_value.i64, sizeof(int64));
                    global_data += sizeof(int64);
                    break;
                default:
                    bh_assert(0);
            }
        }
        bh_assert(global_data == global_data_end);

        /* Initialize the memory data with data segment section */
        if (module_inst->default_memory->cur_page_count > 0) {
            for (i = 0; i < module->data_seg_count; i++) {
                data_seg = module->data_segments[i];
                bh_assert(data_seg->memory_index == 0);
                bh_assert(data_seg->base_offset.init_expr_type ==
                            INIT_EXPR_TYPE_I32_CONST
                          || data_seg->base_offset.init_expr_type ==
                            INIT_EXPR_TYPE_GET_GLOBAL);

                if (data_seg->base_offset.init_expr_type == INIT_EXPR_TYPE_GET_GLOBAL) {
                    bh_assert(data_seg->base_offset.u.global_index < global_count
                              && globals[data_seg->base_offset.u.global_index].type ==
                                    VALUE_TYPE_I32);
                    data_seg->base_offset.u.i32 =
                        globals[data_seg->base_offset.u.global_index].initial_value.i32;
                }

                base_offset = (uint32)data_seg->base_offset.u.i32;
                length = data_seg->data_length;
                memory_size = module_inst->default_memory->num_bytes_per_page
                              * module_inst->default_memory->cur_page_count;

                if (length > 0
                    && (base_offset >= memory_size
                        || base_offset + length > memory_size)) {
                    set_error_buf(error_buf, error_buf_size,
                            "Instantiate module failed: data segment out of range.");
                    wasm_deinstantiate(module_inst);
                    return NULL;
                }

                bh_memcpy_s(memory_data + base_offset, memory_size - base_offset,
                            data_seg->data, length);
            }
        }
    }

    if (module_inst->table_count) {
        module_inst->default_table = module_inst->tables[0];

        /* Initialize the table data with table segment section */
        table_data = (uint32*)module_inst->default_table->base_addr;
        table_seg = module->table_segments;
        for (i = 0; i < module->table_seg_count; i++, table_seg++) {
            bh_assert(table_seg->table_index == 0);
            bh_assert(table_seg->base_offset.init_expr_type ==
                        INIT_EXPR_TYPE_I32_CONST
                      || table_seg->base_offset.init_expr_type ==
                        INIT_EXPR_TYPE_GET_GLOBAL);

            if (table_seg->base_offset.init_expr_type ==
                    INIT_EXPR_TYPE_GET_GLOBAL) {
                bh_assert(table_seg->base_offset.u.global_index < global_count
                          && globals[table_seg->base_offset.u.global_index].type ==
                                VALUE_TYPE_I32);
                table_seg->base_offset.u.i32 =
                    globals[table_seg->base_offset.u.global_index].initial_value.i32;
            }
            if ((uint32)table_seg->base_offset.u.i32 <
                    module_inst->default_table->cur_size) {
                length = table_seg->function_count;
                if ((uint32)table_seg->base_offset.u.i32 + length >
                        module_inst->default_table->cur_size)
                    length = module_inst->default_table->cur_size
                             - (uint32)table_seg->base_offset.u.i32;
                /* Check function index */
                for (j = 0; j < length; j++) {
                    if (table_seg->func_indexes[j] >= module_inst->function_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "function index is overflow");
                        wasm_deinstantiate(module_inst);
                        return NULL;
                    }
                }
                bh_memcpy_s(table_data + table_seg->base_offset.u.i32,
                            (uint32)((module_inst->default_table->cur_size
                                      - (uint32)table_seg->base_offset.u.i32)
                                     * sizeof(uint32)),
                            table_seg->func_indexes, (uint32)(length * sizeof(uint32)));
            }
        }
    }

#if WASM_ENABLE_LIBC_WASI != 0
    if (!wasm_runtime_init_wasi((WASMModuleInstanceCommon*)module_inst,
                                module->wasi_args.dir_list,
                                module->wasi_args.dir_count,
                                module->wasi_args.map_dir_list,
                                module->wasi_args.map_dir_count,
                                module->wasi_args.env,
                                module->wasi_args.env_count,
                                module->wasi_args.argv,
                                module->wasi_args.argc,
                                error_buf, error_buf_size)) {
        wasm_deinstantiate(module_inst);
        return NULL;
    }
#endif

    if (module->start_function != (uint32)-1) {
        bh_assert(module->start_function >= module->import_function_count);
        module_inst->start_function =
            &module_inst->functions[module->start_function];
    }

    module_inst->module = module;

    /* module instance type */
    module_inst->module_type = Wasm_Module_Bytecode;

    /* Initialize the thread related data */
    if (stack_size == 0)
        stack_size = DEFAULT_WASM_STACK_SIZE;
#if WASM_ENABLE_SPEC_TEST != 0
    if (stack_size < 48 *1024)
        stack_size = 48 * 1024;
#endif
    module_inst->default_wasm_stack_size = stack_size;

    /* Execute __post_instantiate function */
    if (!execute_post_inst_function(module_inst)
        || !execute_start_function(module_inst)) {
        set_error_buf(error_buf, error_buf_size,
                      module_inst->cur_exception);
        wasm_deinstantiate(module_inst);
        return NULL;
    }

    (void)global_data_end;
    return module_inst;
}

void
wasm_deinstantiate(WASMModuleInstance *module_inst)
{
    if (!module_inst)
        return;

#if WASM_ENABLE_LIBC_WASI != 0
    /* Destroy wasi resource before freeing app heap, since some fields of
       wasi contex are allocated from app heap, and if app heap is freed,
       these fields will be set to NULL, we cannot free their internal data
       which may allocated from global heap. */
    wasm_runtime_destroy_wasi((WASMModuleInstanceCommon*)module_inst);
#endif

    if (module_inst->memory_count > 0)
        memories_deinstantiate(module_inst->memories, module_inst->memory_count);
    else if (module_inst->memories != NULL && module_inst->global_count > 0)
        /* No imported memory and defined memory, the memory is created when
           global count > 0. */
        memories_deinstantiate(module_inst->memories, 1);

    tables_deinstantiate(module_inst->tables, module_inst->table_count);
    functions_deinstantiate(module_inst->functions, module_inst->function_count);
    globals_deinstantiate(module_inst->globals);
    export_functions_deinstantiate(module_inst->export_functions);

    wasm_runtime_free(module_inst);
}

WASMFunctionInstance*
wasm_lookup_function(const WASMModuleInstance *module_inst,
                     const char *name, const char *signature)
{
    uint32 i;
    for (i = 0; i < module_inst->export_func_count; i++)
        if (!strcmp(module_inst->export_functions[i].name, name))
            return module_inst->export_functions[i].function;
    (void)signature;
    return NULL;
}

bool
wasm_call_function(WASMExecEnv *exec_env,
                   WASMFunctionInstance *function,
                   unsigned argc, uint32 argv[])
{
    WASMModuleInstance *module_inst = (WASMModuleInstance*)exec_env->module_inst;
    wasm_interp_call_wasm(module_inst, exec_env, function, argc, argv);
    return !wasm_get_exception(module_inst) ? true : false;
}

bool
wasm_create_exec_env_and_call_function(WASMModuleInstance *module_inst,
                                       WASMFunctionInstance *func,
                                       unsigned argc, uint32 argv[])
{
    WASMExecEnv *exec_env;
    bool ret;

    if (!(exec_env = wasm_exec_env_create((WASMModuleInstanceCommon*)module_inst,
                                          module_inst->default_wasm_stack_size))) {
        wasm_set_exception(module_inst, "allocate memory failed.");
        return false;
    }

    ret = wasm_call_function(exec_env, func, argc, argv);
    wasm_exec_env_destroy(exec_env);
    return ret;
}

void
wasm_set_exception(WASMModuleInstance *module_inst,
                   const char *exception)
{
    if (exception)
        snprintf(module_inst->cur_exception,
                 sizeof(module_inst->cur_exception),
                 "Exception: %s", exception);
    else
        module_inst->cur_exception[0] = '\0';
}

const char*
wasm_get_exception(WASMModuleInstance *module_inst)
{
    if (module_inst->cur_exception[0] == '\0')
        return NULL;
    else
        return module_inst->cur_exception;
}

int32
wasm_module_malloc(WASMModuleInstance *module_inst, uint32 size,
                   void **p_native_addr)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = mem_allocator_malloc(memory->heap_handle, size);
    if (!addr) {
        wasm_set_exception(module_inst, "out of memory");
        return 0;
    }
    if (p_native_addr)
        *p_native_addr = addr;
    return (int32)(addr - memory->memory_data);
}

void
wasm_module_free(WASMModuleInstance *module_inst, int32 ptr)
{
    if (ptr) {
        WASMMemoryInstance *memory = module_inst->default_memory;
        uint8 *addr = memory->memory_data + ptr;
        if (memory->heap_data < addr && addr < memory->memory_data)
            mem_allocator_free(memory->heap_handle, addr);
    }
}

int32
wasm_module_dup_data(WASMModuleInstance *module_inst,
                     const char *src, uint32 size)
{
    char *buffer;
    int32 buffer_offset = wasm_module_malloc(module_inst, size,
                                             (void**)&buffer);
    if (buffer_offset != 0) {
        buffer = wasm_addr_app_to_native(module_inst, buffer_offset);
        bh_memcpy_s(buffer, size, src, size);
    }
    return buffer_offset;
}

bool
wasm_validate_app_addr(WASMModuleInstance *module_inst,
                       int32 app_offset, uint32 size)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    /* integer overflow check */
    if (app_offset + (int32)size < app_offset) {
        goto fail;
    }

    if (app_offset <= memory->heap_base_offset
        || app_offset + (int32)size > memory_data_size) {
        goto fail;
    }
    return true;
fail:
    wasm_set_exception(module_inst, "out of bounds memory access");
    return false;
}

bool
wasm_validate_native_addr(WASMModuleInstance *module_inst,
                          void *native_ptr, uint32 size)
{
    uint8 *addr = (uint8*)native_ptr;
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (addr + size < addr) {
        goto fail;
    }

    if (addr <= memory->heap_data
        || addr + size > memory->memory_data + memory_data_size) {
        goto fail;
    }
    return true;
fail:
    wasm_set_exception(module_inst, "out of bounds memory access");
    return false;
}

void *
wasm_addr_app_to_native(WASMModuleInstance *module_inst,
                        int32 app_offset)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_base_offset < app_offset
        && app_offset < memory_data_size)
        return memory->memory_data + app_offset;
    return NULL;
}

int32
wasm_addr_native_to_app(WASMModuleInstance *module_inst,
                        void *native_ptr)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = (uint8*)native_ptr;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_data < addr
        && addr < memory->memory_data + memory_data_size)
        return (int32)(addr - memory->memory_data);
    return 0;
}

bool
wasm_get_app_addr_range(WASMModuleInstance *module_inst,
                        int32 app_offset,
                        int32 *p_app_start_offset,
                        int32 *p_app_end_offset)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_base_offset < app_offset
        && app_offset < memory_data_size) {
        if (p_app_start_offset)
            *p_app_start_offset = memory->heap_base_offset;
        if (p_app_end_offset)
            *p_app_end_offset = memory_data_size;
        return true;
    }
    return false;
}

bool
wasm_get_native_addr_range(WASMModuleInstance *module_inst,
                           uint8 *native_ptr,
                           uint8 **p_native_start_addr,
                           uint8 **p_native_end_addr)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = (uint8*)native_ptr;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_data < addr
        && addr < memory->memory_data + memory_data_size) {
        if (p_native_start_addr)
            *p_native_start_addr = memory->heap_data;
        if (p_native_end_addr)
            *p_native_end_addr = memory->memory_data + memory_data_size;
        return true;
    }
    return false;
}

bool
wasm_enlarge_memory(WASMModuleInstance *module, uint32 inc_page_count)
{
    WASMMemoryInstance *memory = module->default_memory, *new_memory;
    uint32 heap_size = memory->memory_data - memory->heap_data;
    uint32 old_page_count = memory->cur_page_count;
    uint32 total_size_old = memory->end_addr - (uint8*)memory;
    uint32 total_page_count = inc_page_count + memory->cur_page_count;
    uint64 total_size = offsetof(WASMMemoryInstance, base_addr)
                        + (uint64)heap_size
                        + memory->num_bytes_per_page * (uint64)total_page_count
                        + memory->global_data_size;
    uint8 *global_data_old;
    void *heap_handle_old = memory->heap_handle;
    uint32 module_index = module->access_control->module_index;

    if (inc_page_count <= 0){
        /* No need to enlarge memory */
        return true;
    }

    //RL: Make sure the enlarged memory does not expand beyond the request.
    if((uint32)total_size > module->access_control->module_info[module_index]->memory_consumption){
      wasm_set_exception(module, "fail to enlarge memory because of access memory size.");
      return false;
    }

    if (total_page_count < memory->cur_page_count /* integer overflow */
        || total_page_count > memory->max_page_count) {
        wasm_set_exception(module, "fail to enlarge memory.");
        return false;
    }

    if (total_size >= UINT32_MAX) {
        wasm_set_exception(module, "fail to enlarge memory.");
        return false;
    }

    /* Destroy heap's lock firstly, if its memory is re-allocated,
       we cannot access its lock again. */
    mem_allocator_destroy_lock(memory->heap_handle);
    if (!(new_memory = wasm_runtime_realloc(memory, (uint32)total_size))) {
        if (!(new_memory = wasm_runtime_malloc((uint32)total_size))) {
            /* Restore heap's lock if memory re-alloc failed */
            mem_allocator_reinit_lock(memory->heap_handle);
            wasm_set_exception(module, "fail to enlarge memory.");
            return false;
        }
        bh_memcpy_s((uint8*)new_memory, (uint32)total_size,
                    (uint8*)memory, total_size_old);
        wasm_runtime_free(memory);
    }

    // Check error buffer. Then reallocate the memory.
    if(!check_illegal_memory_boundary(new_memory) &&
      (new_memory = aerogel_wasm_safe_allocation(new_memory->base_addr, total_size))
    ) {
      // set_error_buf(error_buf, error_buf_size,
      //   "Reallocate failed because memory address illegally accesses
      //     the sensors and actuators. fml!\n");
      mem_allocator_reinit_lock(memory->heap_handle);
      wasm_set_exception(module, "fail to enlarge memory due to illegal access \
        to sensors and actuators.");
      return false;
    }

    memset((uint8*)new_memory + total_size_old,
           0, (uint32)total_size - total_size_old);

    new_memory->total_size = (uint32)total_size;

    new_memory->heap_handle = (uint8*)heap_handle_old +
                              ((uint8*)new_memory - (uint8*)memory);
    if (mem_allocator_migrate(new_memory->heap_handle,
                              heap_handle_old) != 0) {
        wasm_set_exception(module, "fail to enlarge memory.");
        return false;
    }

    new_memory->cur_page_count = total_page_count;
    new_memory->heap_data = new_memory->base_addr;
    new_memory->memory_data = new_memory->base_addr + heap_size;
    new_memory->global_data = new_memory->memory_data +
                              new_memory->num_bytes_per_page * total_page_count;
    new_memory->end_addr = new_memory->global_data + new_memory->global_data_size;

    global_data_old = new_memory->memory_data +
                              new_memory->num_bytes_per_page * old_page_count;

    /* Copy global data */
    bh_memcpy_s(new_memory->global_data, new_memory->global_data_size,
                global_data_old, new_memory->global_data_size);
    memset(global_data_old, 0, new_memory->global_data_size);

    module->memories[0] = module->default_memory = new_memory;
    module->access_control->module_info[module_index]->memory_consumption = (uint32)total_size;
    return true;
}

void get_imu_sensor(uint32* return_val) {
  return_val[0] = get_renju_rand() % 100;
  return_val[1] = get_renju_rand() % 100;
  return_val[2] = get_renju_rand() % 100;
}

void get_door_battery_percentage(uint32* return_val) {
  return_val[0] = get_renju_rand() % 100;
}

// It returns 20*20 double array.
void get_camera_data(uint32** image) {
  for(int i = 0 ; i < 10 ; i++) {
    for(int j = 0; j < 10 ; j++) {
      image[i][j] = get_renju_rand() % 100;
    }
  }
}

void get_motion_data(uint32* direction) {
  direction[0] = get_renju_rand() % 4;
}

// Returns a single array
void get_microphone_data(uint32* mic_data) {
  for(int i = 0; i < 10 ; i++) {
    mic_data[i] = get_renju_rand() % 4;
  }
}

void set_speaker_data(uint32* speaker_data, uint32 latency)
{
  //output the speaker data to the device through driver.
  (void) &speaker_data;
  sleep_us(latency);
}

void set_door_motor(uint32* state, uint32 latency){
  //output to the door motor.
  (void) &state;
  sleep_us(latency);
}

void set_propeller(uint32* state, uint32 latency){
  // 4 propellers.
  (void)&(state[0]);
  (void)&(state[1]);
  (void)&(state[2]);
  (void)&(state[3]);
  sleep_us(latency);
}

void set_home_camera_control(uint32* state, uint32 latency){

  // angle
  (void) &state;
  sleep_us(latency);
}

void get_imu(uint32* data, uint32 freq)
{
  uint32 latency = 1000000/freq;
  get_imu_sensor(data);
  sleep_us(latency);
}

void get_door_battery(uint32* data, uint32 freq)
{
  uint32 latency = 1000000/freq;
  get_door_battery_percentage(data);
  sleep_us(latency);
}

void get_camera(uint32* data, uint32 freq)
{
  uint32 latency = 1000000/freq;
  uint32 **temp = wasm_runtime_malloc(sizeof(uint32*) * 10);
  for(int i = 0; i < 10; i++){
    temp[i] = wasm_runtime_malloc(sizeof(uint32) * 10);
  }
  get_camera_data(temp);
  for(int i = 0 ; i < 10;i++){
    for(int j = 0;j < 10;j++){
      data[i*10+j] = temp[i][j];
    }
  }
  sleep_us(latency);
}

void get_motion(uint32* data, uint32 freq)
{
  uint32 latency = 1000000/freq;
  get_motion_data(data);
  sleep_us(latency);
}

void get_microphone(uint32* data, uint32 freq)
{
  uint32 latency = 1000000/freq;
  get_microphone_data(data);
  sleep_us(latency);
}

bool check_access_name(char* module_name,
    char* sensor_name)
{
  char* tmp = module_spec;
  if(!(tmp = strstr(module_spec, module_name))) {
    printf("Name not found. No access to any sensors.\n");
    return true;
  }
  char access_string[200];
  int j = 0;
  while(*tmp != '\n' && *tmp) {
    access_string[j++] = *tmp;
    ++tmp;
  }
  if(strstr(access_string, sensor_name)){
    return false;
  }
  else return true;
}

bool check_access_concurrency(char* sensor_name, uint32 max_concurrent)
{
  int i = 0;
  // printf("name: %s\n", sensor_name);
  for(; i < sensor_index_mapping_len; i++){
    if(strstr(sensor_index_mapping[i], sensor_name))
    {
      // printf("first: %u, second: %u\n", sensor_actuator_concurrent_access[i], max_concurrent);
      if(sensor_actuator_concurrent_access[i] < max_concurrent){
        // printf("i: %d, access: %u\n",i, sensor_actuator_concurrent_access[i]);
        sensor_actuator_concurrent_access[i]++;
        return false;
      }
      return true;
    }
  }
  return true;
}

// return true means should not execute.
bool check_access_energy(WASMModuleInstance* module_inst, char* peripheral_name)
{
  uint32 peripheral_id = -1;
  uint32 i = 0;
  for(; i < sensor_index_mapping_len; i++){
    if(strstr(sensor_index_mapping[i], peripheral_name))
    {
      peripheral_id = i;
      break;
    }
  }

  if(peripheral_id == -1) {
    printf("check_access_energy. Sensor: %s not found.\n", peripheral_name);
    return true;
  }

  uint32 ac_module_index = module_inst->access_control->module_index;
  AccessControlModule* access_control_module = module_inst->access_control->module_info[ac_module_index];
  for(i = 0 ; i < access_control_module->num_authorized_sensor_actuator; i++){
    if(access_control_module->authorized_sensor_actuator[i]->id == peripheral_id){
      SensorActuatorInfo* sensor_info = access_control_module->authorized_sensor_actuator[i];
      uint32 cur_time = bh_get_tick_us();
      if(cur_time - sensor_info->timestamp > 10000000) {
        sensor_info->timestamp = cur_time;
        sensor_info->used_power = 0;
        return false;
      }

      sensor_info->used_power += sensor_info->power * 1;
      if(sensor_info->allowed_power_consumption < sensor_info->used_power) {
        sensor_info->used_power = 0;
        return true;
      }
      return false;
    }
  }
  return true;
}

// I need to think about how to implement this function.
// third param: return val from the sesnors
void aerogel_sensor_module(WASMModuleInstance* module_inst,
    char* module_name,
    aerogel_sensor* sensor_list,
    uint32 len_sensor_list,
    aerogel_val* ret_val,
    uint32 len_ret_val,
    aerogel_actuator* actuator_list,
    uint32 len_actuator_list)
{
  for(uint32 i = 0 ; i < len_actuator_list; i++){
    char* name = actuator_list[i].actuator_name;
    char* tmp = device_spec;
    tmp = strstr(tmp, name);
    tmp = strstr(tmp, "concurrent_access:");
    tmp += strlen("concurrent_access:");
    char temp[20];
    memset(temp, 0, 20);
    int j = 0;
    while(tmp[j] != '\n'){
      temp[j] = tmp[j];
      ++j;
    }
    uint32 max_access = (uint32)atoi(temp);
    // printf("module_name is: %s, name is: %s\n", module_name, name);
    // printf("check_access_name: %d\n", check_access_name(module_name, name));
    // printf("check_access_energy: %d\n", check_access_energy(module_inst, name));
    // printf("check_access_concurrency: %d\n", check_access_concurrency(name, max_access));

    // if(check_access_name(module_name, name) ||
    //    check_access_energy(module_inst, name) ||
    //    check_access_concurrency(name, max_access))
    // {
    //   printf("Actuator access failed. Need further debugging.\n");
    //   continue;
    // }
    printf("actuator name: %s\n", name);
    if(check_access_name(module_name, name)){
      printf("Actuator %s access failed. Not allowed for module %s.\n", name, module_name);
      continue;
    }
    else if (check_access_energy(module_inst, name)) {
      printf("Actuator %s energy exceeds.\n", name);
      continue;
    }
    else if (check_access_concurrency(name, max_access)) {
      printf("Actuator %s max concurrent access achieved.\n", name);
      continue;
    }

    uint32 repetition = actuator_list[i].repetition;
    uint32 latency = actuator_list[i].latency;

    for(uint32 k = 0 ; k < repetition; k++){
      if (!strcmp(name, "speaker")) {
        set_speaker_data(actuator_list[i].val, latency);
      }
      else if (!strcmp(name, "door_motor")) {
        set_door_motor(actuator_list[i].val, latency);
      }
      else if (!strcmp(name, "propeller")) {
        set_propeller(actuator_list[i].val, latency);
      }
      else if (!strcmp(name, "home_camera_control")) {
        set_home_camera_control(actuator_list[i].val, latency);
      }
      else {
        printf("Error! Unknown actuator with name: %s\n", name);
        continue;
      }
    }
  }

  // printf("len_sensor_list: %u\n", len_sensor_list);
  for(uint32 i = 0 ; i < len_sensor_list; i++){
    char* name = sensor_list[i].sensor_name;
    char* tmp = device_spec;
    tmp = strstr(tmp, name);
    tmp = strstr(tmp, "concurrent_access:");
    tmp += strlen("concurrent_access:");
    char temp[20];
    int j = 0;
    while(tmp[j] != '\n'){
      temp[j] = tmp[j];
      ++j;
    }
    uint32 max_access = (uint32)atoi(temp);
    // printf("arrived here.\n");

    if(check_access_name(module_name, name)){
      printf("Sensor %s access failed. Not allowed for module %s.\n", name, module_name);
      continue;
    }
    else if (check_access_energy(module_inst, name)) {
      printf("Sensor %s energy exceeds.\n", name);
      continue;
    }
    else if (check_access_concurrency(name, max_access)) {
      printf("Sensor %s max concurrent access achieved.\n", name);
      continue;
    }

    uint32 freq = sensor_list[i].freq;

    // how many repetitions do we need? durating / latency.
    uint32 repetition = sensor_list[i].duration / (1000000/freq);
    ret_val[i].sensor_name = name;
    ret_val[i].value = wasm_runtime_malloc(sizeof(uint32) * repetition);
    ret_val[i].len_value = repetition;
    ret_val[i].num_ret_val = wasm_runtime_malloc(sizeof(uint32) * repetition);

    for(uint32 k = 0 ; k < repetition; k++){
      if(!strcmp(name, "imu")) {
        ret_val[i].value[k] = wasm_runtime_malloc(sizeof(uint32) * 3);
        ret_val[i].num_ret_val[k] = 3;
        get_imu((ret_val[i].value[k]), freq);
      }
      else if(!strcmp(name, "door_battery")) {
        ret_val[i].value[k] = wasm_runtime_malloc(sizeof(uint32) * 1);
        ret_val[i].num_ret_val[k] = 1;
        get_door_battery((ret_val[i].value[k]), freq);
      }
      else if(!strcmp(name, "camera") || !strcmp(name, "home_camera_image")) {
        ret_val[i].value[k] = wasm_runtime_malloc(sizeof(uint32) * 100);
        ret_val[i].num_ret_val[k] = 100;
        get_camera((ret_val[i].value[k]), freq);
      }
      else if(!strcmp(name, "motion")) {
        ret_val[i].value[k] = wasm_runtime_malloc(sizeof(uint32) * 1);
        ret_val[i].num_ret_val[k] = 1;
        get_motion((ret_val[i].value[k]), freq);
      }
      else if(!strcmp(name, "microphone")) {
        ret_val[i].value[k] = wasm_runtime_malloc(sizeof(uint32) * 10);
        ret_val[i].num_ret_val[k] = 10;
        get_microphone((ret_val[i].value[k]), freq);
      }
      else {
        printf("Error! Unknown sensor with name: %s\n", name);
        continue;
      }
    }
  }
}

void test_print_returned_sensor_val(aerogel_val* ret_val, uint32 len_ret_val) {
  for(uint32 i = 0; i < len_ret_val; i++) {
    aerogel_val val = ret_val[i];
    if(!val.sensor_name) {
      continue;
    }
    printf("Sensor name: %s\n", val.sensor_name);
    for(uint32 j = 0; j < val.len_value; j++) {
      printf("Round %u: ", j+1);
      for(uint32 k = 0; k < val.num_ret_val[j]; k++){
        printf("%u\t", val.value[j][k]);
      }
      printf("\n");
    }
    printf("\n");
  }
}

void test_aerogel_sensor_module(WASMModuleInstance *module_inst)
{
  aerogel_sensor sensor1;
  sensor1.sensor_name = "imu";
  sensor1.freq = 80;
  sensor1.duration = 100000;

  aerogel_sensor sensor2;
  sensor2.sensor_name = "motion";
  sensor2.freq = 100;
  sensor2.duration = 100000;

  aerogel_sensor* sensors = wasm_runtime_malloc(sizeof(aerogel_sensor) * 70);
  sensors[0] = sensor1;
  sensors[1] = sensor2;
  uint32 len_sensor_list = 2;

  aerogel_val* ret_val = wasm_runtime_malloc(sizeof(aerogel_val) * 2);
  uint32 len_ret_val = 2;
  memset(ret_val, 0, sizeof(aerogel_val) * 2);

  aerogel_actuator* actuator = wasm_runtime_malloc(sizeof(aerogel_actuator) * 1);
  uint32 len_actuator_list = 1;
  aerogel_actuator actuator1;
  actuator1.actuator_name = "propeller";
  uint32 tmp_value = 0;
  actuator1.val = &tmp_value;
  actuator1.len_val = 1;
  actuator1.repetition = 20;
  actuator1.latency = 20;
  actuator[0] = actuator1;

  aerogel_sensor_module(module_inst, module_inst->name, sensors, len_sensor_list,
      ret_val, len_ret_val, actuator, len_actuator_list);

  test_print_returned_sensor_val(ret_val, len_ret_val);
}

void
aerogel_sensor_interaction_native(
  wasm_exec_env_t exec_env,
  aerogel_sensor* sensor_list,
  uint32 len_sensor_list,
  aerogel_val* ret_val,
  uint32 len_ret_val)
{
  WASMModuleInstance *module_inst = (WASMModuleInstance*)exec_env->module_inst;
  aerogel_sensor_module(module_inst,
      module_inst->name, sensor_list, len_sensor_list,
      ret_val, len_ret_val,
      NULL, // Actuator list
      0 // Length of actuator list
  );
}

void aerogel_actuator_interaction_native(
  wasm_exec_env_t exec_env,
  aerogel_actuator* actuator_list,
  uint32 len_actuator_list)
{
  WASMModuleInstance *module_inst = (WASMModuleInstance*)exec_env->module_inst;
  // for(int i = 0 ; i < 3 ; i++) {
  //   printf("actuator name in native: %s\n", actuator_list[i].actuator_name);
  // }

  aerogel_sensor_module(module_inst,
      module_inst->name, //module name
      NULL, // Sensor list
      0, // Length of sensor list
      NULL, // Return value list
      0, // Length of return value list
      actuator_list,
      len_actuator_list);
}

void
test_wasm_runtime_native_print(wasm_exec_env_t exec_env) {
  uint32* imu_data = wasm_runtime_malloc(sizeof(uint32)*3);
  WASMModuleInstance *module_inst = (WASMModuleInstance*)exec_env->module_inst;
  get_imu_sensor(imu_data);
  test_aerogel_sensor_module(module_inst);

  uint32** image = wasm_runtime_malloc(sizeof(uint32*)*20);
  if(!image){
    printf("image init failed!\n");
  }
  for(int i = 0; i < 20; i++) {
    image[i] = wasm_runtime_malloc(sizeof(uint32)*20);
    if(!image[i]){
      printf("image[%d] failed!\n", i);
    }
  }

  get_camera_data(image);

  printf("\n\n\n\n");
  printf("==========start=========\n");
  printf("I have nothing to say\n");
  printf("print here!\n");
  printf("imu0: %u\n", imu_data[0]);
  printf("imu1: %u\n", imu_data[1]);
  printf("imu2: %u\n", imu_data[2]);
  printf("===========end==========\n");
  printf("\n\n\n\n");
}

bool
wasm_call_indirect(WASMExecEnv *exec_env,
                   uint32_t element_indices,
                   uint32_t argc, uint32_t argv[])
{
    WASMModuleInstance *module_inst = NULL;
    WASMTableInstance *table_inst = NULL;
    uint32_t function_indices = 0;
    WASMFunctionInstance *function_inst = NULL;

    module_inst =
        (WASMModuleInstance*)exec_env->module_inst;
    bh_assert(module_inst);

    table_inst = module_inst->default_table;
    if (!table_inst) {
        wasm_set_exception(module_inst, "there is no table");
        goto got_exception;
    }

    if (element_indices >= table_inst->cur_size) {
        wasm_set_exception(module_inst, "undefined element");
        goto got_exception;
    }

    function_indices = ((uint32_t*)table_inst->base_addr)[element_indices];
    if (function_indices == 0xFFFFFFFF) {
        wasm_set_exception(module_inst, "uninitialized element");
        goto got_exception;
    }

    function_inst = module_inst->functions + function_indices;
    wasm_interp_call_wasm(module_inst, exec_env, function_inst, argc, argv);
    return !wasm_get_exception(module_inst) ? true : false;

got_exception:
    return false;
}
