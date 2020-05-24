#ifndef PARSE_CONFIG_H

#define PARSE_CONFIG_H

enum pc_type {
	TYPE_STRING,
	TYPE_INT
};

typedef struct pc_keymap {
   char* name;
   void* target;
   enum  pc_type type;
   int   defined;
   int   mandatory;
} struct_pc_keymap;

int parse_config_buffer( struct_pc_keymap definition[], const char *buffer_to_barse);

#endif

