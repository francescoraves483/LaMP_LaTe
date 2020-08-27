#ifndef NAMED_ENUMS_H_INCLUDED
#define NAMED_ENUMS_H_INCLUDED

// Adapted from https://stackoverflow.com/questions/147267/easy-way-to-use-variables-of-enum-types-as-string-in-c
#define ENUM_VALUE(name,value) name value,
#define ENUM_TO_STR_CASE(name,value) case name: return #name;
#define ENUM_VALID_CASE(name,value) case name: return (int) name;
#define ENUM_STRCMP(name,value) if(!strcmp(str,#name)) return name;

#define NAMED_ENUM_DECLARE(enum_t_name,ENUM_DEFINITION_MACRO) \
	typedef enum { \
		ENUM_DEFINITION_MACRO(ENUM_VALUE) \
	} enum_t_name; \
	enum_t_name str_to_enum_##enum_t_name(const char *str); \
	const char *enum_to_str_##enum_t_name(enum_t_name enum_value); \
	int is_enum_valid_##enum_t_name(enum_t_name enum_value);

#define NAMED_ENUM_DEFINE_FCNS(enum_t_name,ENUM_DEFINITION_MACRO) \
	enum_t_name str_to_enum_##enum_t_name(const char *str) { \
		ENUM_DEFINITION_MACRO(ENUM_STRCMP) \
		return 0; \
	} \
	const char *enum_to_str_##enum_t_name(enum_t_name enum_value) { \
		switch(enum_value) { \
			ENUM_DEFINITION_MACRO(ENUM_TO_STR_CASE) \
			default: \
				return "(unknown)"; \
		} \
	} \
	int is_enum_valid_##enum_t_name(enum_t_name enum_value) { \
		switch(enum_value) { \
			ENUM_DEFINITION_MACRO(ENUM_VALID_CASE) \
			default: \
				return 0; \
		} \
	} 
#endif